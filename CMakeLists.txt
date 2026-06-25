cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

project(rtsp-server VERSION 0.1 LANGUAGES CXX)

find_package(PkgConfig)

pkg_check_modules(GST REQUIRED IMPORTED_TARGET gstreamer-1.0)
pkg_check_modules(GST_RTSP_SERVER REQUIRED IMPORTED_TARGET gstreamer-rtsp-server-1.0)

include_directories(third_party/tomlplusplus/)

add_executable(rtsp-server
	src/main.cpp
	src/Config.cpp
	src/CameraProbe.cpp
	src/RtspServer.cpp
)

target_link_libraries(rtsp-server
	PkgConfig::GST
	PkgConfig::GST_RTSP_SERVER
)

install(TARGETS rtsp-server RUNTIME DESTINATION bin)
