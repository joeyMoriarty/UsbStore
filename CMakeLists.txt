# UsbStore - ESP32-S3 USB mass-storage host + LAN file server.
cmake_minimum_required(VERSION 3.16)

# Shown on the web page, so you can tell which build is running after an OTA.
set(PROJECT_VER "0.2.3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(usbstore)
