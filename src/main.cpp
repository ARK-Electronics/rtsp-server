#include "RtspServer.hpp"
#include "Config.hpp"
#include <iostream>
#include <filesystem>
#include <toml.hpp>

int main(int argc, char** argv)
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
		},
		.hls = {
			.enabled = false,
			.segmentDuration = 1,
			.playlistLength = 5
		}
	};

	// Config lookup: --config <path> (or --config=<path>) overrides everything;
	// otherwise user override > deb-installed default.
	const std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
	const auto user_config = std::filesystem::path(home) / ".config/ark/rtsp-server/config.toml";
	const auto default_config = std::filesystem::path("/opt/ark/share/rtsp-server/config.toml");
	std::string config_path = (std::filesystem::exists(user_config) ? user_config : default_config).string();

	for (int i = 1; i < argc; i++) {
		std::string arg = argv[i];

		if (arg == "--config" && i + 1 < argc) {
			config_path = argv[++i];

		} else if (arg.rfind("--config=", 0) == 0) {
			config_path = arg.substr(std::string("--config=").size());
		}
	}

	try {
		toml::table tomlConfig = toml::parse_file(config_path);

		// RTSP server config
		if (auto rtsp = tomlConfig["rtsp"].as_table()) {
			if (rtsp->contains("url")) {
				config.server.path = (*rtsp)["url"].value_or("camera1");
			}

			if (rtsp->contains("address")) {
				config.server.address = (*rtsp)["address"].value_or("0.0.0.0");
			}

			if (rtsp->contains("port")) {
				// Accept the port whether it is stored as a TOML integer
				// (port = 5600) or a quoted string (port = "5600"). value_or<int>
				// silently returns the fallback when the stored type does not
				// match, so handle both forms explicitly.
				auto port = (*rtsp)["port"];

				if (auto i = port.value<int64_t>()) {
					config.server.port = std::to_string(*i);

				} else if (auto s = port.value<std::string>()) {
					config.server.port = *s;
				}
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

		// HLS config (browser-playable restream; off by default)
		if (auto hls = tomlConfig["hls"].as_table()) {
			if (hls->contains("enabled")) {
				config.hls.enabled = (*hls)["enabled"].value_or(false);
				std::cout << "HLS " << (config.hls.enabled ? "enabled" : "disabled") << std::endl;
			}

			if (hls->contains("segment_duration")) {
				config.hls.segmentDuration = (*hls)["segment_duration"].value_or(1);
			}

			if (hls->contains("playlist_length")) {
				config.hls.playlistLength = (*hls)["playlist_length"].value_or(5);
			}
		}

	} catch (const toml::parse_error& err) {
		std::cerr << "Parsing failed:\n" << err << "\n";
		std::cerr << "Using default configuration." << std::endl;

	} catch (const std::exception& err) {
		std::cerr << "Error: " << err.what() << "\n";
		std::cerr << "Using default configuration." << std::endl;
	}

	RtspServer server(config.server, config.camera, config.hls);
	server.run();

	return 0;
}
