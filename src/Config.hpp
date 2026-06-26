#pragma once

#include <string>
#include <map>
#include <utility>
#include <vector>

// Resolution presets - limited to options that work on both Pi and Jetson
enum class ResolutionPreset {
	R320x240,
	R640x480,
	R1280x720,
	R1920x1080,
};

enum class CameraRotation {
	ROTATE_0,
	ROTATE_90,
	ROTATE_180,
	ROTATE_270,
};

const std::map<ResolutionPreset, std::pair<int, int>> RESOLUTION_MAP = {
	{ResolutionPreset::R320x240, {320, 240}},
	{ResolutionPreset::R640x480, {640, 480}},
	{ResolutionPreset::R1280x720, {1280, 720}},
	{ResolutionPreset::R1920x1080, {1920, 1080}}
};

ResolutionPreset stringToResolution(const std::string& resolution);
std::string resolutionToString(ResolutionPreset resolution);

CameraRotation stringToRotation(const std::string& rotation);
std::string rotationToString(CameraRotation rotation);

struct ServerConfig {
	std::string path;
	std::string address;
	std::string port;
};

struct CameraConfig {
	ResolutionPreset resolution;
	int framerate;
	int bitrate;
	CameraRotation rotation;

	std::pair<int, int> getDimensions() const
	{
		return RESOLUTION_MAP.at(resolution);
	}

	int getWidth() const
	{
		return getDimensions().first;
	}

	int getHeight() const
	{
		return getDimensions().second;
	}

	int getRotationDegrees() const
	{
		switch (rotation) {
		case CameraRotation::ROTATE_0: return 0;

		case CameraRotation::ROTATE_90: return 90;

		case CameraRotation::ROTATE_180: return 180;

		case CameraRotation::ROTATE_270: return 270;
		}

		return 0;
	}
};

struct AppConfig {
	ServerConfig server;
	CameraConfig camera;
};
