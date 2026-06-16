#include "RtspServer.hpp"
#include "CameraProbe.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <csignal>
#include <filesystem>
#include <system_error>
#include <ctime>
#include <sys/stat.h>

// namespace gst c lib for readability
namespace gst
{
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <glib-unix.h>
}

namespace
{

// Installed for SIGINT/SIGTERM via g_unix_signal_add, which dispatches it from the
// main loop (not from async-signal context), so quitting the loop here is safe.
// Returning G_SOURCE_REMOVE uninstalls the handler, so a second signal falls through
// to the default disposition and still force-kills us if teardown ever hangs.
gst::gboolean quit_main_loop(gst::gpointer loop)
{
	std::cout << std::endl << "Shutting down RTSP server..." << std::endl;
	gst::g_main_loop_quit(static_cast<gst::GMainLoop*>(loop));
	return G_SOURCE_REMOVE;
}

// Collect every connected client (REF adds it to the filter's result list) so each
// can be closed explicitly on shutdown.
gst::GstRTSPFilterResult ref_client(gst::GstRTSPServer*, gst::GstRTSPClient*, gst::gpointer)
{
	return gst::GST_RTSP_FILTER_REF;
}

} // namespace

// HLS segments are written to tmpfs (RAM) so the segment churn never wears the SD
// card; nginx serves this directory at /video/hls/. Default umask leaves the files
// world-readable so nginx (www-data) can serve them.
static constexpr const char* HLS_OUTPUT_DIR = "/dev/shm/ark-rtsp-hls";

// Viewer-presence lease. The web UI's gateway touches this file while the Video page
// is open (POST /api/video/keepalive) and removes it when the page closes. We only
// stat() its mtime — no read permission needed — and it lives beside, not inside,
// HLS_OUTPUT_DIR so nginx never serves it.
static constexpr const char* HLS_LEASE_FILE = "/dev/shm/ark-rtsp-hls.lease";
// The lease counts as live if touched within this window; the page heartbeats every
// few seconds, so a couple of missed beats (tab hidden, client gone) lets HLS stop.
static constexpr int HLS_LEASE_TTL_SECONDS = 6;
// How often we reconcile the pipeline against the lease.
static constexpr int HLS_POLL_SECONDS = 1;
// After a pipeline error/EOS (camera unavailable, or RTSP media not ready yet) wait this
// long before rebuilding, so a viewer watching a broken camera doesn't spam the log at
// the poll rate.
static constexpr int HLS_ERROR_BACKOFF_SECONDS = 3;

namespace gst
{
namespace
{

// On-demand loopback HLS pipeline. It is an ordinary RTSP client of our own server,
// so the camera stays owned solely by the RTSP media factory (one exclusive source,
// shared across clients) and this branch only depays + remuxes to MPEG-TS — no
// decode/re-encode. It runs only while a viewer lease is fresh, so the camera and
// encoder spin up when someone opens the Video page and release when they leave. The
// context outlives run() (which never returns), so it is intentionally leaked.
struct HlsContext {
	std::string launch;
	GstElement* pipeline;   // null while idle (no viewers) or between reconnects
	bool parseFailed;       // pipeline could not be built (missing element); stop trying
	time_t retryAfter;      // earliest time to rebuild after an error (backoff)
};

// Fresh if the lease file exists and was touched within the TTL. stat() needs only
// search permission on /dev/shm (world-traversable), not read on the file, and
// st_mtime shares time()'s epoch — so no permission coupling or clock conversion.
bool lease_is_fresh()
{
	struct stat st;

	if (::stat(HLS_LEASE_FILE, &st) != 0) {
		return false;
	}

	return std::time(nullptr) - st.st_mtime <= HLS_LEASE_TTL_SECONDS;
}

gboolean hls_bus_cb(GstBus* bus, GstMessage* msg, gpointer user_data);

// Tear the pipeline down, releasing our loopback RTSP client so the shared camera
// media can stop once no other clients remain. The caller has already removed the bus
// watch, or is the bus callback itself returning G_SOURCE_REMOVE.
void hls_drop_pipeline(HlsContext* ctx)
{
	if (ctx->pipeline) {
		gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
		gst_object_unref(ctx->pipeline);
		ctx->pipeline = nullptr;
	}
}

void hls_start_pipeline(HlsContext* ctx)
{
	GError* err = nullptr;
	ctx->pipeline = gst_parse_launch(ctx->launch.c_str(), &err);

	if (!ctx->pipeline) {
		// A parse failure means a missing element (e.g. hlssink2) — not self-correcting,
		// so latch it off and log once rather than rebuilding on every poll.
		g_printerr("HLS: could not build pipeline: %s\n", err ? err->message : "unknown error");
		g_clear_error(&err);
		ctx->parseFailed = true;
		return;
	}

	g_clear_error(&err);

	GstBus* bus = gst_element_get_bus(ctx->pipeline);
	gst_bus_add_watch(bus, hls_bus_cb, ctx);
	gst_object_unref(bus);

	gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING);
	std::cout << "HLS: viewer present, restreaming to /video/hls/stream.m3u8" << std::endl;
}

// Explicit stop (the viewer left). Unlike the bus callback we are not inside the watch,
// so remove it before dropping the pipeline.
void hls_stop_pipeline(HlsContext* ctx)
{
	if (!ctx->pipeline) {
		return;
	}

	GstBus* bus = gst_element_get_bus(ctx->pipeline);
	gst_bus_remove_watch(bus);
	gst_object_unref(bus);
	hls_drop_pipeline(ctx);
	std::cout << "HLS: no viewers, camera released" << std::endl;
}

gboolean hls_bus_cb(GstBus* /*bus*/, GstMessage* msg, gpointer user_data)
{
	HlsContext* ctx = static_cast<HlsContext*>(user_data);

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR: {
		GError* err = nullptr;
		gchar* dbg = nullptr;
		gst_message_parse_error(msg, &err, &dbg);
		g_printerr("HLS: pipeline error: %s\n", err ? err->message : "unknown error");
		g_clear_error(&err);
		g_free(dbg);
		[[fallthrough]];
	}

	case GST_MESSAGE_EOS:
		// Usually the RTSP server was not accepting yet, or the shared media was torn
		// down. Drop the pipeline; the lease poll rebuilds it after a short backoff if a
		// viewer is still present.
		hls_drop_pipeline(ctx);
		ctx->retryAfter = std::time(nullptr) + HLS_ERROR_BACKOFF_SECONDS;
		return G_SOURCE_REMOVE; // a rebuilt pipeline installs its own watch

	default:
		return G_SOURCE_CONTINUE;
	}
}

// Reconcile the pipeline against the viewer lease once per tick: start when a viewer
// appears, stop when the last one leaves.
gboolean hls_lease_poll_cb(gpointer user_data)
{
	HlsContext* ctx = static_cast<HlsContext*>(user_data);
	const bool viewer = lease_is_fresh();

	if (viewer && !ctx->pipeline && !ctx->parseFailed && std::time(nullptr) >= ctx->retryAfter) {
		hls_start_pipeline(ctx);

	} else if (!viewer && ctx->pipeline) {
		hls_stop_pipeline(ctx);
	}

	return G_SOURCE_CONTINUE;
}

void start_hls_on_demand(const std::string& launch)
{
	HlsContext* ctx = new HlsContext{launch, nullptr, false, 0};
	gst::g_timeout_add_seconds(HLS_POLL_SECONDS, hls_lease_poll_cb, ctx);
}

} // namespace
} // namespace gst

RtspServer::RtspServer(const ServerConfig& serverConfig, const CameraConfig& cameraConfig, const HlsConfig& hlsConfig)
	: _path(serverConfig.path)
	, _address(serverConfig.address)
	, _port(serverConfig.port)
	, _cameraConfig(cameraConfig)
	, _hlsConfig(hlsConfig)
{
	std::cout << "Camera config: "
		  << _cameraConfig.getWidth() << "x" << _cameraConfig.getHeight()
		  << " @ " << _cameraConfig.framerate << "fps, bitrate: "
		  << _cameraConfig.bitrate << "kbps, rotation: "
		  << _cameraConfig.getRotationDegrees() << "°" << std::endl;
}

void RtspServer::run()
{
	gst::GstRTSPServer* server;
	gst::GstRTSPMountPoints* mounts;
	gst::GstRTSPMediaFactory* factory;
	gst::GMainLoop* loop;
	std::string pipeline;

	gst::gst_init(nullptr, nullptr);

	server = gst::gst_rtsp_server_new();
	gst::gst_rtsp_server_set_address(server, _address.c_str());
	gst::gst_rtsp_server_set_service(server, _port.c_str());

	mounts = gst::gst_rtsp_server_get_mount_points(server);
	factory = gst::gst_rtsp_media_factory_new();

	// Build pipeline string
	pipeline = get_pipeline(detect_platform());

	gst::gst_rtsp_media_factory_set_launch(factory, pipeline.c_str());

	// Share one media (camera pipeline) across all clients. The camera source is
	// exclusive, so without this a second consumer — the HLS loopback below, or a
	// second RTSP client — would fail to open it.
	gst::gst_rtsp_media_factory_set_shared(factory, TRUE);

	gst::gst_rtsp_mount_points_add_factory(mounts, std::string("/" + _path).c_str(), factory);
	gst::g_object_unref(mounts);

	// TODO: we need to handle bad disconnect events gracefully (client loses network connection and doesn't end session)
	gst::gst_rtsp_server_attach(server, NULL);

	// Quit the loop on SIGINT/SIGTERM so a systemd stop/restart triggers a graceful
	// teardown below instead of the process being killed mid-stream.
	loop = gst::g_main_loop_new(NULL, FALSE);
	gst::g_unix_signal_add(SIGINT, quit_main_loop, loop);
	gst::g_unix_signal_add(SIGTERM, quit_main_loop, loop);

	std::cout << "Stream ready at rtsp://" << _address << ":" << _port << "/" << _path << std::endl << std::endl;

	if (_hlsConfig.enabled) {
		std::error_code ec;
		// Start each run from a clean directory so stale segments from a previous run
		// can't linger in the playlist. The pipeline itself starts on demand, once the
		// Video page signals a viewer via the lease (see start_hls_on_demand).
		std::filesystem::remove_all(HLS_OUTPUT_DIR, ec);
		std::filesystem::create_directories(HLS_OUTPUT_DIR, ec);

		if (ec) {
			std::cerr << "HLS: could not create " << HLS_OUTPUT_DIR << ": " << ec.message()
				  << " — HLS disabled" << std::endl;

		} else {
			gst::start_hls_on_demand(get_hls_launch());
			std::cout << "HLS available on demand at /video/hls/stream.m3u8 "
				  << "(streams only while the Video page is open)" << std::endl;
		}
	}

	gst::g_main_loop_run(loop);

	// Reached only after a shutdown signal. Close each connected client so its media
	// pipeline (nvarguscamerasrc) is set to NULL and the Argus camera session is
	// released cleanly — otherwise nvargus-daemon logs "(Argus) Error EndOfFile" when
	// the socket drops as the process exits.
	gst::GList* clients = gst::gst_rtsp_server_client_filter(server, ref_client, nullptr);

	for (gst::GList* l = clients; l != nullptr; l = l->next) {
		gst::gst_rtsp_client_close(static_cast<gst::GstRTSPClient*>(l->data));
		gst::g_object_unref(l->data);
	}

	gst::g_list_free(clients);
	gst::g_main_loop_unref(loop);
	gst::g_object_unref(server);
}

std::string RtspServer::get_pipeline(Platform platform)
{
	switch (platform) {
	case Platform::Ubuntu:
		return create_ubuntu_pipeline();

	case Platform::Pi:
		return create_pi_pipeline();

	case Platform::Jetson:
		return create_jetson_pipeline();
	}

	return create_ubuntu_pipeline();
}

std::string RtspServer::create_jetson_pipeline()
{
	std::stringstream ss;

	// Clamp the requested settings to the sensor's real capabilities. Requesting a
	// framerate the sensor can't deliver makes nvarguscamerasrc fail to acquire the
	// camera ("Frame Rate specified is greater than supported"), which then cascades
	// into "No cameras available" under the systemd restart loop.
	const int sensorId = 0;

	// Retry the probe until the sensor reports its modes instead of falling back.
	// Right after a restart the previous process's nvarguscamerasrc can still hold
	// /dev/video0, so v4l2-ctl sees EBUSY and prints nothing — a transient. Falling
	// back would mean streaming at the requested framerate the sensor may not
	// support, so it is better to wait a moment for a real answer.
	std::vector<SensorMode> modes = probe_sensor_modes(sensorId);

	for (int attempt = 1; modes.empty(); ++attempt) {
		std::cout << "No sensor modes detected (attempt " << attempt
			  << "; is v4l-utils installed and the camera free?); retrying in 1s..." << std::endl;
		std::this_thread::sleep_for(std::chrono::seconds(1));
		modes = probe_sensor_modes(sensorId);
	}

	for (const auto& m : modes) {
		std::cout << "Detected sensor mode: " << m.width << "x" << m.height
			  << " @ " << m.max_fps << "fps" << std::endl;
	}

	const StreamProfile profile = select_stream_profile(
					      modes, _cameraConfig.getWidth(), _cameraConfig.getHeight(), _cameraConfig.framerate);

	std::cout << "Requested " << _cameraConfig.getWidth() << "x" << _cameraConfig.getHeight()
		  << " @ " << _cameraConfig.framerate << "fps; streaming "
		  << profile.width << "x" << profile.height << " @ " << profile.framerate << "fps" << std::endl;

	// Apply rotation using nvvidconv with flip-method
	// 0 = no rotation
	// 1 = 90 degrees (rotate clockwise)
	// 2 = 180 degrees
	// 3 = 270 degrees (rotate counter-clockwise)
	int flipMethod = 0;

	switch (_cameraConfig.rotation) {
	case CameraRotation::ROTATE_90:  flipMethod = 1; break;

	case CameraRotation::ROTATE_180: flipMethod = 2; break;

	case CameraRotation::ROTATE_270: flipMethod = 3; break;

	default: flipMethod = 0; break;
	}

	// Output size after rotation: 90/270 swap width and height.
	int outWidth = profile.width;
	int outHeight = profile.height;

	if (_cameraConfig.rotation == CameraRotation::ROTATE_90 || _cameraConfig.rotation == CameraRotation::ROTATE_270) {
		std::swap(outWidth, outHeight);
	}

	// nvarguscamerasrc selects the sensor mode from these caps; the framerate must be
	// one the sensor advertises. nvvidconv then enforces the final output size, so the
	// result is deterministic even if argus emits the full sensor resolution.
	ss << "( nvarguscamerasrc sensor-id=" << sensorId << " ! "
	   << "video/x-raw(memory:NVMM),width=" << profile.width
	   << ",height=" << profile.height
	   << ",framerate=" << profile.framerate << "/1 ! "
	   << "nvvidconv flip-method=" << flipMethod << " ! "
	   << "video/x-raw,width=" << outWidth << ",height=" << outHeight << ",format=I420 ! "
	   << "x264enc key-int-max=" << profile.framerate << " bitrate=" << _cameraConfig.bitrate
	   << " tune=zerolatency speed-preset=ultrafast ! "
	   << "video/x-h264,stream-format=byte-stream,profile=baseline ! "
	   << "rtph264pay config-interval=1 mtu=1400 name=pay0 pt=96 )";

	std::cout << "Using pipeline: " << ss.str() << std::endl;
	return ss.str();
}

std::string RtspServer::create_pi_pipeline()
{
	std::stringstream ss;

	// Base dimensions
	int width = _cameraConfig.getWidth();
	int height = _cameraConfig.getHeight();
	// Cap framerate at 30fps
	int framerate = std::min(_cameraConfig.framerate, 30);

	// Start building the pipeline.
	// format=NV12 is required: without an explicit format, libcamerasrc negotiates its
	// src pad to the sensor's RAW Bayer stream (e.g. imx708 -> SBGGR16), which videoconvert
	// cannot consume -> "not-negotiated". NV12 is the PiSP ISP's native processed output.
	ss << "( libcamerasrc ! "
	   << "video/x-raw,format=NV12,width=" << width
	   << ",height=" << height
	   << ",framerate=" << framerate << "/1 ! "
	   << "videoconvert ! ";

	// Add rotation using videoflip element
	switch (_cameraConfig.rotation) {
	case CameraRotation::ROTATE_90:
		ss << "videoflip method=clockwise ! ";
		break;

	case CameraRotation::ROTATE_180:
		ss << "videoflip method=rotate-180 ! ";
		break;

	case CameraRotation::ROTATE_270:
		ss << "videoflip method=counterclockwise ! ";
		break;

	default:
		// No rotation needed
		break;
	}

	// Complete the pipeline with encoder and payloader
	ss << "video/x-raw,format=I420 ! "
	   << "x264enc key-int-max=30 bitrate=" << _cameraConfig.bitrate
	   << " tune=zerolatency speed-preset=ultrafast ! "
	   << "video/x-h264,stream-format=byte-stream,profile=baseline ! "
	   << "rtph264pay config-interval=1 mtu=1400 name=pay0 pt=96 )";

	std::cout << "Using pipeline: " << ss.str() << std::endl;
	return ss.str();
}

std::string RtspServer::create_ubuntu_pipeline()
{
	std::stringstream ss;

	// Base dimensions
	int width = _cameraConfig.getWidth();
	int height = _cameraConfig.getHeight();
	// Cap framerate at 30fps
	int framerate = std::min(_cameraConfig.framerate, 30);

	// Start with test source
	ss << "( videotestsrc pattern=ball ! "
	   << "video/x-raw,width=" << width
	   << ",height=" << height
	   << ",framerate=" << framerate << "/1 ! "
	   << "videoconvert ! ";

	// Add rotation using videoflip element
	switch (_cameraConfig.rotation) {
	case CameraRotation::ROTATE_90:
		ss << "videoflip method=clockwise ! ";
		break;

	case CameraRotation::ROTATE_180:
		ss << "videoflip method=rotate-180 ! ";
		break;

	case CameraRotation::ROTATE_270:
		ss << "videoflip method=counterclockwise ! ";
		break;

	default:
		// No rotation needed
		break;
	}

	// Complete the pipeline with encoder and payloader
	ss << "video/x-raw,format=I420 ! "
	   << "x264enc bitrate=" << _cameraConfig.bitrate
	   << " tune=zerolatency speed-preset=ultrafast ! "
	   << "video/x-h264,stream-format=byte-stream,profile=baseline ! "
	   << "rtph264pay config-interval=1 mtu=1400 name=pay0 pt=96 )";

	std::cout << "Using pipeline: " << ss.str() << std::endl;
	return ss.str();
}

RtspServer::Platform RtspServer::detect_platform()
{
	std::array<char, 128> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen("uname -a", "r"), pclose);

	if (!pipe) {
		return Platform::Ubuntu; // Assume Ubuntu Desktop if command can't be executed
	}

	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}

	std::cout << "result: " << result << std::endl;

	if (result.find("tegra") != std::string::npos) {
		std::cout << "Platform: Jetson" << std::endl;
		return Platform::Jetson;

	} else if (result.find("rpi") != std::string::npos || result.find("raspberrypi") != std::string::npos) {
		std::cout << "Platform: Raspberry Pi" << std::endl;
		return Platform::Pi;
	}

	std::cout << "Platform: Ubuntu Desktop" << std::endl;
	return Platform::Ubuntu;
}

std::string RtspServer::get_hls_launch()
{
	// Consume our own RTSP stream over loopback and remux H.264 into HLS segments.
	// No decode/re-encode: rtph264depay + h264parse hand the elementary stream to
	// hlssink2, which writes the .ts segments and the .m3u8 playlist on tmpfs.
	std::stringstream ss;

	ss << "rtspsrc location=rtsp://127.0.0.1:" << _port << "/" << _path
	   << " latency=0 protocols=tcp ! "
	   << "rtph264depay ! h264parse ! "
	   << "hlssink2 target-duration=" << _hlsConfig.segmentDuration
	   << " playlist-length=" << _hlsConfig.playlistLength
	   << " max-files=" << (_hlsConfig.playlistLength + 2)
	   << " location=" << HLS_OUTPUT_DIR << "/segment%05d.ts"
	   << " playlist-location=" << HLS_OUTPUT_DIR << "/stream.m3u8";

	std::cout << "Using HLS pipeline: " << ss.str() << std::endl;
	return ss.str();
}
