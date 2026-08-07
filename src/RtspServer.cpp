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

RtspServer::RtspServer(const ServerConfig& serverConfig, const CameraConfig& cameraConfig)
	: _path(serverConfig.path)
	, _address(serverConfig.address)
	, _port(serverConfig.port)
	, _cameraConfig(cameraConfig)
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
	// exclusive, so without this a second consumer — the go2rtc WebRTC gateway, or a
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

std::optional<VideoDevice> RtspServer::select_device(const std::vector<VideoDevice>& devices)
{
	if (devices.empty()) {
		return std::nullopt;
	}

	// An explicit device from config/UI wins, as long as it's actually connected.
	if (!_cameraConfig.device.empty()) {
		auto it = std::find_if(devices.begin(), devices.end(), [&](const VideoDevice & d) {
			return d.path == _cameraConfig.device;
		});

		if (it != devices.end()) {
			return *it;
		}

		std::cout << "Configured camera " << _cameraConfig.device
			  << " not found among connected devices; falling back to auto-select" << std::endl;
	}

	// Auto-select: the lowest-numbered capture node (enumerate_video_devices() returns
	// them sorted ascending). On Jetson a connected CSI sensor is /dev/video0, so it
	// naturally wins over a USB camera that enumerates later.
	return devices.front();
}

std::string RtspServer::get_pipeline(Platform platform)
{
	const std::vector<VideoDevice> devices = enumerate_video_devices();

	for (const auto& d : devices) {
		std::cout << "Detected camera " << d.path << " [" << camera_type_name(d.type)
			  << "] driver=" << d.driver << " card=\"" << d.name << "\"" << std::endl;
	}

	const std::optional<VideoDevice> selected = select_device(devices);

	if (selected) {
		std::cout << "Selected camera " << selected->path << " (" << camera_type_name(selected->type)
			  << ")" << std::endl;
	}

	// Routing is platform-primary: each platform owns the source element for its native
	// CSI stack (nvarguscamerasrc / libcamerasrc), and a USB/UVC camera goes through
	// v4l2src on any platform.
	switch (platform) {
	case Platform::Jetson:

		// A connected USB webcam streams via v4l2src; otherwise use the Argus CSI source
		// (also the no-camera default, whose mode probe waits for the sensor to appear).
		if (selected && selected->type == CameraType::Usb) {
			return create_usb_pipeline(selected->path);
		}

		return create_jetson_pipeline(csi_sensor_id(devices, selected));

	case Platform::Pi:

		// Only a UVC webcam uses v4l2src; CSI sensors (and their raw Bayer /dev/video
		// nodes, which v4l2src can't consume) go through libcamerasrc.
		if (selected && selected->type == CameraType::Usb) {
			return create_usb_pipeline(selected->path);
		}

		return create_pi_pipeline();

	case Platform::Ubuntu:

		// Desktop has no CSI ISP source: stream any real capture device via v4l2src,
		// else fall back to the test pattern so the stream still comes up.
		if (selected) {
			return create_usb_pipeline(selected->path);
		}

		return create_ubuntu_pipeline();
	}

	return create_ubuntu_pipeline();
}

int RtspServer::csi_sensor_id(const std::vector<VideoDevice>& devices, const std::optional<VideoDevice>& selected)
{
	// nvarguscamerasrc sensor-id is the CSI sensor's rank, not the /dev/video number, so
	// count the CSI nodes that sort before the selected one. Single-camera rigs (and the
	// no-selection default) stay sensor-id 0.
	if (!selected || selected->type != CameraType::Csi) {
		return 0;
	}

	int sensorId = 0;

	for (const auto& d : devices) {
		if (d.type == CameraType::Csi && d.index < selected->index) {
			++sensorId;
		}
	}

	return sensorId;
}

std::string RtspServer::create_jetson_pipeline(int sensorId)
{
	std::stringstream ss;

	// Clamp the requested settings to the sensor's real capabilities. Requesting a
	// framerate the sensor can't deliver makes nvarguscamerasrc fail to acquire the
	// camera ("Frame Rate specified is greater than supported"), which then cascades
	// into "No cameras available" under the systemd restart loop.

	// Retry the probe until the sensor reports its modes instead of falling back.
	// Right after a restart the previous process's nvarguscamerasrc can still hold
	// /dev/video0, so v4l2-ctl sees EBUSY and prints nothing — a transient. Falling
	// back would mean streaming at the requested framerate the sensor may not
	// support, so it is better to wait a moment for a real answer.
	std::vector<SensorMode> modes = probe_sensor_modes(sensorId);

	// Rate-limit the log (first few, then ~every 30s): an absent or never-free camera
	// would otherwise write one journal line a second forever — the spam that made the
	// log look stuck on this message.
	for (int attempt = 1; modes.empty(); ++attempt) {
		if (attempt <= 5 || attempt % 30 == 0) {
			std::cout << "No sensor modes detected (attempt " << attempt
				  << "; is a camera connected, v4l-utils installed, and the camera free?); "
				  << "retrying every 1s..." << std::endl;
		}

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

std::string RtspServer::create_usb_pipeline(const std::string& device)
{
	std::stringstream ss;

	int width = _cameraConfig.getWidth();
	int height = _cameraConfig.getHeight();
	// Cap framerate at 30fps, matching the other software-encoded pipelines.
	int framerate = std::min(_cameraConfig.framerate, 30);

	// Output size after rotation: 90/270 swap width and height.
	int outWidth = width;
	int outHeight = height;

	if (_cameraConfig.rotation == CameraRotation::ROTATE_90 || _cameraConfig.rotation == CameraRotation::ROTATE_270) {
		std::swap(outWidth, outHeight);
	}

	// decodebin makes this work across the two ways USB cameras expose video: cheap
	// webcams stream MJPEG (decodebin inserts jpegdec) while others emit raw YUYV
	// (decodebin passes it through). videoscale/videorate then force a deterministic
	// output size and rate regardless of which native mode the camera negotiated.
	ss << "( v4l2src device=" << device << " ! "
	   << "decodebin ! "
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

	ss << "videoscale ! videorate ! "
	   << "video/x-raw,format=I420,width=" << outWidth
	   << ",height=" << outHeight
	   << ",framerate=" << framerate << "/1 ! "
	   << "x264enc key-int-max=" << framerate << " bitrate=" << _cameraConfig.bitrate
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
