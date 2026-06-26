# RTSP Server
## Pre-requisites
**All**
```
	sudo apt-get install -y  \
		libgstreamer1.0-dev \
		libgstreamer-plugins-base1.0-dev \
		libgstreamer-plugins-bad1.0-dev \
		libgstrtspserver-1.0-dev \
		gstreamer1.0-plugins-ugly \
		gstreamer1.0-tools \
		gstreamer1.0-gl \
		gstreamer1.0-gtk3 \
		gstreamer1.0-rtsp
```
**Pi**
```
sudo apt-get install -y gstreamer1.0-libcamera
```
**Ubuntu 22.04**, see https://github.com/antimof/UxPlay/issues/121
```
sudo apt remove gstreamer1.0-vaapi
```

## Build and run
Build
```
make
```
Run
```
./build/rtsp-server
```
View the stream
```
gst-launch-1.0 playbin uri=rtsp://0.0.0.0:5600/camera1 latency=0
```
or
```
gst-launch-1.0 rtspsrc location=rtsp://0.0.0.0:5600/camera1 latency=0 ! rtph264depay ! avdec_h264 ! videoconvert ! autovideosink
```

## Camera auto-detection (Jetson)
The `framerate` and `resolution` in `config.toml` are treated as desired values. On Jetson the server probes the sensor's real capture modes (via `v4l2-ctl --list-formats-ext`) and clamps them to what the sensor actually supports before building the pipeline. This prevents `nvarguscamerasrc` from failing to acquire the camera ("Frame Rate specified is greater than supported" → "No cameras available") when the configured framerate exceeds the sensor's mode — e.g. an IMX708 that only advertises `4608x2592@14` will stream at 14fps. Requires `v4l-utils`. If the probe comes back empty — e.g. a just-restarted instance whose `nvarguscamerasrc` still holds the camera, so `v4l2-ctl` sees `EBUSY` — the server retries once a second until the sensor reports its modes rather than guessing at the requested values.
