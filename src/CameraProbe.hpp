#pragma once

#include <string>
#include <vector>

// How a /dev/video* node is driven, which decides the GStreamer source element:
// CSI sensors go through nvarguscamerasrc (Jetson) or libcamerasrc (Pi); USB/UVC
// webcams go through v4l2src on any platform.
enum class CameraType {
	Csi,
	Usb,
	Other,
};

// One enumerated V4L2 capture node (/dev/videoN). `index` is N, used both to order
// devices (auto-selection picks the lowest) and to derive the nvarguscamerasrc
// sensor-id for CSI sensors.
struct VideoDevice {
	int index;
	std::string path;     // e.g. "/dev/video0"
	std::string name;     // V4L2 card name, e.g. "vi-output, imx219 9-0010" or "HD Webcam"
	std::string driver;   // V4L2 driver name, e.g. "tegra-video" or "uvcvideo"
	CameraType type;
};

// Human-readable label for a CameraType ("csi"/"usb"/"other").
const char* camera_type_name(CameraType type);

// Enumerate every /dev/video* node that can actually capture video (VIDIOC_QUERYCAP
// reports V4L2_CAP_VIDEO_CAPTURE), sorted ascending by index. UVC webcams expose
// extra metadata-only nodes; those are filtered out here so they never get picked.
// Returns an empty vector if no capture device is present.
std::vector<VideoDevice> enumerate_video_devices();

// A single discrete capture mode as reported by the kernel V4L2 driver.
struct SensorMode {
	int width;
	int height;
	double max_fps;
};

// The resolution/framerate actually used for streaming, after clamping the
// desired settings down to what the sensor can deliver.
struct StreamProfile {
	int width;
	int height;
	int framerate;
};

// Enumerate the discrete capture modes of a CSI sensor by running
// `v4l2-ctl --list-formats-ext -d /dev/video<sensor_id>`. v4l2 and Argus read the
// same device-tree mode table, so this matches what nvarguscamerasrc will accept.
// Returns an empty vector if v4l2-ctl is missing or reports nothing.
std::vector<SensorMode> probe_sensor_modes(int sensor_id);

// Parse the stdout of `v4l2-ctl --list-formats-ext` into deduplicated modes
// (max fps per resolution). Split out from probe_sensor_modes so it is testable.
std::vector<SensorMode> parse_v4l2_modes(const std::string& v4l2_output);

// Pick a streaming profile: the desired resolution clamped to the largest mode,
// and the framerate clamped to the fastest mode able to source that resolution.
// With no detected modes, returns the desired values with framerate capped at 30.
StreamProfile select_stream_profile(const std::vector<SensorMode>& modes, int desired_w, int desired_h, int desired_fps);
