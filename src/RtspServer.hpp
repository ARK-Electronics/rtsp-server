#pragma once

#include "Config.hpp"
#include "CameraProbe.hpp"
#include <optional>
#include <string>
#include <vector>

class RtspServer
{
public:
	RtspServer(const ServerConfig& serverConfig, const CameraConfig& cameraConfig);
	void run();

private:
	enum class Platform {
		Ubuntu,
		Pi,
		Jetson,
	};

	Platform detect_platform();
	std::string get_pipeline(Platform platform);

	// Resolve the configured/auto-selected camera against what's actually connected.
	// Returns nullopt when no capture device is present, so the caller can fall back
	// to the platform default pipeline.
	std::optional<VideoDevice> select_device(const std::vector<VideoDevice>& devices);

	// Map the selected CSI device to its nvarguscamerasrc sensor-id (its rank among CSI
	// nodes). Returns 0 when nothing CSI is selected.
	static int csi_sensor_id(const std::vector<VideoDevice>& devices, const std::optional<VideoDevice>& selected);

	std::string create_jetson_pipeline(int sensorId);
	std::string create_pi_pipeline();
	std::string create_usb_pipeline(const std::string& device);
	std::string create_ubuntu_pipeline();

	// Server configuration
	std::string _path;
	std::string _address;
	std::string _port;

	// Camera configuration
	CameraConfig _cameraConfig;
};
