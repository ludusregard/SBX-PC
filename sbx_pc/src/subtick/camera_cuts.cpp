// The game's own camera switches, reported to the camera blend as cuts
// (--subtick_camera_cuts; car_transforms.cpp). Blending across a switch
// slides the camera through the car or across the track for a tick; these
// are the places the race camera switches (game/race_camera.h):
//
// - RaceCamera::SetMode (sub_82157188): every call starts the mode's camera
//   again, snapped to the car. The view button, the race start, the
//   restore after a pause or a replay, the intros' ends.
// - The chase camera's reset (button 0x80, GameObject384::reset_behind):
//   its update puts the camera straight behind the car.
// - The TV cameras (mode 4, sub_82157380): a change of zone (another
//   trackside camera) or of the TV cameras' kind.
// - The scripted camera (mode 5, sub_821541E0): a shot ending (the shot
//   index steps), and the next or previous camera picked on the pad
//   (sub_82153BE0, sub_82153CA8). Its update writes the ended shot's last
//   frame on the tick the index steps, so the cut is reported at the next
//   update, when the new shot's first frame is written.
//
// Mode 3 and the menus' cameras (the garage's presets) aren't reported yet;
// they're blended across their switches.

#include "subtick/car_transforms.h"

#include "game/game_modes.h"
#include "hooks/native_calls.h"

#include <map>

namespace {

using guest::At;

// Scripted cameras whose shot changed (or was picked) since their last update.
std::map<uint32_t, bool> g_scripted_cut_pending;

}  // namespace

// RaceCamera::SetMode: r3 the race mode's member, r4 the mode.
extern "C" REX_FUNC(sub_82157188) {
  REX_FUNC_PROLOGUE();
  subtick::CarTransforms_GameCameraCut("race camera mode change");
  hooks::NativeRaceCamera_SetMode(ctx, base);
}

// ChaseCamera::Update (the chase camera's vtable slot 0): r3 the camera.
extern "C" REX_FUNC(sub_821528E0) {
  REX_FUNC_PROLOGUE();
  guest::Guest g(ctx, base);
  if (At<game::GameObject384>(ctx.r3.u32)->reset_behind != 0) {
    subtick::CarTransforms_GameCameraCut("chase camera reset");
  }
  __imp__sub_821528E0(ctx, base);
}

// TvCameras::Update (vtable slot 0): r3 the TV cameras. The zone (and the
// camera's kind) for this tick is picked and the camera written in the same
// call.
extern "C" REX_FUNC(sub_82157380) {
  REX_FUNC_PROLOGUE();
  guest::Guest g(ctx, base);
  game::Member13360* tv = At<game::Member13360>(ctx.r3.u32);
  const int32_t zone = tv->zone;
  const uint32_t kind = tv->unknown_448;
  __imp__sub_82157380(ctx, base);
  if (tv->zone != zone || tv->unknown_448 != kind) {
    subtick::CarTransforms_GameCameraCut("TV camera change");
  }
}

// ScriptedCamera::Update (vtable slot 0): r3 the scripted camera.
extern "C" REX_FUNC(sub_821541E0) {
  REX_FUNC_PROLOGUE();
  guest::Guest g(ctx, base);
  const uint32_t address = ctx.r3.u32;
  game::Member56896* scripted = At<game::Member56896>(address);
  bool& pending = g_scripted_cut_pending[address];
  if (pending) {
    pending = false;
    subtick::CarTransforms_GameCameraCut("scripted camera shot change");
  }
  const int32_t animation = scripted->animation;
  const uint32_t shot = scripted->shot;
  __imp__sub_821541E0(ctx, base);
  if (scripted->animation != animation || scripted->shot != shot) {
    pending = true;
  }
}

// ScriptedCamera::NextCamera and PreviousCamera (from its vtable slot 1, on
// pad codes 164 and 163): r3 the scripted camera. The pick shows at the
// next update.
extern "C" REX_FUNC(sub_82153BE0) {
  REX_FUNC_PROLOGUE();
  g_scripted_cut_pending[ctx.r3.u32] = true;
  __imp__sub_82153BE0(ctx, base);
}

extern "C" REX_FUNC(sub_82153CA8) {
  REX_FUNC_PROLOGUE();
  g_scripted_cut_pending[ctx.r3.u32] = true;
  __imp__sub_82153CA8(ctx, base);
}
