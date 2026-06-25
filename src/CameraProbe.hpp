#pragma once

#include <string>
#include <vector>

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
