#pragma once

#include "Config.hpp"
#include <string>

class RtspServer
{
public:
	RtspServer(const ServerConfig& serverConfig, const CameraConfig& cameraConfig, const HlsConfig& hlsConfig);
	void run();

private:
	enum class Platform {
		Ubuntu,
		Pi,
		Jetson,
	};

	Platform detect_platform();
	std::string get_pipeline(Platform platform);
	std::string create_jetson_pipeline();
	std::string create_pi_pipeline();
	std::string create_ubuntu_pipeline();

	// Loopback restream of the RTSP stream into HLS segments on tmpfs
	std::string get_hls_launch();

	// Server configuration
	std::string _path;
	std::string _address;
	std::string _port;

	// Camera configuration
	CameraConfig _cameraConfig;

	// HLS restream configuration
	HlsConfig _hlsConfig;
};
