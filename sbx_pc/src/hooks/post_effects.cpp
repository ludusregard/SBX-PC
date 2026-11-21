// The race's post effects (ITC-Recomp's docs/functions/post-effects.md;
// addresses through docs/itc_to_sbx_functions.tsv, and SBX's post effect
// code sits 0x1528 below ITC's: Glow::Glow ITC 82138610 -> 821370E8,
// MotionBlur::MotionBlur 82136130 -> 82134C08, both "calls 0.90").
//
// The glow (kind 0, the bloom and star streaks) records its draws each
// frame in Glow::Record (sub_821364B0; ITC sub_821379D8, its vtable slot 0),
// reading its strength from +8452 (Glow::SetStrength, sub_821359F0: +8452 =
// f1, set by the speed each update) into the bloom combine's c33.y (pixel
// shader 9750599DF5ED3B39). --bloom=false records it with the strength at 0,
// so the combine adds nothing, as ITC's --glow does on the emulated GPU. It's
// game state, so it works with either renderer.
//
// Camera cuts (--reset_effects_on_camera_cut; the cuts are subtick's,
// subtick/car_transforms.h): the two effects that carry earlier frames start
// again on a tick the camera was cut, so the old view doesn't bleed into the
// new one.
// - The glow keeps a history: Glow::Record's cross-fade quad draws last
//   frame's glow over this frame's at the history weight (+8460). Its one-shot
//   flag at +40 (set by its constructor, cleared by Record) makes the weight 0
//   for a frame: no history. It's set before each Record of the tick.
// - The motion blur's velocity buffer compares each vertex against the
//   previous view-projection, which View::UploadPreviousViewProjection
//   (sub_82134EC8) records into vertex c100: on a logic pass +8080 (the last
//   tick's), which it then moves to +8144 before taking the camera pass's new
//   one into +8080; on render-only passes +8144. Across a cut that's the old
//   camera, so the whole screen smears. On a cut tick both are the camera
//   pass's view-projection for the call (on a logic pass left so, as they're
//   the new camera; on a render-only pass put back after).

#include "base/settings.h"
#include "guest.h"
#include "subtick/car_transforms.h"
#include "subtick/game_messages.h"

SBX_SETTING_BOOL(bloom, true, "Graphics", "Post-Process",
                 "The game's bloom (the glow over bright lights in races); false records "
                 "it with no strength");

SBX_SETTING_BOOL(reset_effects_on_camera_cut, true, "Graphics", "Post-Process",
                 "On a camera cut, start the effects that keep earlier frames again: the "
                 "bloom's history, the motion blur's previous camera, and the native "
                 "renderer's own (XeGTAO's history, the camera motion, blur_frame_blend)");

namespace {
constexpr uint32_t kGlowStrength = 8452;  // float
constexpr uint32_t kGlowNoHistory = 40;   // one-shot: this frame's cross-fade weight 0

// The view (r3 of View::UploadPreviousViewProjection).
constexpr uint32_t kViewCameraPass = 36;      // its camera pass's index
constexpr uint32_t kViewProjection = 8080;    // the last tick's view-projection
constexpr uint32_t kViewPrevious = 8144;      // the one before it
constexpr uint32_t kCameraPasses = 0x826AA8E0;
constexpr uint32_t kCameraPassSize = 592;
constexpr uint32_t kCameraPassCount = 21;
constexpr uint32_t kCameraPassViewProjection = 144;
constexpr uint32_t kCameraPassEnabled = 588;

bool CameraCutThisTick() {
  return SBX_GET(reset_effects_on_camera_cut) && subtick::CarTransforms_CameraCutThisTick();
}

void CopyMatrix(uint8_t* base, uint32_t to, uint32_t from) {
  for (uint32_t i = 0; i < 16; i++) {
    guest::StoreU32(base, to + 4 * i, guest::LoadU32(base, from + 4 * i));
  }
}
}  // namespace

// Glow::Record: r3 = the glow.
extern "C" REX_FUNC(sub_821364B0) {
  REX_FUNC_PROLOGUE();
  if (CameraCutThisTick()) {
    guest::StoreU32(base, ctx.r3.u32 + kGlowNoHistory, 1);
  }
  if (SBX_GET(bloom)) {
    __imp__sub_821364B0(ctx, base);
    return;
  }
  uint32_t glow = ctx.r3.u32;
  uint32_t strength = guest::LoadU32(base, glow + kGlowStrength);
  guest::StoreU32(base, glow + kGlowStrength, 0);  // 0.0f
  __imp__sub_821364B0(ctx, base);
  guest::StoreU32(base, glow + kGlowStrength, strength);
}

// View::UploadPreviousViewProjection: r3 = the view. On a camera cut tick,
// the previous view-projection is the camera's own (no camera motion).
extern "C" REX_FUNC(sub_82134EC8) {
  REX_FUNC_PROLOGUE();
  const uint32_t view = ctx.r3.u32;
  const uint32_t index = guest::LoadU32(base, view + kViewCameraPass);
  const uint32_t pass = kCameraPasses + kCameraPassSize * index;
  if (!CameraCutThisTick() || index >= kCameraPassCount ||
      guest::LoadU32(base, pass + kCameraPassEnabled) == 0) {
    __imp__sub_82134EC8(ctx, base);
    return;
  }
  uint32_t saved[2][16];
  for (uint32_t i = 0; i < 16; i++) {
    saved[0][i] = guest::LoadU32(base, view + kViewProjection + 4 * i);
    saved[1][i] = guest::LoadU32(base, view + kViewPrevious + 4 * i);
  }
  CopyMatrix(base, view + kViewProjection, pass + kCameraPassViewProjection);
  CopyMatrix(base, view + kViewPrevious, pass + kCameraPassViewProjection);
  __imp__sub_82134EC8(ctx, base);
  if (subtick::GameMessages_IsRenderOnlyPass()) {
    for (uint32_t i = 0; i < 16; i++) {
      guest::StoreU32(base, view + kViewProjection + 4 * i, saved[0][i]);
      guest::StoreU32(base, view + kViewPrevious + 4 * i, saved[1][i]);
    }
  }
}
