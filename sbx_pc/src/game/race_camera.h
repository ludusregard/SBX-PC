// The race camera: the viewports the race camera's modes write, the camera
// passes they're copied into, and the per-car cockpit camera points.
//
// How a race frame's camera is made:
//   1. RaceCamera::Update (the race mode's Update message) reads the pad,
//      steps the view on the view button, switches mode (RaceCamera::SetMode)
//      and runs the active camera mode's update.
//   2. The mode writes the viewport's eye, target and up (Viewport +1600).
//   3. Viewport::Update copies them into the viewport's first view and builds
//      the rear-view mirror's and the reflection cube's views from them.
//   4. CameraPass::SetFromViewport copies a view into a camera pass and
//      builds its matrices (CameraPass::Update).
//
// The modes (RaceModeMember, game/game_modes.h): 0 chase, 1 cockpit,
// 2 hood (both players in split screen), 3 not identified, 4 TV cameras,
// 5 scripted camera (race intros, rival intros).

#pragma once

#include "game/game_modes.h"
#include "game/guest_types.h"
#include "game/math.h"

namespace game {

using guest::f32_be;
using guest::i16_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// --- Viewports ---

// One view of a viewport, 80 bytes: view i goes to camera pass i. View 0 is
// the main camera, 1 the rear-view mirror, 2-7 the reflection cube's faces.
struct ViewportView {
  u32_be unknown_0;
  uint8_t unknown_4[12];
  Vector eye;
  Vector target;
  Vector up;
  f32_be roll;
  f32_be fov;
  f32_be unknown_72;  // Copied to the camera pass's +388.
  uint8_t unknown_76[4];
};
GUEST_SIZE(ViewportView, 80);
GUEST_OFFSET(ViewportView, eye, 16);
GUEST_OFFSET(ViewportView, roll, 64);
GUEST_OFFSET(ViewportView, fov, 68);

// A player's viewport (1,872 bytes, made by Viewport::Viewport sub_820AAFA8).
// The camera modes write eye, target and up; Viewport::Update (sub_820AA508)
// turns them into the views.
struct Viewport {
  static constexpr int kViews = 20;

  ViewportView views[kViews];
  Vector eye;
  Vector target;
  Vector up;
  Vector reflection_center;  // Where the reflection cube is built from.
  Vector alternate_target;   // The main view's target in mode 1 (cockpit).
  // Car-space points the cockpit and hood cameras start from.
  Vector eye_offset;
  Vector target_offset;
  Vector up_offset;
  Vector unknown_1728;
  Vector alternate_target_offset;
  // The main view's eye, target and up the frame before (Viewport::Update).
  Vector last_eye;
  Vector last_target;
  Vector last_up;
  uint8_t unknown_1808[48];
  f32_be unknown_1856;
  u32_be mode;  // The race camera's mode (RaceModeMember::camera_mode).
  f32_be unknown_1864;
  f32_be unknown_1868;
};
GUEST_SIZE(Viewport, 1872);
GUEST_OFFSET(Viewport, eye, 1600);
GUEST_OFFSET(Viewport, target, 1616);
GUEST_OFFSET(Viewport, up, 1632);
GUEST_OFFSET(Viewport, reflection_center, 1648);
GUEST_OFFSET(Viewport, alternate_target, 1664);
GUEST_OFFSET(Viewport, eye_offset, 1680);
GUEST_OFFSET(Viewport, up_offset, 1712);
GUEST_OFFSET(Viewport, alternate_target_offset, 1744);
GUEST_OFFSET(Viewport, last_eye, 1760);
GUEST_OFFSET(Viewport, mode, 1860);

// The viewports at [0x826B29C8] (made by sub_820AB310): one a player.
struct Viewports {
  ptr<Viewport> players[2];
  u32_be split_screen;
};
constexpr uint32_t kViewports = 0x826B29C8;  // Holds the viewports' address.

// --- Camera passes ---

// A camera pass (21 at 0x826AA8E0, 592 bytes each). Pass 0 is the main view.
struct CameraPass {
  uint8_t unknown_0[16];
  Matrix view;
  Matrix projection;
  Matrix view_projection;
  uint8_t unknown_208[176];
  f32_be unknown_384;  // Pass 0's is set by Viewport_SetNearClip (likely the near clip).
  f32_be unknown_388;
  uint8_t unknown_392[8];
  Vector eye;
  Vector target;
  Vector up;
  f32_be roll;
  uint8_t unknown_452[136];
  u32_be enabled;
};
GUEST_SIZE(CameraPass, 592);
GUEST_OFFSET(CameraPass, view, 16);
GUEST_OFFSET(CameraPass, unknown_384, 384);
GUEST_OFFSET(CameraPass, eye, 400);
GUEST_OFFSET(CameraPass, roll, 448);
GUEST_OFFSET(CameraPass, enabled, 588);
constexpr uint32_t kCameraPasses = 0x826AA8E0;

// --- What the race camera reads ---

// A car's control record (5 words; one a car at 0x826B5810).
struct ControlRecord {
  u32_be buttons;  // 0x40 the view button, 0x80 the chase camera's reset.
  uint8_t unknown_4[8];
  i16_be look;     // The look stick, -32768 to 32767.
  uint8_t unknown_14[6];
};
GUEST_SIZE(ControlRecord, 20);
GUEST_OFFSET(ControlRecord, look, 12);
constexpr uint32_t kControlRecords = 0x826B5810;  // A pointer a car.

constexpr uint32_t kRaceData = 0x826B580C;  // Holds the race data's address.

// A message the dispatcher delivers (an id and two arguments), and the
// dispatcher's queue (a ring of 50).
struct Message {
  u32_be id;
  u32_be arguments[2];
};
GUEST_SIZE(Message, 12);

struct MessageDispatcher {
  static constexpr uint32_t kQueue = 50;

  u32_be handlers[50];
  u32_be handler_count;
  Message queue[kQueue];
  u32_be queued;
  u32_be read;
};
GUEST_OFFSET(MessageDispatcher, queue, 204);
GUEST_OFFSET(MessageDispatcher, queued, 804);

// --- The cockpit camera's points ---

// An item of a car's scene list, as sub_820C5B28 finds one: kind 3, type 2
// is the car model, and model_block its block in the car models' order
// (docs/game_data.md, "Car models": 0 RX7 ... 44 LEAD06).
struct CarSceneItem {
  uint8_t unknown_0[16];
  u32_be kind;
  uint8_t unknown_20[12];
  u32_be type;
  uint8_t unknown_36[12];
  u32_be model_block;
};
GUEST_OFFSET(CarSceneItem, kind, 16);
GUEST_OFFSET(CarSceneItem, type, 32);
GUEST_OFFSET(CarSceneItem, model_block, 48);

// The cockpit camera's eye and target in car space (x forward, y up, z to
// the side: about -0.38 for a right-hand-drive car, +0.33 for the
// left-hand-drive ECLI and the rivals built on it).
struct CockpitPoint {
  Vector eye;
  Vector target;
};
GUEST_SIZE(CockpitPoint, 32);

// The table of cockpit points, one per car model block. In the game's
// writable data, so read from guest memory, not copied.
constexpr uint32_t kCockpitPoints = 0x82608D20;
constexpr uint32_t kCockpitPointCount = 45;

}  // namespace game
