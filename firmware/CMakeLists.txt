cmake_minimum_required(VERSION 3.16.0)

# The one place the firmware version is written. ESP-IDF embeds it in
# the image header (esp_app_desc_t), which is what the boot banner, the
# `version` command and -- later -- an OTA update compare against.
#
# It is set here rather than taken from `git describe` on purpose:
# PlatformIO only re-runs CMake when this file changes, so a version
# derived from git would silently go stale between builds. Editing this
# line is what forces the rebuild. See CHANGELOG.md for the scheme.
set(PROJECT_VER "0.7.0-dev")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)
