// The native renderer's window (Vulkan): made on the main thread with its
// Vulkan instance and surface, sized from --width and --height, and the
// resolution scale the game's frame is drawn at in it (see "Resolution" in
// docs/native-renderer-port.md).

#pragma once

#include "renderer/vulkan/vulkan_functions.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <future>
#include <string>

namespace renderer {

// The game's own frame size (the 3D view is 1024x576 inside it).
constexpr uint32_t kDefaultWidth = 1280, kDefaultHeight = 720;

// The window and Vulkan instance, created on the main thread (SDL's window
// calls belong there; the SDK runs SDL's event loop on it).
struct WindowSetup {
  bool validation = false;
  SDL_Window* window = nullptr;
  VkExtent2D display{};  // its screen's size in pixels
  float density = 1.0f;  // pixels per unit of the desktop's
  VkInstance instance = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  std::string error;
  std::promise<void> done;
};

// Runs on the main thread (SDL_RunOnMainThread): `data` is a new
// std::shared_ptr<WindowSetup>, deleted here; `done` is set when finished.
void SDLCALL CreateWindowOnMainThread(void* data);

// The window's size from --width and --height (one alone keeps 16:9).
VkExtent2D WindowSize();

// How many times the game's own resolution is drawn, for a window of
// `pixels` (the swapchain's size).
float ResolutionScale(VkExtent2D pixels);

// Render thread: whether the window's size in pixels changed since the last
// call (the swapchain is then made again; Wayland never reports it out of
// date).
bool TakeWindowResized();

// Where a frame of `source`'s shape goes in `target`: as large as fits,
// centered. Returns the start; `end` is set to the end.
VkOffset3D FitOffsets(VkExtent2D source, VkExtent2D target, VkOffset3D& end);

}  // namespace renderer
