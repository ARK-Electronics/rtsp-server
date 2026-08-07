#include "CameraProbe.hpp"
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/videodev2.h>

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

// CSI sensors on Jetson bind to the "tegra-video" VI driver; on Pi they come up via
// the unicam/rp1-cfe drivers but are streamed through libcamerasrc, not this node.
// USB webcams are uvcvideo. Everything else (loopback, virtual, encoders) is Other.
CameraType classify_driver(const std::string& driver)
{
	if (driver.find("tegra") != std::string::npos || driver == "vi-output") {
		return CameraType::Csi;
	}

	if (driver == "uvcvideo") {
		return CameraType::Usb;
	}

	return CameraType::Other;
}

} // namespace

const char* camera_type_name(CameraType type)
{
	switch (type) {
	case CameraType::Csi:   return "csi";

	case CameraType::Usb:   return "usb";

	case CameraType::Other: return "other";
	}

	return "other";
}

std::vector<VideoDevice> enumerate_video_devices()
{
	std::vector<VideoDevice> devices;
	namespace fs = std::filesystem;

	std::error_code ec;
	if (!fs::exists("/dev", ec)) {
		return devices;
	}

	for (const auto& entry : fs::directory_iterator("/dev", ec)) {
		const std::string name = entry.path().filename().string();

		// Only /dev/videoN nodes, where N is a number (skip e.g. /dev/video-enc).
		if (name.rfind("video", 0) != 0) {
			continue;
		}

		const std::string digits = name.substr(std::strlen("video"));

		if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) {
		return std::isdigit(c);
		})) {
			continue;
		}

		const std::string path = entry.path().string();

		// O_NONBLOCK so a node held open elsewhere (a busy CSI sensor) still answers
		// QUERYCAP without blocking; QUERYCAP needs no streaming access.
		int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);

		if (fd < 0) {
			continue;
		}

		struct v4l2_capability cap {};
		const int rc = ioctl(fd, VIDIOC_QUERYCAP, &cap);
		close(fd);

		if (rc != 0) {
			continue;
		}

		// device_caps describes THIS node specifically (a UVC camera's metadata node
		// reports Metadata Capture here, not Video Capture); fall back to the device-wide
		// capabilities when the split isn't advertised.
		const __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;

		if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) {
			continue;
		}

		const std::string driver(reinterpret_cast<const char*>(cap.driver));

		devices.push_back({
			std::stoi(digits),
			path,
			std::string(reinterpret_cast<const char*>(cap.card)),
			driver,
			classify_driver(driver),
		});
	}

	std::sort(devices.begin(), devices.end(), [](const VideoDevice & a, const VideoDevice & b) {
		return a.index < b.index;
	});

	return devices;
}

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
