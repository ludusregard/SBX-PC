# Audio

How the game's sound works, layer by layer: the libraries, the files, the
code that loads and plays them, and where each part can be replaced.
Formats of other data are in `docs/game_data.md`; function details in
`docs/functions.md`; custom music in `docs/custom_music.md`.

Traced from the code and the bank files; labels marked "probably" come
from the code's shape and are still to be checked in play ("Not known yet"
at the end).

## The stack

```
 game code (menus, the race scene, the cars)
   |                              |
   | cues (effects, music)        | car sounds (raw voices)
   v                              v
 AudioManager 0x82117000-0x8211A000    CarSounds 0x8210CB10-0x82110180
   (33-slot vtable, 3 bank slots)        (7 car slots, own submix + reverb)
   v                              |
 XLAudio 0x825820C8-0x82582D00    |
   (Genki's XACT wrapper)         |
   v                              |
 XACT 0x825A0000-0x825BFFFF       |
   v                              v
 XAudio 0x82440000-0x82468000 (voices, mixer, X3DAudio)
   v
 render driver -> ours (--native_audio); XMA -> ours (--native_xma)
```

- There are two paths. Everything except the cars is an XACT cue played
  through the game's `AudioManager`. The cars play raw XAudio voices
  ("Car sounds" below) and never touch XACT.
- Every cue in the game is trivial: one cue, one sound, one wave, a
  category (Default or Music), a fixed volume, and (music only) an infinite
  loop. There are no RPCs, DSP presets, variations or 3D cues in use, and
  the engine sounds don't use XACT's variables.
- Movies play through our own player (`src/movies/`), not this stack.
- Voice chat has its own library (`0x8255xxxx`-`0x82564xxx`); its worker is
  skipped (`src/hooks/voice.cpp`).

## XAudio and the output

- `XAudioInitialize` (`0x82443668`) is called once, from XLAudio
  (`sub_82582450`): 6 channels, 48 kHz.
- XAudio mixes every voice on the CPU into 6-channel frames (256 samples a
  channel, big-endian floats, about 5.3 ms at 48 kHz) and hands each frame
  to the render driver, which the game reaches only through the driver
  object's vtable:

  | Function | Kernel call |
  |---|---|
  | `AudioRenderDriver::Register` (`sub_8244D0A0`) | `XAudioRegisterRenderDriverClient` |
  | `AudioRenderDriver::Submit` (`sub_8244D128`) | `XAudioSubmitRenderDriverFrame` |
  | `AudioRenderDriver::Destroy` (`sub_8244D188`) | `XAudioUnregisterRenderDriverClient` |

  These are ours (`--native_audio`, `src/audio/native_audio.cpp`): our SDL3
  output thread takes the frames instead of the SDK's Audio Worker.
- XMA, the console's compressed format, is decoded by our own code
  (`--native_xma`, `src/audio/xma_stream.*`, `src/hooks/xma.cpp`, FFmpeg's
  decoder). The game imports only `XMACreateContext` and
  `XMAReleaseContext`.
- Other audio imports: `XAudioGetVoiceCategoryVolume` and
  `XAudioGetVoiceCategoryVolumeChangeMask`.
- X3DAudio (`X3DAudioInitialize` `0x82464B20`, `X3DAudioCalculate`
  `0x82465CB8`) has two instances, both 5.1 (speaker mask 0x3F): XLAudio's
  (for cues) and the car sounds' at `0x826B5970` (set up by `sub_82110180`).
- Who makes voices: source voices (`0x82443670`) only the car sounds
  (`sub_8210D1D8`), XACT and voice chat; the one submix voice
  (`0x824436D0`) is the car sounds' (`[0x826B5924]`, "Car sounds" below).

## XACT

Microsoft's engine, linked in, with an XACT2-style interface. The engine
(`XACTCreateEngine`, `sub_825A4B68`, object at `[0x829738D8]`) has its
vtable at `0x820721A0`. The slots the game uses, all through XLAudio:

| Slot | Call |
|---|---|
| 2 | Initialize (the XGS buffer; notification callback `sub_825828C0`) |
| 4 | DoWork |
| 5 | CreateSoundBank |
| 6 | CreateInMemoryWaveBank |
| 10 | GetCategory |
| 12 | SetVolume(category, volume) |
| 13 | Pause(category, pause) |

A sound bank's slot 0 is GetCueIndex and slot 1 Prepare(index, 0, &cue).
A cue's slot 0 is Play, 1 Stop (always immediate), 2 GetState (0x04
prepared, 0x20 stopped), 3 Destroy, 15 Apply3D. Every wave bank is created
in memory (none streams), and no global variable is ever set.

## XLAudio

Genki's thin wrapper (`.\Src\XLAudio\XLAudio_Engine.cpp` in its asserts):
each call is one XACT call and an error print.

| Function | What |
|---|---|
| `sub_82582450` | `XAudioInitialize` (6 channels, 48 kHz) |
| `sub_82582510` | `X3DAudioInitialize`; 6-channel DSP settings at +0x4C |
| `sub_825828F0` | `XACTCreateEngine` and Initialize from a game file (the XGS) |
| `sub_82582578`, `sub_825826A8` | A wave bank from memory |
| `sub_82582610`, `sub_82582738` | A sound bank from memory |
| `sub_825827C8`, `sub_82582800`, `sub_82582820`, `sub_82582840` | A category: get, set volume, pause, unpause |
| `sub_825828A8` | DoWork |
| `sub_825820C8` | `Emitter::Emitter`, the cue holder (0x12C bytes): the cue at +0, its emitter, its DSP settings at +0x88 |
| `sub_82582260` | Prepare a cue (bank, index) and apply its 3D |
| `sub_82582350`, `sub_82582370` | Play, stop (immediate) |
| `sub_82582300` | Destroy the cue |
| `sub_825823A0`, `sub_825823F8` | Not stopped; prepared |
| `sub_82582860` | `X3DAudioCalculate` (flags 0x3D) for the cue |
| `sub_82582AD8` | A cue's index by name |
| `sub_82582B60` | `Listener::Listener`: front +0, top +0xC, position +0x18, velocity +0x24, distances +0x30, +0x34 |

## AudioManager

The game's sound API: one object, 0x658 bytes, made at boot
(`sub_82119D08` from `sub_82094BB8`, with the engine name `txr_xact` and 3
bank slots), kept at `[0x826B5960]` (set by `sub_82110180`). Vtable
`0x82008318`.

Layout: +0xC the app; +0x10 the XLAudio engine; +0x5B0 the listener;
+0x5EC, +0x5F0 the bank slots (a vector of `BankPair*`); +0x5F8 the playing
one-shots (a list); +0x604 the handle sounds (a list); +0x610 the next
handle; +0x614 the name; +0x630 the XGS load task.

The constructor (`sub_821199B0`) queues the XGS load (`sub_82118818`:
`data\audio\%s.xgs`, so `TXR_XACT.XGS`) and joins the run loop: its update
`sub_82119288` (DoWork, every sound's update, finished sounds dropped) is a
tick handler, so it runs every pass of the run loop (every rendered frame
with subtick), and `sub_821192F8` listens for app events (on 5 it releases
every sound; on 6 it registers again: suspend and resume, probably).

| Slot | Function | What | Callers |
|---|---|---|---|
| 0 | `sub_82119620` | Destructor | |
| 1 | `sub_82118020` | Any slot still loading | |
| 2 | `sub_82118158` | A slot by name | |
| 3 | `sub_821180E0` | The first free slot | |
| 4 | `sub_82118220` | Load(slot, name, flag, wave id, sound id) | `Bgm::Load` |
| 5 | `sub_821182F8` | Release a slot | 14 |
| 6 | `sub_82118368` | Slot still loading | 12 |
| 7 | `sub_821183D8` | Slot loaded | |
| 8 | `sub_82118448` | Slot empty | 7 |
| 9 | `sub_82118488` | A cue's index by name | |
| 10 | `sub_82119808` | Play a cue, 2D (slot, cue, volume) | `MenuSound::Play`, `Sound::Play` |
| 11 | `sub_821197D8` | Play a cue, 3D | none found |
| 12 | `sub_82118BC0` | Stop a slot's one-shots | none found |
| 13 | `sub_82118C60` | Stop a cue (slot, cue) | 47 |
| 14 | `sub_82118D28` | Is a cue playing (slot, cue) | 38 |
| 15 | `sub_82119860` | Play with a handle (returns its id) | none found |
| 16-22 | `sub_82118F60`-`sub_821190D0` | A handle's position, (nothing), 3D flag, both, start, stop, release | none found |
| 23 | `sub_82118E40` | Release every handle | `sub_821192F8` |
| 24 | `sub_82119220` | Update the handles | |
| 25-27 | `AudioManager::SetListenerPosition`, `SetListenerOrientation`, `SetListenerVelocity` | The listener | `RaceSounds::SetListener` |
| 28 | `sub_82117F88` | The listener's distances | |
| 29 | `sub_82117FD0` | A category by name | volume and pause code |
| 30 | `sub_82117FD8` | A category's volume | the volume options, the fades |
| 31, 32 | `sub_82117FE0`, `sub_82117FE8` | Pause, unpause a category | the race pause |

"None found" means no call through `[0x826B5960]`: library code the game
doesn't use, probably.

- **BankPair** (0x70 bytes, `sub_82117E70`; its load `sub_82117A30`): +0x24
  the XLAudio engine, +0x28 the name, +0x44 a flag (0 in every call), +0x48
  and +0x4C the wave and sound bank holders, +0x50 the slot, +0x54 and +0x58
  the wave and sound bank archive ids, +0x5C and +0x60 their sizes, +0x64
  and +0x68 their buffers, +0x6C and +0x6D the loaded flags (set by the
  archive callbacks `sub_821178E0`, `sub_82117938`; for slot 2
  `sub_82117990`, `sub_821179E0`). Slot 2 reads into fixed buffers
  (`[0x826B5968]`, 3 MB, the wave bank; `[0x826B596C]`, 512 bytes, the
  sound bank; both allocated by `sub_82110180`); the other slots allocate
  for each load.
- **Sound** (0x18 bytes, `sub_821174B0`): +0 the bank interface, +4 the
  cue holder, +8 3D, +9 play again when stopped, +0xA active, +0xB start
  pending, +0xC its handle, +0x10 the slot, +0x14 the cue. Its update
  (`sub_821175A0`) plays it once prepared, applies its 3D, and prepares and
  plays a stopped sound again when +9 is set.
- **`AudioManager::PlayCue`** (`sub_821196D0`) makes a Sound, sets it up,
  runs its update once and starts it; it's dropped once it stops. With
  subtick it starts nothing on menu redraw passes
  (`src/subtick/per_frame_logic.cpp`).

### The bank slots

| Slot | Bank | Loaded | Cues |
|---|---|---|---|
| 0 | `SB_SYSTEM_SE` (22885, 22886), kept for the whole run | At boot (`sub_82110180`) | 20 effects |
| 1 | `BGM_SYSTEM` (22987, 23005), the menu music | By `sub_82107A38` (every Draw pass) while the game mode at `0x825F3264` is a front-end mode (2, 4-10, 12); released when it isn't | 4 songs |
| 2 | One `bgm_tmp_NN` at a time | By `Bgm::Load` from the race scene and the music select | cue 0 |

The game modes (`sub_821078D0`, a mode's object by its id): 1 the race,
2 the garage, 4 the title, 5 the parking area (its handlers sit with
`ParkingArea::DrawScene`), the others not identified yet.

## The files

The banks are in `BUILD.DAT`, except four that are loose files on disc
(the loose-file table, see `docs/game_data.md`):

| Id | File | What |
|---|---|---|
| 22884 | `DATA/AUDIO/TXR_XACT.XGS` (loose) | Global settings |
| 22885, 22886 | `DATA/AUDIO/SB_SYSTEM_SE.XSB`, `.XWB` (loose) | Menu and HUD effects |
| 22887 | `DATA/AUDIO/CAR_SE.XWB` (loose) | Car effects |
| BUILD 22888-22928 | `<sound>_off` wave banks | Engine sounds, the base layer (41) |
| BUILD 22929-22969 | `<sound>_on` wave banks | Engine sounds, the throttle layer (41, same order) |
| BUILD 22970-22986 | `bgm_tmp_01`-`17` sound banks | Music |
| BUILD 22987 | `BGM_SYSTEM` sound bank | Menu music |
| BUILD 22988-23004 | `bgm_tmp_01`-`17` wave banks | Music |
| BUILD 23005 | `BGM_SYSTEM` wave bank | Menu music |

The engine sound sets, in order: `AE86_RED`, `CELESIOR`, `CROWN_A`,
`FC3S`, `IMP_NORM`, `IMP_TUNE`, `JZA70_SOARA`, `JZA80`, `MARKX`, `R32`,
`R32_TUNE`, `RX8`, `S13_TUNE`, `S16`, `S16_WHITE`, `V36_SKYLINE`, then
the generic sets `OP_1`-`5`, `RO_1`-`5`, `S4_1`-`5`, `S6_1`-`5`,
`V6_1`-`5` (names from each bank's header).

`DATA/AUDIO/` on the disc also has loose copies of every bank above, plus
some the archive doesn't have: `bgm_tmp_18`, `CELESIOR.xwb` (no
`_on`/`_off`), `Wave Bank.xwb`, `Sound Bank.xsb` (one cue,
`test128sample`: a tool test) and `Sound Bank 2.xsb`. The game reads the
archive copies.

### Formats

**`TXR_XACT.XGS`** (`FSGX`): three categories, Global, Default (the
effects) and Music; seven variables, all XACT's own (NumCueInstances,
AttackTime, ReleaseTime, OrientationAngle, DopplerPitchScalar,
SpeedOfSound, Distance). No RPCs, DSP or compression presets.

**Sound banks** (`KBDS`, version 37, big-endian, packed):

| Offset | What |
|---|---|
| +0x04 | Version (u16), then a CRC (u16) |
| +0x08 | Platform (u8, 3) |
| +0x09 | Simple cues (u16) |
| +0x0B | Complex cues (u16) |
| +0x11 | Wave banks (u8) |
| +0x12 | Sounds (u16) |
| +0x18 | Cue name table length (u16) |
| +0x1A | Simple cue table (u32 offset): 5 bytes a cue, flags then the sound's offset |
| +0x1E | Complex cue table (u32, -1 for none) |
| +0x22 | Cue names (u32): zero-ended, in cue order |
| +0x32 | Wave bank names (u32): 64 bytes each |
| +0x3E | Sounds (u32) |
| +0x4A | The bank's name (64 bytes) |

A sound: flags (u8, bit 0 complex), category (u16), volume (u8), pitch
(s16), priority (u8), length (u16); a simple sound then has its wave index
(u16) and wave bank (u8), a complex one its tracks. Every bank in the game
reads with this.

**Wave banks** (`DNBW`, version 37, big-endian): five regions at +0x08
(offset, length each): the bank data (flags, entry count, name at +8,
entry size at +72), the entries, two unused, the wave data. An entry is 40
bytes: flags and duration, the mini format (codec in bits 0-1: 0 PCM, 1
XMA; channels in bits 2-4; rate in bits 5-22), the play offset and length
in bytes, the loop start and length (samples for PCM), 16 zero bytes. The
engine banks and `car_se` are 16-bit mono PCM at 44.1 kHz; the music is
stereo XMA (one wave a bank, about a minute, with loop points); the menu
effects are mono XMA.

## The game's sounds

### Menu and HUD effects (slot 0)

`MenuSound::Play(cue)` (`sub_82110318`) plays slot 0's cue at volume 1.0.
Every `SB_SYSTEM_SE` cue is category Default, volume byte 90, one mono XMA
wave of the same index, no loop. The callers, by cue (constant arguments
only; 6 calls pass a variable):

| Cue | Name | Length | Calls | Where (probably) |
|---|---|---|---|---|
| 0 | `se00` | 0.42 s | 206 | Confirm (the only sound of the OK-only dialogs, `LoadPage::Note`, `SavePage::Note`) |
| 1 | `se01` | 1.45 s | 1 | The title screen's start (`TitleScreen::Update`) |
| 2 | `se02` | 0.73 s | 146 | Cancel, back |
| 3 | `se03` | 0.37 s | 14 | |
| 4 | `se04` | 0.15 s | 227 | Cursor move (with 0 in the yes/no dialogs) |
| 5 | `se05` | 1.07 s | 50 | Page changes |
| 6 | `se06` | 0.24 s | 2 | The pause menu opening (`sub_8220D7C0`, `sub_8220F118`) |
| 7 | `se07` | 0.48 s | 1 | The race HUD (`sub_822124F8`) |
| 8 | `se08` | 0.88 s | 2 | The countdown (`RaceHud::RunCountdown`, `sub_82213B90`) |
| 10 | `se10` | 0.80 s | 1 | The countdown's end (`sub_822107D0`, which also waits on it) |
| 12, 13, 15 | `se12`, `se13`, `se15` | 0.92, 0.32, 0.87 s | 2 each | The garage stage (`sub_82177E40`-`sub_82178220`): doors, hood |
| 9, 11, 14, 16-19 | `se09`, `se11`, `se14`, `se28`-`se31` | | 0 | No constant caller |

### Menu music (slot 1)

`Sound::Play(slot, cue)` (`sub_82173710`) with slot 1: cue 0 `COURSEIN`
(5 calls), 1 `GARAGE` (4), 2 `PA` (9), 3 `TITLE` (6). Screens wait for the
bank with slot 6 and stop or check songs with slots 13 and 14. Each song
is a complex sound with one track: one stereo XMA wave, category Music,
volume byte 101, looping.

`BGM_SYSTEM`'s waves (offsets from the wave data's start, 0x800 into the
file; the uses are from play, the cue names don't obviously match them):

| Wave | Wave data (offset, length) | Length | CRC-32 of its first 2048 bytes | Used for (from play) |
|---|---|---|---|---|
| 0 | 0x0, 0x1BD000 | 61.8 s | `E3A47B25` | course select |
| 1 | 0x1BD000, 0xBA800 | 27.2 s | `E66C2C17` | main menu |
| 2 | 0x277800, 0x18F800 | 49.0 s | `C448EE59` | other menus (dialogs, garage, car select) |
| 3 | 0x407000, 0x12B000 | 39.0 s | `4FEB9978` | dialog after some story events |

### Highway and race music (slot 2)

Every change of music bank is a call to `Bgm::Load` (the menu songs are
the exception, above). Its callers, by return address (custom music uses
these as its categories, `docs/custom_music.md`; the labels are from the
code):

| Return address | Function | Music | Category |
|---|---|---|---|
| `0x82107B38` | `sub_82107A38` | `BGM_SYSTEM` (slot 1, once) | MENU_COURSE, MENU_MAIN, MENU_GENERAL, MENU_STORY (told apart by the sound's data) |
| `0x82173D80` | `Music::PlayRoaming` (`sub_82173C70`) | `bgm_tmp_06`, or one of 07, 01, 15, 05, 10 at random | ROAMING |
| `0x8217424C` | `Music::PlayRequested` (`sub_821741B8`), bit 0x40 | `bgm_tmp_07`, 01 or 15 by the course variant (`[[0x826B580C]+32]`: 0, 1, 2) | BATTLE |
| `0x8217427C` | the same, bit 0x100 | `bgm_tmp_10` | BATTLE |
| `0x821742AC` | the same, bit 0x80 | `bgm_tmp_05` | BATTLE |
| `0x821742DC` | the same, bit 0x200 | `bgm_tmp_03` | BOSS |
| `0x8217430C` | the same, bit 0x400 | `bgm_tmp_09` | BOSS |
| `0x8217433C` | the same, bit 0x800 | `bgm_tmp_17` | BOSS |
| `0x8217436C` | the same, bit 0x1000 | `bgm_tmp_11` | BOSS |
| `0x8217439C` | the same, bit 0x2000 | `bgm_tmp_16` | BOSS |
| `0x82174C84` | `sub_82174AD0` | `bgm_tmp_14` if the race was won, else `bgm_tmp_08` (entry 11 or 12 of the table at `0x82609334`) | RACE_WIN or RACE_LOSE, by the name |
| `0x82262678` | `sub_822624F0` | `bgm_tmp_14` (the win music) | RACE_WIN |
| `0x822626B0` | the same | `bgm_tmp_08` | RACE_LOSE |
| `0x821B8788` | `sub_821B8580` | any of the 17, by a setting (`[[r31+32544]+740]`): the music select | never replaced |
| `0x82110280` | `sub_82110180` | `SB_SYSTEM_SE` (slot 0, at boot) | never replaced |

- **The random roll** is in `Music::PlayRoaming`: by the play mode at
  scene+9652 (the race data's play mode, +827888, copied there by
  `sub_82175238`; 4 is split screen, 5 a replay), play modes 2, 4, 5 and 6
  pick entry 1-5 of the table at `0x82609334` with `sub_8258F558() % 5`;
  others play entry 0.
- **The table at `0x82609334`**: 13 name pointers, entries 0-12:
  `bgm_tmp_06`, 07, 01, 15, 05, 10, 03, 09, 17, 11, 16, 14, 08.
- **Battle music** is requested by `Music::RequestBattle` (`sub_821743B0`)
  as a bit at scene+9644, by the opponent: its record (`sub_821172E0`) byte
  +35 is the rival's class. No opponent, class 0 or above 5: 0x40; class
  1: 0x100; class 2: 0x80; class 3: 0x400 for rivals 383-390, 0x800 for
  392-397, else 0x200; class 4: 0x800 for rival 391, 0x1000 for 398, else
  0x400; class 5: 0x2000. `Music::PlayRequested` plays it and clears the
  bit.
- **The change itself** runs on the tick, in the race sounds' state word
  (scene+9644, the same word as the requests), in every state with a tick
  (`sub_82174668` and its copies, "The race sounds" below):
  1. 0x8000, a change asked for: fade (`sub_821737C8`: the music level
     `0x82606B74` goes down by its saved level x 0.1 a tick, so about 10
     ticks), then Stop cue (2, 0); 0x20 set.
  2. 0x20: once cue (2, 0) has stopped, slot 2 released and the level put
     back (`sub_82173888`); 0x2 set.
  3. 0x2: once slot 2 is empty, 0x4 set.
  4. 0x4: the next bank loaded into slot 2 by the state's pick
     (`Music::PlayRoaming` on the highway, `Music::PlayRequested` in a
     battle, between stages and in a rolling start; after a battle the tick
     loads the win or lose music itself).
  5. Once slot 2 has loaded and 0x10 isn't set: `Sound::Play(2, 0)`; 0x10
     set (the music started).

  Every race music bank holds one cue, so the music is always cue 0 of
  slot 2.

### The race sounds

The race scene's sound handlers run on the scene (r3; `RaceSounds` in
`src/game/race_sounds.h`), registered by `sub_82175238` as (state,
message) -> handler, next state. Message 0 is the tick. The state names
are worked out from the moves:

| State | What (probably) | Tick | Messages: handler -> next state |
|---|---|---|---|
| 1 | Set-up | | 4: `sub_821744D8` (loads `CAR_SE.XWB`, starts the highway music) -> 38, or 24 in a replay (play mode 5) |
| 38 | Driving on the highway | `sub_82174668` | 26 `OnCountdown` -> 23; 14 `OnRollingStart` -> 10; 34, 36 `sub_82174A60` -> 30; 29 pause -> 24; 31 stop the cars -> 24; 49 stop the cars -> 37; 44 `OnAbort` -> 32 |
| 23 | A battle | `sub_82174890` | 34, 36 the battle over -> 30; 19 the battle over -> 38; 29 pause -> 25; 31, 58 stop the cars -> 25 |
| 30 | After a battle | `sub_82174AD0` | 40 `OnLeaveAfterRace` -> 38; 49 the same -> 37; 42 `OnNextStage` -> 17; 44 `OnAbort` -> 32; 31 stop the cars |
| 17 | Between stages | `sub_82174DE8` | 26 `OnStageCountdown` -> 23 |
| 10 | A rolling start | `sub_82174FF0` | 10 `OnRollingStartDone` -> 23; 31 stop the cars; 32 resume |
| 24, 25 | Paused (from 38, 23) | none | 29 unpause, 32 (and 58 in 25) resume the cars -> 38, 23; 44 `OnAbort` -> 32; 49 -> 37; 14 `OnRollingStart` -> 10 |
| 32 | After an abort | | 4 `OnAbortDone` -> 38 |
| 37 | Leaving the highway | `sub_82173900` (the music stopped, the cars paused, once) | 50 `sub_82175198` (the car sounds torn down, `sub_8210FE48`) -> 35 |

- **Pause** (message 29, `sub_821739A8`): every car slot paused
  (`CarSound::PauseAll`) and the Music category paused (slot 31). Unpause
  (`sub_82173A40`) resumes them (slot 32). The effects are never paused.
- **Stop the cars** (31, 49, 58; `sub_82173AC0`): every car slot paused,
  the music left playing. The resume (`RaceSounds::ResumeCars`,
  `sub_82173B18`) calls `CarSound::ResumeAll` on slot 0 seven times, not
  once for each slot; the other cars come back because their update starts
  any voice that should be playing.
- **The battle over** (`sub_82174A60`, messages 34, 36, 19): the effects level `0x82606BC0` x 0 (the cars go silent), a music
  change asked for (0x8000).
- **The system UI:** while `[0x826B7420]` or `[0x826B7424]` is set (the
  sign-in UI or a system message box, set by `sub_8214D900` around
  `XamShowSigninUI` and `XamShowMessageBoxUI`), the car tick pauses every
  car and resumes them after (`sub_82173DE0`, flag scene+9640).
- **Other state bits** (scene+9644): 0x4000 holds the cars (the car tick
  skips them; set with a change by `OnNextStage` and `OnAbort`, cleared by
  `OnStageCountdown`, `OnAbortDone` and `OnRollingStartDone`); 0x20000 the
  cars' sounds built (set by `sub_82174048`). scene+9648 is 1 when the cars
  are to be built, 2 once they run. scene+9656 delays the car tick.

### Car sounds

The cars don't use XACT. Each race car has a car audio object that plays
the engine banks and `car_se` on raw XAudio voices (create `0x82443670`,
submit with loop `0x82442FB8`, play `0x82443070`, stop `0x824430B8`,
destroy `0x82442D80`, state `0x82442F08`, volume `0x824431D0`, pitch in
octaves `0x82443248`, output levels `0x82442EB0`).

- **Set-up:** `CarSounds::Init` (`sub_8210FC00`, from `sub_82175238`)
  makes 7 car audio objects at `[0x826B591C]` (0x4F0 bytes each) and the
  car submix at `[0x826B5924]` (6 channels, 48 kHz, its effect from the
  preset at `0x826065C0`). `sub_82175238` also allocates each slot's two
  engine bank buffers (`0x826C7E1C`, 0x6B080 bytes; `0x826C7E00`,
  0x5D2D0). Torn down by `sub_8210FE48`.
- **Which banks:** `SceneModel::SetUp` (`sub_820DB0C8`) loads a car's
  engine banks by its model kind (`src/game/scene_models.cpp`): 2 the
  player's car, 1 another race car (each by its car's sound record, the
  tier from its parts record), 3 traffic (always record 47). It loads them
  only in a race scene outside game mode 5, so the parking area's cars stay
  silent. Which set each car gets: below.
- **The tick:** `sub_82173DE0`, once a 30 Hz tick from the race sounds'
  ticks in states 38, 23 and 30 (not between stages or in a rolling start): it waits out scene+9656, skips the cars while 0x4000 is
  set or 0x20000 isn't, builds any car whose banks just loaded (its objects
  `sub_8210CB10`, its parts `sub_8210E120`, its curves `sub_8210CEE8`), then
  runs the car's update `sub_8210FFA0` for every race car. So with subtick
  the engine pitch steps once a tick.
- **The update** (`sub_8210FFA0`): the 3D step (`sub_8210E4D8`), the
  engine (`sub_8210E958`, two banks of four looped layers crossfaded and
  pitched by rpm and throttle), then the car_se parts. The submix's effect
  swaps to the preset at `0x82606680` while car+5314 bit 1 is set (a
  tunnel, probably).
- **Volume:** the 3D step multiplies every output level by the effects
  level `0x82606BC0` (in the cockpit view, 0.4 x it, unpositioned). The
  Default category doesn't reach the cars.
- **`car_se`**, by the code that plays each wave: 0 a loop while car+2424
  is set, 1 tire squeal (two voices), 2 road noise, 4 a steady tone
  (car+2944), 5 the blow-off, 6 wind, 8 the car+2960 event, 9-11 impacts (9
  + the strength / 3000, up to 2), 12 a scrape (looped), 13-15 the horn (13
  + the car's horn type, held on control bit 0x20000), 17 the pass-by.
  Waves 3, 7, 16 and 18 are never played.

#### The engine banks

Each set is two banks, `<set>_off` (22888 + i) and `<set>_on` (22929 + i),
of 4 waves each: 16-bit mono PCM, 44.1 kHz, about 0.8-1.1 s, each looped
(the loop region in the wave entry). Wave k is layer k, recorded at about
1000, 3000, 5000 and 7000 rpm (S4_5, an L4: firing frequencies of about 32,
102, 162 and 230 Hz).

The `_off` bank is the base and always plays; the `_on` bank is mixed in by
rpm and throttle. The `_off` waves have almost nothing above 2 kHz. In the
named sets (AE86_RED, CELESIOR, JZA80 ...) each `_off` wave is the same take
as its `_on` wave (same length and loop, correlated at a 1-2 ms lag): a
second mic or a heavy low-pass. The generic sets use separate takes. The
generic family names follow the spec's engine types: OP = H4, RO = rotary,
S4 = L4, S6 = L6, V6 = V6; there is no V8 family.

#### The curve blocks (`0x825F3350`-`0x826059FF`)

One 1640-byte block a set, 0x668 apart:

| Offset | What |
|---|---|
| +0 | u32, 65-127: the set's volume, / 128 |
| +4 | The `_on` bank's layers, then a -1 tag |
| +824 | The `_off` bank's layers, then a -1 tag |

A layer is 136 bytes: +0 a tag, 60-63 (layer 0-3); +4 the pitch range
(1600 = 1 octave, 2133, 3200 = 2); +8 the volume curve, up to 8 (x, volume)
float pairs ended by x = -1.0; +72 the pitch curve, the same. x is rpm
scaled to the car's maximum, 0 to 8000. Pitch 8191 is the recorded pitch;
each pitch curve passes it at its layer's recording point (1000, 3000,
5000, 7000; CELESIOR, CROWN_A and V36_SKYLINE have layer 3 at 6000, OP_2-OP_5
layer 0 at 0).

#### Which set a car gets

All in code; nothing comes from the car specs but the car id.

1. The car id at car+12: the index of its car spec record (0 RX7 ... 71
   WON06).
2. `sub_82159880` (from `Car::TickUpdate` through `sub_8215A0E0`) builds the
   car's set-up at car+16: its kind, its **sound record** from
   `sub_82158AD0(car id)` (a switch, the byte table at `0x82009678`), and
   (the player) its engine stage, car+1652.
3. `SceneModel::SetUp` (`sub_820DB0C8`, `src/game/scene_models.cpp`) picks
   the record and tier: traffic record 47 tier 0; race cars their record,
   with a record under 18 tiered by the engine stage (table at
   `0x820050F8`: stage 1-3 tier 0, 4-5 tier 1, 6-7 tier 2, 8-9 tier 3, 10
   tier 4, the same steps where the rev limit rises), else tier 0.
4. The record table at `0x826062A0` (50 records of 16 bytes: +0 a list of
   curve block pointers, one a tier; +4 the entries + 1; +8 the `_on` bank
   ids; +12 the `_off` bank ids) gives the banks and the curve block
   (`sub_8210CEE8` stores the tier's halves in the car audio object, +496
   and +500 the `_on` half, +412 and +416 the `_off` half).

| Id | Car | Engine | Record | Sets (tier 0 > 4) |
|---|---|---|---|---|
| 0 | RX7 | RO | 0 | FC3S, RO_2, RO_3, RO_4, RO_5 |
| 1 | RX8 | RO | 1 | RX8, RO_1, RO_2, RO_3 |
| 2 | CON_X | L4 | 18 | S4_5 |
| 3 | ECLI | V6 | 19 | V36_SKYLINE |
| 4 | EVO9 | L4 | 2 | S4_1-S4_5 |
| 5 | N180 | L4 | 4 | S13_TUNE, S4_1-S4_4 |
| 6 | CIMA | V8 | 3 | V6_1-V6_5 |
| 7 | Z33 | V6 | 10 | V6_1-V6_5 |
| 8 | Z32 | V6 | 9 | V6_1-V6_5 |
| 9 | S30 | L6 | 21 | V6_5 |
| 10 | SIL15 | L4 | 5 | S16_WHITE, S16, S4_2-S4_4 |
| 11 | SKY35 | V6 | 8 | V6_1-V6_4, V36_SKYLINE |
| 12 | SKY34 | L6 | 7 | S6_2, S6_3, S6_5, R32_TUNE, R32 |
| 13 | SKY32 | L6 | 6 | S6_2, S6_3, S6_5, R32_TUNE, R32 |
| 14 | IMP_GDB | H4 | 12 | IMP_NORM, OP_3, OP_4, OP_5, IMP_TUNE |
| 15 | IMP_GC8 | H4 | 11 | IMP_NORM, OP_3, OP_4, OP_5, IMP_TUNE |
| 16 | ARISTO | L6 | 13 | V6_1-V6_5 |
| 17 | CEL | V8 | 14 | CELESIOR, V6_2-V6_5 |
| 18 | L86 | L4 | 20 | AE86_RED |
| 19 | CROWN | V6 | 15 | CROWN_A, V6_1-V6_4 |
| 20 | T86 | L4 | 22 | AE86_RED |
| 21 | SUPRA80 | L6 | 17 | S6_1-S6_4, JZA80 |
| 22 | SUPRA70 | L6 | 16 | S6_1-S6_4, JZA80 |
| 23-34 | BOSS00-BOSS11 | | 23-34 | S6_5, JZA80, IMP_TUNE, IMP_TUNE, RO_5, RX8, JZA70_SOARA x3, V6_5, RX8, RO_5 |
| 35-46 | BOSS12-BOSS23 | | 35 | S4_5 |
| 47-50 | LEAD01, 02, 04, 06 | | 41-44 | AE86_RED, AE86_RED, V36_SKYLINE, S4_5 |
| 51-54 | WON01-WON04 | | 36-39 | RX8, V6_5, AE86_RED, V36_SKYLINE |
| 55-70 | Traffic (MK100 ... QUON) | | road cars' records | record 47 (S4 family) in play |
| 71 | WON06 | V8 | 40 | S6_5 |

Records 45-49 are the generic families (OP, RO, S4, S6, V6); only 47 is
reached.

#### The car audio object (`sub_8210FFA0`, the car at +400)

| Object offset | Function | Sound | Driven by |
|---|---|---|---|
| | `sub_8210E4D8` | 3D (X3DAudio) | The camera and the car; DSP settings at +176 (Doppler +204) |
| +408 | `sub_8210E958` | The engine | rpm, throttle |
| +652 | `sub_8210EAB0` | `car_se` 1, two looped voices | Tire slip (`sub_820C00C8`, `sub_820C0148`) |
| +584 | `sub_8210FF18` | `car_se` 9-12 | Collisions (`sub_820B7B30`, `Contact::Strength`) |
| +952 | `sub_8210F998` | `car_se` 4 | car+2944 (turbo whine, probably) |
| +1136 | `sub_8210F538` | `car_se` 17 | The pass-by flag +1260 (set by the 3D step) |
| +736 * | `sub_8210F0E0` | `car_se` 5 | car+2948, car+2952, x 0.6/0.75/1.0 by the engine stage: the blow-off |
| +840 * | `sub_8210F1D8` | `car_se` 8 | car+2960, car+2504 (a shift or backfire, probably) |
| +888 * | `sub_8210F390` | `car_se` 0 (looped) | car+2424: fades in and out, fixed pitch |
| +792 * | `sub_8210DEB8` | `car_se` 2 (looped) | Speed (`Vehicle::GetSpeedKmh`): road noise |
| +1016 * | `sub_8210F260` | `car_se` 6 | Speed: wind |
| +1072 * | `sub_8210F5C8` | `car_se` 13-15 (looped) | The horn, with Doppler |

\* The player's car only (car+6148 & 0x81).

#### The engine (`sub_8210E958`)

- x = rpm (`sub_820BFF78`) / the car's maximum (`[[car+272]+1280]`) x 8000,
  at most 8000, so the curves fit every rev limit.
- Throttle: car+288, or car+284 when car+2252 is -1, or 0 while shifting
  (car+2248, or car+2268 > 0 with car+2256 0); smoothed by 0.25 a tick.
- The `_off` bank at gain 1.0; the `_on` bank at (x / 8000) x (0.3 + 0.7 x
  throttle).
- A layer's volume = the set's volume / 128 x its curve(x) x the bank's gain
  x the car gain (+1252) x 0.4, started once above 0; its pitch in octaves
  = (curve(x) - 8191) / 8192 x (range / 1600), plus the Doppler.
- The output levels: X3DAudio's matrix x the effects level; in the
  cockpit view (camera mode 1, `[[0x826B29C8]]+1860`) the player's car is
  played centered on all six channels at 0.4 x the effects level.

## Volumes

| Global | What | Written by |
|---|---|---|
| `0x82606BC4` | The effects option's gain | `SoundOptions::SetEffectsVolume` |
| `0x82606BC0` | The effects level now (the cars) | The same; set back from `0x82606BC4` by the race handlers; x 0 when a battle ends |
| `0x82606B74` | The music level now | `SoundOptions::SetMusicVolume`, the fades |
| `0x82606BBC` | The music level saved before a fade | The race handlers |
| `0x826B5964` | The music fading (a byte) | The race handlers |

The options also set the XACT categories: the effects option `Default`,
the music option `Music` (slot 30). A setting 0-15 is the gain setting /
15. Nothing sets `Global`.

## Replacing it

The seams, in the order that keeps the game running at each step:

1. **A cue player of our own behind `AudioManager`.** The game only uses
   slots 4-8, 10, 13, 14 and 25-32 and the update; give them our own C++
   (bank slots that read the XSB and XWB above, one voice a playing cue in
   our mixer, XMA through `xma_stream`, the three categories with volume
   and pause, music looping on the wave's loop region). XACT and XLAudio
   then go unused, and custom music moves from under XACT's voice to the
   bank slot.
2. **The cars**: our own car audio at the car tick and `CarSounds::Init`
   (the vehicle audio plan), the submix and its effect our own bus.
3. **XAudio off**: nothing else makes a voice, so XAudio, X3DAudio and the
   SDK's XMA path go unused and the render driver is our mixer's only
   output.

The game's own sound logic (banks, cues, fades, pause, the race messages)
only talks to the slots in step 1, so it keeps working unchanged.

## Not known yet

- The menu effects' labels (from listening, or a log of `MenuSound::Play`
  with the screen).
- The messages' and states' names above (from the moves only).
- Game modes 3, 6-12 and play modes 2 and 6.
- What `sub_821192F8`'s app events 5 and 6 are.
- Replays: whether their cars play engine sounds as in a race.
