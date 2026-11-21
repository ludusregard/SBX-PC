// The game modes: the state machines every mode's messages go through, the
// modes themselves, and the race mode's members.

#pragma once

#include "game/fixed_list.h"
#include "game/guest_types.h"
#include "game/math.h"
#include "game/model.h"

namespace game {

using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// A state machine's key: the state, then the message's three words.
struct StateKey {
  u32_be words[4];

  bool operator==(const StateKey& other) const {
    for (int i = 0; i < 4; i++) {
      if (words[i] != other.words[i]) {
        return false;
      }
    }
    return true;
  }
};

// What a key leads to: a functor (its object kept inside it), and the state
// to go to (0 to stay).
struct Handler {
  u32_be target;     // The functor's object: `object` here.
  u32_be object[6];  // Its first word is its vtable.
  u32_be next_state;
};
GUEST_SIZE(Handler, 32);

struct StateEntry {
  StateKey key;
  Handler handler;
};
GUEST_SIZE(StateEntry, 48);

struct StateMachine {
  static constexpr uint32_t kEntries = 200;

  StateEntry entries[kEntries];
  u32_be count;
  u32_be state;
};
GUEST_SIZE(StateMachine, 9608);

// The race overlays' machine: a state machine, then its overlays.
struct RaceOverlay {
  uint8_t unknown_0[772];
  u32_be unknown_772;
  u32_be unknown_776;
  u32_be unknown_780;
};

struct RaceOverlaysMachine {
  static constexpr int kOverlays = 25;

  StateMachine machine;
  ptr<RaceOverlay> overlays[kOverlays];
};

// A mode: a base with two members, each with its own vtable.
struct Mode {
  u32_be vtable;
  u32_be member;  // Points at member_vtable's object.
  u32_be member_vtable;
  uint8_t unknown_12[20];
  u32_be member2_vtable;
};
GUEST_OFFSET(Mode, member2_vtable, 32);

struct Modes {
  u32_be vtable;
  u32_be unknown_4;
};

// The object many of the modes' members are built on: a vtable, two words,
// and 18 points (their w set to 1 by its constructor). The race camera's
// modes (game/race_camera.h) are all built on it: the car they follow, the
// viewport they write, and the car's pose copied in each update (points[0]
// its position, points[4..7] its rotation; CameraMode::FollowCar).
struct GameObject {
  u32_be vtable;
  uint8_t unknown_4[12];
  u32_be car;       // The followed car (a vehicle), or 0.
  u32_be viewport;  // The viewport the camera writes.
  uint8_t unknown_24[8];
  Vector points[14];
  uint8_t unknown_256[4];
  u32_be unknown_260;  // The race camera keeps the TV cameras following it.
  uint8_t unknown_264[8];
  Vector points_272[4];
};
GUEST_SIZE(GameObject, 336);
GUEST_OFFSET(GameObject, vtable, 0);
GUEST_OFFSET(GameObject, points, 32);
GUEST_OFFSET(GameObject, unknown_260, 260);
GUEST_OFFSET(GameObject, points_272, 272);

// The chase camera (race camera mode 0, game/race_camera.h), also inside the
// TV cameras.
struct GameObject384 {
  GameObject object;
  uint8_t unknown_336[16];
  Vector unknown_352;
  uint8_t unknown_368[12];
  u32_be reset_behind;  // Set by the race camera's button 0x80: moves the camera behind the car.
};
GUEST_SIZE(GameObject384, 384);
GUEST_OFFSET(GameObject384, object, 0);
GUEST_OFFSET(GameObject384, reset_behind, 380);

// The hood camera (race camera mode 2); the cockpit camera's base.
struct GameObject1104 {
  GameObject object;
  uint8_t unknown_336[16];
  uint8_t unknown_352[736];  // Set up by sub_820BFB20 when the camera starts.
  Vector unknown_1088;
};
GUEST_SIZE(GameObject1104, 1104);
GUEST_OFFSET(GameObject1104, object, 0);

struct Buffer28 {
  u32_be vtable;
  uint8_t unknown_4[8];
  i32_be unknown_12;
  i32_be unknown_16;
  u32_be block;  // 32 KB, freed by Buffer28::Free.
  u32_be unknown_24;
};
GUEST_SIZE(Buffer28, 28);
GUEST_OFFSET(Buffer28, vtable, 0);
GUEST_OFFSET(Buffer28, block, 20);

struct XPRHolder {
  u32_be unknown_0;
  XPR first;
  u32_be unknown_28;
  XPR second;
};
GUEST_SIZE(XPRHolder, 56);
GUEST_OFFSET(XPRHolder, first, 4);
GUEST_OFFSET(XPRHolder, second, 32);


// The TV cameras (race camera mode 4): the trackside camera zones, and a
// chase camera kept running inside.
struct Member13360 {
  GameObject object;
  uint8_t unknown_336[24];
  i32_be zone;  // The zone whose camera is in use, -1 for none.
  uint8_t unknown_364[20];
  Vector unknown_384[3];
  uint8_t unknown_432[16];
  u32_be unknown_448;  // Not 0: the other kind of TV camera (sub_82150D70).
  u32_be zone_count;
  uint8_t unknown_456[42560];
  Buffer28 buffers[4];
  uint8_t unknown_43128[8];
  GameObject384 unknown_43136;
};
GUEST_SIZE(Member13360, 43520);
GUEST_OFFSET(Member13360, object, 0);
GUEST_OFFSET(Member13360, zone, 360);
GUEST_OFFSET(Member13360, unknown_448, 448);
GUEST_OFFSET(Member13360, buffers, 43016);

// The scripted camera (race camera mode 5): camera animations made of shots
// (the race intros and the rival intro cutscene).
struct Member56896 {
  GameObject object;
  uint8_t unknown_336[16];
  i32_be animation;  // -1 for none.
  u32_be shot_frame;
  u32_be shot;       // Steps when a shot ends.
  uint8_t unknown_364[10416];
  XPRHolder xprs;
};
GUEST_SIZE(Member56896, 10836);
GUEST_OFFSET(Member56896, object, 0);
GUEST_OFFSET(Member56896, animation, 352);
GUEST_OFFSET(Member56896, shot, 360);
GUEST_OFFSET(Member56896, xprs, 10780);

struct Entry20 {
  u32_be words[5];
};
GUEST_SIZE(Entry20, 20);
GUEST_OFFSET(Entry20, words, 0);

struct Member67760 {
  GameObject object;
  uint8_t unknown_336[16];
  FixedList<16, Entry20> list;
  u32_be unknown_748;
  u32_be unknown_752;
  u32_be unknown_756;
};
GUEST_SIZE(Member67760, 760);
GUEST_OFFSET(Member67760, object, 0);
GUEST_OFFSET(Member67760, list, 352);

struct Block1104 {
  uint8_t unknown_0[1064];
  f32_be unknown_1064;
  uint8_t unknown_1068[4];
  Vector unknown_1072;
  uint8_t unknown_1088[16];
};
GUEST_SIZE(Block1104, 1104);
GUEST_OFFSET(Block1104, unknown_1072, 1072);

// The cockpit camera (race camera mode 1): a hood camera's layout, then
// the look angle.
struct CockpitCameraObject {
  GameObject1104 camera;
  f32_be look_angle;  // Radians, from the pad's look stick (+-60 degrees).
  uint8_t unknown_1108[12];
};
GUEST_SIZE(CockpitCameraObject, 1120);

// The race mode's member: its state machine and the race camera
// (game/race_camera.h). The camera modes' objects are kept side by side; one
// (two in split screen) is the active camera.
struct RaceModeMember {
  u32_be vtable;
  uint8_t unknown_4[12];
  StateMachine machine;
  u32_be start_view;     // The view the race starts in (RaceCamera::Start).
  u32_be camera_mode;    // The active camera's mode (RaceCamera::SetMode).
  u32_be saved_view;     // The player's view, put back by RaceCamera::Restore.
  u32_be followed_racer; // The racer the camera follows (0 or 1).
  uint8_t unknown_9640[8];
  GameObject1104 hood_cameras[2];  // Mode 2, one a player.
  CockpitCameraObject cockpit_camera;  // Mode 1.
  GameObject384 chase_camera;      // Mode 0.
  Member13360 tv_cameras;          // Mode 4.
  Member56896 scripted_camera;     // Mode 5.
  uint8_t unknown_67716[28];
  Member67760 unknown_camera;      // Mode 3 (not identified).
  uint8_t unknown_68504[8];
  ptr<GameObject> active_cameras[2];  // The active camera, one a player.
  uint8_t unknown_68520[8];
  Block1104 unknown_68528[8];
  i32_be unknown_77360;
  u32_be unknown_77364;
  u32_be unknown_77368;
  u32_be unknown_77372;
  uint8_t unknown_77376[16];
  u32_be followed_controller;  // The control record the race camera reads (its low byte).
  uint8_t unknown_77396[12];
};
GUEST_SIZE(RaceModeMember, 77408);
GUEST_OFFSET(RaceModeMember, vtable, 0);
GUEST_OFFSET(RaceModeMember, machine, 16);
GUEST_OFFSET(RaceModeMember, start_view, 9624);
GUEST_OFFSET(RaceModeMember, followed_racer, 9636);
GUEST_OFFSET(RaceModeMember, hood_cameras, 9648);
GUEST_OFFSET(RaceModeMember, cockpit_camera, 11856);
GUEST_OFFSET(RaceModeMember, chase_camera, 12976);
GUEST_OFFSET(RaceModeMember, tv_cameras, 13360);
GUEST_OFFSET(RaceModeMember, scripted_camera, 56880);
GUEST_OFFSET(RaceModeMember, unknown_camera, 67744);
GUEST_OFFSET(RaceModeMember, active_cameras, 68512);
GUEST_OFFSET(RaceModeMember, unknown_68528, 68528);
GUEST_OFFSET(RaceModeMember, unknown_77360, 77360);
GUEST_OFFSET(RaceModeMember, followed_controller, 77392);


}  // namespace game
