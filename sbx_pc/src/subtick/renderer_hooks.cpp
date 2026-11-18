// Frame pacing for --subtick: game logic keeps its fixed 30 Hz tick, and in
// races extra frames are drawn between ticks.
//
// Each pass, the game posts an Update and a Draw message (game_messages.h),
// then waits for the render thread in Renderer::WaitForFrame, swaps buffers
// and has the render thread draw the new recording. After that wait, the
// pacer decides the coming pass: when the next 30 Hz tick is due it runs
// normally (a logic pass); otherwise it waits for the next frame slot
// (--max_fps) and the coming pass is render-only: Update messages are
// skipped, so it draws without advancing the game. Everything else in the
// pass runs as the game has it. The menus draw frames between their ticks
// too, once a mode change has settled and while no menu waits to close
// (game_messages.h).
//
// The present's 2-vblank wait is dropped (the pacer holds the rate instead).

#include "debug/frame_state_diff.h"
#include "guest.h"
#include "hooks/title_screen.h"
#include "subtick/car_transforms.h"
#include "subtick/frame_pacer.h"
#include "subtick/game_messages.h"
#include "subtick/input.h"

#ifdef SBX_VULKAN_RENDERER
#include "renderer/renderer.h"
#endif

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include <cmath>
#include <cstring>
#include <set>
#include <string>

REXCVAR_DEFINE_BOOL(subtick, true, "SBX",
                    "Draw frames between the game's 30 Hz ticks (--max_fps), in races and the "
                    "menus, with the cars' controls fed input sampled each frame "
                    "(--subtick_input); game logic stays at 30 Hz");

namespace {

// The video output object passed to the vblank wait: its graphics device.
constexpr uint32_t kVideoOutputDeviceOffset = 140;

#ifdef SBX_VULKAN_RENDERER
// Camera pass 0 (docs/functions.md, "Known data": the camera pass array at
// 0x826AA8E0; a pass's view at +0x10, projection at +0x50, view-projection
// at +0x90).
constexpr uint32_t kMainCamera = 0x826AA8E0;
constexpr uint32_t kCameraView = 0x10;
constexpr uint32_t kCameraProjection = 0x50;
constexpr uint32_t kCameraViewProjection = 0x90;
constexpr uint32_t kCameraPassSize = 592;
constexpr uint32_t kCameraPasses = 21;

// The camera the list just recorded was drawn with, for the native
// renderer's own passes (--gbuffer, --xegtao). In a race, pass 0: with
// --subtick it's this frame's blended camera (CarTransforms_RebuildCamera),
// still in place until CarTransforms_EndFrame. In the menus, the camera pass
// their 3D scene updated on this pass (CarTransforms_MenuSceneCamera; also
// blended with --subtick).
renderer::ListCamera CaptureListCamera(uint8_t* base) {
  renderer::ListCamera camera;
  camera.in_race = subtick::GameMessages_InRace();
  const uint32_t pass = camera.in_race ? kMainCamera : subtick::CarTransforms_MenuSceneCamera();
  if (pass == 0) {
    return camera;
  }
  auto read = [&](uint32_t offset, float* out) {
    for (uint32_t i = 0; i < 16; i++) {
      uint32_t word = guest::LoadU32(base, pass + offset + 4 * i);
      std::memcpy(&out[i], &word, 4);
    }
  };
  read(kCameraView, camera.view);
  read(kCameraProjection, camera.projection);
  read(kCameraViewProjection, camera.view_projection);
  camera.valid = true;
  if (camera.in_race) {
    // The rear-view mirror's pass: the game resolves the mirror's view at
    // 512x128 (its 3D target: pitch 520, 4x MSAA; passes.cpp, kMirrorView),
    // so the first pass after 0 with a perspective projection of that
    // aspect (|P11 / P00| 4; P00 may be negative, a mirrored image). In
    // John's race log (2026-11-17) that's pass 3; passes 1 and 2 are 16:9
    // (3.56), 4 and on square.
    // Each new set of candidates is logged (up to 8), to check it.
    static std::set<std::string> logged_sets;
    std::string candidates;
    for (uint32_t index = 1; index < kCameraPasses; index++) {
      const uint32_t address = kMainCamera + index * kCameraPassSize;
      float projection[16];
      for (uint32_t i = 0; i < 16; i++) {
        uint32_t word = guest::LoadU32(base, address + kCameraProjection + 4 * i);
        std::memcpy(&projection[i], &word, 4);
      }
      const bool perspective = std::fabs(std::fabs(projection[11]) - 1.0f) < 1e-3f &&
                               std::fabs(projection[15]) < 1e-3f && projection[0] != 0.0f;
      if (!perspective) {
        continue;
      }
      const float aspect = std::fabs(projection[5] / projection[0]);
      candidates += fmt::format(" {} (aspect {:.2f}, P00 {:.3f})", index, aspect, projection[0]);
      if (!camera.mirror_valid && std::fabs(aspect - 4.0f) < 0.05f) {
        camera.mirror_valid = true;
        std::memcpy(camera.mirror_projection, projection, sizeof(projection));
        auto read_pass = [&](uint32_t offset, float* out) {
          for (uint32_t i = 0; i < 16; i++) {
            uint32_t word = guest::LoadU32(base, address + offset + 4 * i);
            std::memcpy(&out[i], &word, 4);
          }
        };
        read_pass(kCameraView, camera.mirror_view);
        read_pass(kCameraViewProjection, camera.mirror_view_projection);
        candidates += " <- mirror";
      }
    }
    if (logged_sets.size() < 8 && logged_sets.insert(candidates).second) {
      REXLOG_INFO("race camera passes with a perspective projection:{}",
                  candidates.empty() ? std::string(" none") : candidates);
    }
  }
  return camera;
}
#endif

// Main thread, once per main-loop pass, after the game's own wait for the
// frame in flight: picks the coming pass.
void PaceNextPass(PPCContext& ctx, uint8_t* base) {
  subtick::CarTransforms_EndFrame(base);  // the last frame's camera blend
  subtick::FramePacer& pacer = subtick::GetFramePacer();
  bool finished_logic_pass = !subtick::GameMessages_IsRenderOnlyPass();
  subtick::GameMessages_OnPassFinished(base);
  if (finished_logic_pass) {
    subtick::Input_OnLogicPassFinished();
  }
  bool render_only;
  if (subtick::GameMessages_InRace()) {
    render_only = pacer.WaitForNextFrame();  // false once the next tick is due
  } else if (hooks::TitleScreen_SkipTickWait()) {
    // --skip_cutscenes is running the boot fast (hooks/title_screen.h).
    render_only = false;
  } else if (subtick::GameMessages_MenusSettled()) {
    render_only = pacer.WaitForNextFrame();
  } else {
    // Menus: the game's 30 Hz.
    pacer.WaitForNextTick();
    render_only = false;
  }
  if (!render_only) {
    pacer.OnTickReleased();
  }
  pacer.OnFrameStarted();
  subtick::GameMessages_SetRenderOnlyPass(render_only);
  debug::FrameStateDiff_OnPassStart(base, render_only, subtick::GameMessages_InRace());
  if (render_only) {
    subtick::Input_OnRenderOnlyPass(ctx, base, pacer.TickProgress());
  }
}

}  // namespace

// Renderer::WaitForFrame: r3 = renderer. A tick handler on the main thread.
// With the native renderer, the pass's command list is handed over first
// (whether a logic pass recorded it).
extern "C" REX_FUNC(sub_82092E60) {
  REX_FUNC_PROLOGUE();
#ifdef SBX_VULKAN_RENDERER
  renderer::Renderer_OnListHandedOver(!subtick::GameMessages_IsRenderOnlyPass(),
                                    CaptureListCamera(base));
#endif
  __imp__sub_82092E60(ctx, base);
  if (!REXCVAR_GET(subtick)) {
#ifdef SBX_VULKAN_RENDERER
    subtick::GameMessages_OnPassFinished(base);  // tells when a race ends
#endif
    // Turned off on a render-only pass (from F4, mid-race): every pass is a
    // logic pass from here. Left set, the Update message stayed skipped and
    // the game stood still. The camera blend of the last frame is put back.
    if (subtick::GameMessages_IsRenderOnlyPass()) {
      subtick::CarTransforms_EndFrame(base);
      subtick::GameMessages_SetRenderOnlyPass(false);
    }
    return;
  }
  uint32_t r1 = ctx.r1.u32;
  PaceNextPass(ctx, base);
  ctx.r1.u32 = r1;
}

// Renderer::DrawFrame (render thread): r3 = renderer. With the native
// renderer, each frame's replay is framed for it (Renderer_OnReplayStart,
// Renderer_EndFrame), as ITC-Recomp's render thread loop does.
#ifdef SBX_VULKAN_RENDERER
extern "C" REX_FUNC(sub_82091FB0) {
  REX_FUNC_PROLOGUE();
  renderer::Renderer_OnReplayStart();
  __imp__sub_82091FB0(ctx, base);
  renderer::Renderer_EndFrame();
}
#endif

// VideoOutput::WaitForVblank: r3 = the video output. The native renderer gets
// no vblanks, so this keeps the graphics device call that precedes the wait
// and drops the wait itself, which held each frame for the game's 2-vblank
// interval.
extern "C" REX_FUNC(sub_82573638) {
  REX_FUNC_PROLOGUE();
  bool skip_wait = hooks::TitleScreen_SkipTickWait();
  ctx.r3.u64 = guest::LoadU32(base, ctx.r3.u32 + kVideoOutputDeviceOffset);
  sub_8229DD58(ctx, base);
  // Without --subtick the pacer isn't running (PaceNextPass), so hold the
  // game's 30 Hz here instead of the 2-vblank wait. Without it every pass ran
  // at once, as fast as the CPU allowed.
  if (!REXCVAR_GET(subtick) && !skip_wait) {
    subtick::FramePacer& pacer = subtick::GetFramePacer();
    pacer.WaitForNextTick();
    pacer.OnTickReleased();
  }
}
