// The race sounds: the object the race's sound messages are handled on
// (the race scene). docs/audio.md, "The race sounds".

#pragma once

#include "game/guest_types.h"

namespace game {

struct RaceSounds {
  // The state's bits. The battle music requests (0x40-0x2000,
  // Music::RequestBattle) share the word.
  static constexpr uint32_t kMusicStarted = 0x10;   // Cue (2, 0) started.
  static constexpr uint32_t kCarsHeld = 0x4000;     // CarSounds::Tick skips the cars.
  static constexpr uint32_t kMusicChange = 0x8000;  // Fade out, then load the next music.
  static constexpr uint32_t kBit10000 = 0x10000;    // Only ever cleared here.
  static constexpr uint32_t kCarsBuilt = 0x20000;   // Set by CarSounds::Build.

  uint8_t machine[9644];  // A StateMachine and more.
  guest::u32_be state;
  guest::u32_be cars_set_up;  // 1: the cars to be built; 2: built, ticking.
};
GUEST_OFFSET(RaceSounds, state, 9644);
GUEST_OFFSET(RaceSounds, cars_set_up, 9648);

// The sound manager (AudioManager) at [0x826B5960], called through its
// vtable.
struct SoundManager {
  guest::u32_be vtable;
};

}  // namespace game
