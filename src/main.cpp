#include "RtspServer.hpp"
#include "Config.hpp"
#include <iostream>
#include <filesystem>
#include <toml.hpp>

int main()
{
	// Initialize default configuration
	AppConfig config = {
		.server = {
			.path = "camera1",
			.address = "0.0.0.0",
			.port = "5600"
		},
		.camera = {
			.resolution = ResolutionPreset::R640x480,
			.framerate = 15,
			.bitrate = 2000,
			.rotation = CameraRotation::ROTATE_0
		}
	};

	// Two-tier config lookup: user override > deb-installed default
	const std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
	const auto user_config = std::filesystem::path(home) / ".config/ark/rtsp-server/config.toml";
	const auto default_config = std::filesystem::path("/opt/ark/share/rtsp-server/config.toml");
	const auto config_path = std::filesystem::exists(user_config) ? user_config : default_config;

	try {
		toml::table tomlConfig = toml::parse_file(config_path.string());

		// RTSP server config
		if (auto rtsp = tomlConfig["rtsp"].as_table()) {
			if (rtsp->contains("url")) {
				config.server.path = (*rtsp)["url"].value_or("camera1");
			}

			if (rtsp->contains("address")) {
				config.server.address = (*rtsp)["address"].value_or("0.0.0.0");
			}

			if (rtsp->contains("port")) {
				int port = (*rtsp)["port"].value_or(5600);
				config.server.port = std::to_string(port);
			}
		}

		// Camera config
		if (auto camera = tomlConfig["camera"].as_table()) {
			if (camera->contains("resolution")) {
				std::string resStr = (*camera)["resolution"].value_or("640x480");
				config.camera.resolution = stringToResolution(resStr);
				std::cout << "Resolution set to: " << resolutionToString(config.camera.resolution) << std::endl;
			}

			if (camera->contains("framerate")) {
				config.camera.framerate = (*camera)["framerate"].value_or(15);
				std::cout << "Framerate set to: " << config.camera.framerate << std::endl;
			}

			if (camera->contains("bitrate")) {
				config.camera.bitrate = (*camera)["bitrate"].value_or(2000);
				std::cout << "Bitrate set to: " << config.camera.bitrate << std::endl;
			}

			if (camera->contains("rotation")) {
				std::string rotStr = (*camera)["rotation"].value_or("0");
				config.camera.rotation = stringToRotation(rotStr);
				std::cout << "Rotation set to: " << rotationToString(config.camera.rotation)
					  << " degrees" << std::endl;
			}
		}

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		std::cerr << "Using default configuration." << std::endl;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		std::cerr << "Using default configuration." << std::endl;
	}

	RtspServer server(config.server, config.camera);
	server.run();

	return 0;
}
