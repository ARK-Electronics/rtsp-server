#include "RtspServer.hpp"
#include <iostream>
#include <fstream>

// namespace gst c lib for readability
namespace gst
{
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
}

// Original pipeline
const char* PI_PIPELINE = "( libcamerasrc ! videoconvert ! x264enc key-int-max=15 bitrate=2500 tune=zerolatency speed-preset=ultrafast ! \
							video/x-h264,stream-format=byte-stream ! rtph264pay config-interval=1 name=pay0 pt=96 )";

// const char* PI_PIPELINE = "( libcamerasrc ! video/x-raw,framerate=30/1,width=1920,height=1080 ! videoscale ! video/x-raw,width=1280,height=720 ! \
// 							videoconvert ! x264enc bitrate=2500 tune=zerolatency speed-preset=ultrafast key-int-max=30 ! \
//                             video/x-h264,stream-format=byte-stream ! h264parse ! rtph264pay config-interval=1 name=pay0 pt=96 )";

// const char* PI_PIPELINE = "( libcamerasrc ! video/x-raw,framerate=30/1,width=1280,height=720 ! videoconvert ! \
// 							v4l2h264enc extra-controls=\"controls, h264_profile=4, video_bitrate=620000\" ! 'video/x-h264, profile=high, level=(string)4' ! \
// 							h264parse ! rtph264pay config-interval=1 pt=96 )";

// thisone doesn't crash...
// gst-launch-1.0 -e libcamerasrc ! video/x-raw,width=1280, height=720, framerate=15/1 ! \
// 				v4l2h264enc extra-controls="controls, h264_profile=4, video_bitrate=620000" ! 'video/x-h264, profile=high, level=(string)4' ! \
// 				h264parse ! rtph264pay config-interval=1 pt=96 ! udpsink host=192.168.68.78 port=5600

// TODO: figure out if we need to add the plugin path
// gst-launch-1.0 --gst-plugin-path=install/gst_bridge/lib/gst_bridge/ rosimagesrc ros-topic=/camera/color/image_raw \
// ! queue max-size-buffers=1 ! video/x-raw,format=RGB ! videoconvert ! x264enc bitrate=2100 tune=zerolatency speed-preset=ultrafast \
// ! video/x-h264,stream-format=byte-stream ! rtph264pay config-interval=1 pt=96 ! udpsink host=$TARGET_IP port=$TARGET_PORT sync=false
const char* JETSON_PIPELINE = "( rosimagesrc ros-topic=/camera/color/image_raw ! videoconvert ! x264enc bitrate=2000 tune=zerolatency speed-preset=ultrafast ! \
							video/x-h264,stream-format=byte-stream ! rtph264pay config-interval=1 name=pay0 pt=96 )";

const char* UB_PIPELINE = "( videotestsrc pattern=ball ! videoconvert ! x264enc bitrate=2000 tune=zerolatency speed-preset=ultrafast ! \
							video/x-h264,stream-format=byte-stream ! rtph264pay config-interval=1 name=pay0 pt=96 )";

RtspServer::RtspServer(const std::string& address, const std::string& port)
	: _address(address), _port(port)
{}

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

	gst::gst_rtsp_mount_points_add_factory(mounts, "/fpv", factory);
	gst::g_object_unref(mounts);

	gst::gst_rtsp_server_attach(server, NULL);

	std::cout << "Stream ready at rtsp://" << _address << ":" << _port << "/fpv" << std::endl;
	gst::g_main_loop_run(gst::g_main_loop_new(NULL, FALSE));
}

std::string RtspServer::get_pipeline(Platform platform)
{
	switch (platform) {
	case Platform::Ubuntu:
		return UB_PIPELINE;

	case Platform::Pi:
		return PI_PIPELINE;

	case Platform::Jetson:
		return JETSON_PIPELINE;
	}
}

RtspServer::Platform RtspServer::detect_platform()
{
	std::ifstream cpuinfo("/proc/cpuinfo");

	if (!cpuinfo.is_open()) {
		return Platform::Ubuntu; // Assume Ubuntu Desktop if file can't be opened
	}

	std::string line;

	while (getline(cpuinfo, line)) {
		if (line.find("Raspberry Pi") != std::string::npos) {
			return Platform::Pi;
		}

		if (line.find("NVIDIA Jetson") != std::string::npos) {
			return Platform::Jetson; // Define this pipeline similarly to PI_PIPELINE and UB_PIPELINE
		}
	}

	// Default to Ubuntu Desktop pipeline if no specific identifiers are found
	return Platform::Ubuntu;
}
