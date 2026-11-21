// The race sounds' handlers as our own C++ (--native_race_sounds, on by
// default): the listener's set-up through the sound manager, and the
// handlers the race's messages call (the countdowns, the rolling start, the
// next stage, an abort, leaving after the race): the music change asked
// for, the effects' level put back, the state's bits, and the music picked.
// The levels are globals (writable, read each time). The sound manager's
// slots, the cars' sounds, the music pick and the ticks stay recompiled.
// docs/audio.md, "The race sounds".

#include "game/race_sounds.h"

#include "base/settings.h"

SBX_SETTING_BOOL(native_race_sounds, true, "Overrides", "Race",
                 "Our own C++ for the race sounds' handlers (the listener, the countdowns, "
                 "the rolling start, the next stage, an abort, leaving after the race) "
                 "instead of the recompiled functions; the sounds' ticks stay recompiled");

namespace {

using game::RaceSounds;
using game::SoundManager;
using guest::At;
using guest::Guest;
using guest::u32_be;

bool On() { return SBX_GET(native_race_sounds); }

// Writable globals.
constexpr uint32_t kSoundManager = 0x826B5960;     // [it] the AudioManager
constexpr uint32_t kCarSounds = 0x826B591C;        // [it] the seven car audio objects
constexpr uint32_t kMusicLevel = 0x82606B74;       // now (the option sets it)
constexpr uint32_t kMusicLevelSaved = 0x82606BBC;  // the fade's starting level
constexpr uint32_t kEffectsLevel = 0x82606BC0;     // now (the cars)
constexpr uint32_t kEffectsOption = 0x82606BC4;    // the option's
constexpr uint32_t kMusicFading = 0x826B5964;      // a byte
constexpr uint32_t kEffectsBankLoaded = 0x826C7E70;

u32_be& Global(uint32_t address) { return *At<u32_be>(address); }

void SetState(RaceSounds* sounds, uint32_t clear, uint32_t set) {
  sounds->state = (sounds->state & ~clear) | set;
}

// A music change asked for: the fade's starting level saved, the fading
// flag cleared, the music-started bit cleared and `bits` set (the change
// bit, with or without the cars held). The tick does the rest.
void RequestMusicChange(RaceSounds* sounds, uint32_t bits) {
  Global(kMusicLevelSaved) = Global(kMusicLevel);
  *At<uint8_t>(kMusicFading) = 0;
  SetState(sounds, RaceSounds::kMusicStarted, bits);
}

// The effects' level back to the option's.
void RestoreEffects() { Global(kEffectsLevel) = Global(kEffectsOption); }

// Every car's sounds stopped (CarSounds::StopAll, sub_8210FEC0; it takes no
// argument, r3 is left as it was) and the rival music picked (sub_821743B0),
// on the race sounds.
void StopCars(Guest& g, RaceSounds* sounds) { g.Call(sub_8210FEC0, sounds); }
void PickMusic(Guest& g, RaceSounds* sounds) { g.Call(sub_821743B0, sounds); }

}  // namespace

#define SBX_RACE_SOUNDS_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                 \
  if (!On()) {                         \
    __imp__##name(ctx, base);          \
    return;                            \
  }                                    \
  Guest g(ctx, base);

// RaceSounds::ResumeCars: CarSound::ResumeAll (sub_8210E440) seven times,
// as the original does, on car slot 0 each time (not on each slot); the
// other cars' voices restart from their update. The pointer is read each
// time.
extern "C" REX_FUNC(sub_82173B18) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173B18)
  for (int i = 0; i < 7; i++) {
    g.Call(sub_8210E440, Global(kCarSounds));
  }
}

// RaceSounds::OnEffectsBankLoaded: noted. r3 left.
extern "C" REX_FUNC(sub_82173D90) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173D90)
  Global(kEffectsBankLoaded) = 1;
}

// RaceSounds::SetListener: the sound manager's slots 26 (r4 and r5 as
// given), 25 (r4 = r3) and 27 (r4 = r6), the manager read each time.
extern "C" REX_FUNC(sub_82173760) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173760)
  const uint32_t a = ctx.r3.u32, d = ctx.r6.u32;
  auto manager = [] { return At<SoundManager>(Global(kSoundManager)); };
  g.CallVirtual(manager(), 26);
  g.CallVirtual(manager(), 25, a);
  g.CallVirtual(manager(), 27, d);
}

// RaceSounds::OnStageCountdown: the effects' level back, the cars to be
// built, the cars no longer held.
extern "C" REX_FUNC(sub_82173BE0) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173BE0)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  RestoreEffects();
  sounds->cars_set_up = 1;
  SetState(sounds, RaceSounds::kCarsHeld, 0);
}

// RaceSounds::OnAbortDone: the same, the volume last.
extern "C" REX_FUNC(sub_82173BB8) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173BB8)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  sounds->cars_set_up = 1;
  SetState(sounds, RaceSounds::kCarsHeld, 0);
  RestoreEffects();
}

// RaceSounds::OnRollingStartDone: with the cars built and not held, every
// car's sounds stopped (the held bit set, bit 0x10000 cleared); then the
// cars to be built again and no longer held.
extern "C" REX_FUNC(sub_82173C08) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173C08)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  const uint32_t state = sounds->state;
  if ((state & RaceSounds::kCarsBuilt) && !(state & RaceSounds::kCarsHeld)) {
    StopCars(g, sounds);
    SetState(sounds, RaceSounds::kBit10000, RaceSounds::kCarsHeld);
  }
  const uint32_t now = sounds->state & ~RaceSounds::kCarsHeld;
  sounds->cars_set_up = 1;
  sounds->state = now;
}

// RaceSounds::OnCountdown: CarSound::Update (sub_8210FFA0) on car slot 0,
// a music change asked for, the rival music picked.
extern "C" REX_FUNC(sub_82174828) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82174828)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  g.Call(sub_8210FFA0, Global(kCarSounds));
  RequestMusicChange(sounds, RaceSounds::kMusicChange);
  PickMusic(g, sounds);
}

// RaceSounds::OnLeaveAfterRace: the cars' tick (CarSounds::Tick,
// sub_82173DE0), a music change asked for, the effects' level back.
extern "C" REX_FUNC(sub_82174D20) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82174D20)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  g.Call(sub_82173DE0, sounds);
  RequestMusicChange(sounds, RaceSounds::kMusicChange);
  RestoreEffects();
}

// RaceSounds::OnNextStage: every car's sounds stopped, a music change
// asked for with the cars held, the rival music picked.
extern "C" REX_FUNC(sub_82174D88) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82174D88)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  StopCars(g, sounds);
  RequestMusicChange(sounds, RaceSounds::kMusicChange | RaceSounds::kCarsHeld);
  PickMusic(g, sounds);
}

// RaceSounds::OnAbort: every car's sounds stopped, a music change asked for
// with the cars held.
extern "C" REX_FUNC(sub_82173B60) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82173B60)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  StopCars(g, sounds);
  RequestMusicChange(sounds, RaceSounds::kMusicChange | RaceSounds::kCarsHeld);
}

// RaceSounds::OnRollingStart: the cars built (CarSounds::Build,
// sub_82174048) when they're to be, a music change asked for, the rival
// music picked.
extern "C" REX_FUNC(sub_82174F80) {
  SBX_RACE_SOUNDS_FALLBACK(sub_82174F80)
  RaceSounds* sounds = At<RaceSounds>(ctx.r3.u32);
  if (sounds->cars_set_up == 1) {
    g.Call(sub_82174048, sounds);
  }
  RequestMusicChange(sounds, RaceSounds::kMusicChange);
  PickMusic(g, sounds);
}
