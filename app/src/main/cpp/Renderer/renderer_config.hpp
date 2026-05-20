#pragma once

// Select which graphics backend should be installed.
//
// Options:
//   #define SELECT_GRAPHIC AUTO
//   #define SELECT_GRAPHIC VULKAN
//   #define SELECT_GRAPHIC OPENGL
//
// AUTO installs both graphics hooks and the first backend that presents a frame
// becomes active. Input remains auto-detected separately.

#define AUTO 0
#define VULKAN 1
#define OPENGL 2

#ifndef SELECT_GRAPHIC
#define SELECT_GRAPHIC AUTO
#endif

#if SELECT_GRAPHIC != AUTO && SELECT_GRAPHIC != VULKAN && SELECT_GRAPHIC != OPENGL
#error "SELECT_GRAPHIC must be AUTO, VULKAN, or OPENGL"
#endif
