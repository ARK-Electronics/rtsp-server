#include "RtspServer.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <memory>
#include <sstream>
#include <algorithm>

// namespace gst c lib for readability
namespace gst
{
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
}

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

	gst::gst_rtsp_mount_points_add_factory(mounts, std::string("/" + _path).c_str(), factory);
	gst::g_object_unref(mounts);

	// TODO: we need to handle bad disconnect events gracefully (client loses network connection and doesn't end session)
	gst::gst_rtsp_server_attach(server, NULL);

	std::cout << "Stream ready at rtsp://" << _address << ":" << _port << "/" << _path << std::endl << std::endl;
	gst::g_main_loop_run(gst::g_main_loop_new(NULL, FALSE));
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

	// Base dimensions
	int width = _cameraConfig.getWidth();
	int height = _cameraConfig.getHeight();
	// Cap framerate at 30fps
	int framerate = std::min(_cameraConfig.framerate, 30);

	// Start building the pipeline
	ss << "( nvarguscamerasrc sensor-id=0 ! "
	   << "video/x-raw(memory:NVMM),width=" << width
	   << ",height=" << height
	   << ",framerate=" << framerate << "/1 ! ";

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

	// Apply the rotation
	ss << "nvvidconv flip-method=" << flipMethod << " ! "
	   << "video/x-raw,format=I420 ! ";

	// Complete the pipeline with encoder and payloader
	ss << "x264enc key-int-max=30 bitrate=" << _cameraConfig.bitrate
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
