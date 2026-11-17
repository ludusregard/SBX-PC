// The command lists handed from the game's thread to the render thread, as
// the renderer sees them (renderer.h): whether a logic pass recorded each,
// and its camera. Kept apart from the back end: nothing here is Vulkan's.

#include "renderer/renderer.h"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>

namespace renderer {

namespace {
// Lists handed over and not yet replayed: whether a logic pass recorded
// each (any thread, under g_lists_mutex); and the one being replayed
// (render thread).
struct HandedList {
  bool logic_pass;
  ListCamera camera;
};
std::mutex g_lists_mutex;
std::deque<HandedList> g_lists;
bool g_replaying_logic_list = true;
ListCamera g_replaying_camera;  // render thread
uint64_t g_lists_missing = 0, g_lists_queued_most = 0;
}  // namespace

void Renderer_OnListHandedOver(bool logic_pass, const ListCamera& camera) {
  std::lock_guard lock(g_lists_mutex);
  g_lists.push_back({logic_pass, camera});
}

void Renderer_OnReplayStart() {
  std::lock_guard lock(g_lists_mutex);
  g_lists_queued_most = std::max<uint64_t>(g_lists_queued_most, g_lists.size());
  if (g_lists.empty()) {
    // Not handed over by WaitForFrame: taken as a logic pass's.
    g_lists_missing++;
    g_replaying_logic_list = true;
    g_replaying_camera = {};
    return;
  }
  g_replaying_logic_list = g_lists.front().logic_pass;
  g_replaying_camera = g_lists.front().camera;
  g_lists.pop_front();
}

bool Renderer_ReplayingLogicList() {
  return g_replaying_logic_list;
}

ListCamera Renderer_ReplayingCamera() {
  return g_replaying_camera;
}

void Renderer_TakeListStats(uint64_t& missing, uint64_t& most) {
  std::lock_guard lock(g_lists_mutex);
  missing = g_lists_missing;
  most = g_lists_queued_most;
  g_lists_missing = g_lists_queued_most = 0;
}

}  // namespace renderer
