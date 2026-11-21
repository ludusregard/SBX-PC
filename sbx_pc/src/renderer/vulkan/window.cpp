#include "renderer/vulkan/window.h"

#include "renderer/app_window.h"
#include "renderer/vulkan/overlay.h"

#include "base/log.h"
#include "base/settings.h"

#include <SDL3/SDL_vulkan.h>
#include <fmt/format.h>
#include <rex/cvar.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>

SBX_SETTING_INT(width, 0, "Graphics", "Window",
                "Window width in pixels (in fullscreen, the drawing width); 0: 1280, or "
                "from height at 16:9", sbx::kRestart);
SBX_SETTING_INT(height, 0, "Graphics", "Window",
                "Window height in pixels (in fullscreen, the drawing height); 0: 720, or "
                "from width at 16:9", sbx::kRestart);
// The SDK's (it also makes the SDK's own window, hidden in exclusive mode,
// fullscreen); ours follows it.
REXCVAR_DECLARE(bool, fullscreen);

namespace renderer {

// The window's size from --width and --height (one alone keeps 16:9).
VkExtent2D WindowSize() {
  int32_t width = SBX_GET(width), height = SBX_GET(height);
  if (width <= 0 && height <= 0) {
    return {kDefaultWidth, kDefaultHeight};
  }
  if (width <= 0) {
    width = (height * 16 + 8) / 9;
  } else if (height <= 0) {
    height = (width * 9 + 8) / 16;
  }
  return {uint32_t(width), uint32_t(height)};
}

// How many times the game's own resolution (1280x720 frame, 1024x576 3D
// view) is drawn (see "Resolution" in docs/native-renderer.md): in
// fullscreen with --width or --height, the largest that fits them;
// otherwise the largest that fits the window's pixels (`pixels`, the
// swapchain's size).
float ResolutionScale(VkExtent2D pixels) {
  bool sized = SBX_GET(width) > 0 || SBX_GET(height) > 0;
  VkExtent2D size = (REXCVAR_GET(fullscreen) && sized) || !pixels.width || !pixels.height
                        ? WindowSize()
                        : pixels;
  return std::min(float(size.width) / kDefaultWidth, float(size.height) / kDefaultHeight);
}

namespace {

// Set (from the main thread's event loop) when our window's size in pixels
// changes.
std::atomic<bool> g_window_resized{false};
SDL_WindowID g_window_id = 0;

bool SDLCALL WatchWindowEvents(void*, SDL_Event* event) {
  if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
      event->window.windowID == g_window_id) {
    g_window_resized = true;
  }
  overlay::OnEvent(*event, g_window_id);  // F3, F4 and the panels' input
  return true;
}

}  // namespace

bool TakeWindowResized() {
  return g_window_resized.exchange(false);
}

// Where a frame of `source`'s shape goes in `target`: as large as fits,
// centered (black bars on the sides or above and below).
VkOffset3D FitOffsets(VkExtent2D source, VkExtent2D target, VkOffset3D& end) {
  uint64_t width = target.width, height = target.height;
  if (uint64_t(target.width) * source.height > uint64_t(target.height) * source.width) {
    width = (uint64_t(target.height) * source.width + source.height / 2) / source.height;
  } else {
    height = (uint64_t(target.width) * source.height + source.width / 2) / source.width;
  }
  VkOffset3D start{int32_t((target.width - width) / 2), int32_t((target.height - height) / 2), 0};
  end = {start.x + int32_t(width), start.y + int32_t(height), 1};
  return start;
}

void SDLCALL CreateWindowOnMainThread(void* data) {
  auto& setup = *static_cast<std::shared_ptr<WindowSetup>*>(data);
  auto finish = [&](std::string error) {
    setup->error = std::move(error);
    setup->done.set_value();
    delete static_cast<std::shared_ptr<WindowSetup>*>(data);
  };
  VkExtent2D size = WindowSize();
  // Fullscreen: borderless, at the desktop's resolution (no mode change).
  // High pixel density: real pixels under the desktop's scaling, not a
  // smaller surface the compositor stretches (2648x1490 for a 3840x2160
  // screen at 145%).
  SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                          SDL_WINDOW_HIGH_PIXEL_DENSITY |
                          (REXCVAR_GET(fullscreen) ? SDL_WINDOW_FULLSCREEN : 0);
  setup->window =
      SDL_CreateWindow("ITC native renderer", int(size.width), int(size.height), flags);
  if (!setup->window) {
    return finish(fmt::format("SDL_CreateWindow: {}", SDL_GetError()));
  }
  if (const SDL_DisplayMode* mode =
          SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(setup->window))) {
    setup->display = {uint32_t(std::lround(mode->w * mode->pixel_density)),
                      uint32_t(std::lround(mode->h * mode->pixel_density))};
  }
  // The window's size is given in pixels: in the desktop's (scaled) units,
  // it's that over the pixel density.
  if (float density = SDL_GetWindowPixelDensity(setup->window);
      density > 0.0f && density != 1.0f && !REXCVAR_GET(fullscreen)) {
    SDL_SetWindowSize(setup->window, int(std::lround(size.width / density)),
                      int(std::lround(size.height / density)));
    SDL_SyncWindow(setup->window);
  }
  setup->density = SDL_GetWindowPixelDensity(setup->window);
  g_window_id = SDL_GetWindowID(setup->window);
  SDL_AddEventWatch(WatchWindowEvents, nullptr);
  AppWindow_AdoptOurWindow(setup->window);
  auto get_instance_proc_addr =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr());
  if (!get_instance_proc_addr || !vk::LoadGlobal(get_instance_proc_addr)) {
    return finish(fmt::format("no Vulkan loader: {}", SDL_GetError()));
  }
  Uint32 extension_count = 0;
  const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
  uint32_t api_version = VK_API_VERSION_1_1;
  vk::vkEnumerateInstanceVersion(&api_version);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "ITC native renderer";
  app.apiVersion = std::min<uint32_t>(api_version, VK_API_VERSION_1_3);
  const char* validation_layer = "VK_LAYER_KHRONOS_validation";
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  info.pApplicationInfo = &app;
  info.enabledExtensionCount = extension_count;
  info.ppEnabledExtensionNames = extensions;
  if (setup->validation) {
    info.enabledLayerCount = 1;
    info.ppEnabledLayerNames = &validation_layer;
  }
  VkResult result = vk::vkCreateInstance(&info, nullptr, &setup->instance);
  if (result == VK_ERROR_LAYER_NOT_PRESENT) {
    ITC_LOG_WARN("native renderer: validation layer not installed, continuing without it");
    info.enabledLayerCount = 0;
    result = vk::vkCreateInstance(&info, nullptr, &setup->instance);
  }
  if (result != VK_SUCCESS) {
    return finish(fmt::format("vkCreateInstance: {}", int(result)));
  }
  if (!vk::LoadInstance(setup->instance)) {
    return finish("missing instance functions");
  }
  if (!SDL_Vulkan_CreateSurface(setup->window, setup->instance, nullptr, &setup->surface)) {
    return finish(fmt::format("SDL_Vulkan_CreateSurface: {}", SDL_GetError()));
  }
  finish("");
}

}  // namespace renderer
