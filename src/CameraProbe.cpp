#include "CameraProbe.hpp"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <sstream>

namespace
{

// Run a shell command and capture its stdout. Returns "" if it can't be run.
std::string run_command(const std::string& cmd)
{
	std::array<char, 256> buffer;
	std::string result;
	std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);

	if (!pipe) {
		return result;
	}

	while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
		result += buffer.data();
	}

	return result;
}

} // namespace

std::vector<SensorMode> probe_sensor_modes(int sensor_id)
{
	const std::string dev = "/dev/video" + std::to_string(sensor_id);
	return parse_v4l2_modes(run_command("v4l2-ctl --list-formats-ext -d " + dev + " 2>/dev/null"));
}

std::vector<SensorMode> parse_v4l2_modes(const std::string& v4l2_output)
{
	std::vector<SensorMode> modes;
	std::istringstream stream(v4l2_output);
	std::string line;
	int cur_w = 0;
	int cur_h = 0;

	// v4l2-ctl groups frame intervals (fps) under each "Size: Discrete WxH". A
	// resolution can repeat across pixel formats, so merge on (w,h) keeping max fps.
	while (std::getline(stream, line)) {
		int w = 0;
		int h = 0;
		double fps = 0.0;

		if (std::sscanf(line.c_str(), " Size: Discrete %dx%d", &w, &h) == 2) {
			cur_w = w;
			cur_h = h;

		} else if (cur_w > 0 && std::sscanf(line.c_str(), " Interval: Discrete %*fs (%lf fps)", &fps) == 1) {
			auto it = std::find_if(modes.begin(), modes.end(), [&](const SensorMode & m) {
				return m.width == cur_w && m.height == cur_h;
			});

			if (it == modes.end()) {
				modes.push_back({cur_w, cur_h, fps});

			} else if (fps > it->max_fps) {
				it->max_fps = fps;
			}
		}
	}

	return modes;
}

StreamProfile select_stream_profile(const std::vector<SensorMode>& modes, int desired_w, int desired_h, int desired_fps)
{
	// No detection: keep the requested size, fall back to the legacy 30fps cap.
	if (modes.empty()) {
		return {desired_w, desired_h, std::min(desired_fps, 30)};
	}

	// The largest mode bounds the maximum output resolution argus can scale down from.
	const auto& largest = *std::max_element(modes.begin(), modes.end(), [](const SensorMode & a, const SensorMode & b) {
		return (a.width * a.height) < (b.width * b.height);
	});

	const int out_w = std::min(desired_w, largest.width);
	const int out_h = std::min(desired_h, largest.height);

	// Fastest mode that can source the chosen output (argus scales down, so any
	// mode at least that large qualifies; the largest always does).
	double max_fps = largest.max_fps;

	for (const auto& m : modes) {
		if (m.width >= out_w && m.height >= out_h && m.max_fps > max_fps) {
			max_fps = m.max_fps;
		}
	}

	// floor() so we request an integer rate strictly within the sensor's limit:
	// sensors report values like 14.000001 and argus rejects anything "greater
	// than supported", so requesting 14 is safe while 15 is not.
	int fps = std::min(desired_fps, static_cast<int>(std::floor(max_fps)));

	if (fps < 1) {
		fps = 1;
	}

	return {out_w, out_h, fps};
}
