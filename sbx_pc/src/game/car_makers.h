// The front end's garage stage (the stand that shows a car) and the
// records it's built from: the garage cars in the save data, the stage's
// car sizes, and the tuning screen's car.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"
#include "game/physics.h"
#include "game/save_data.h"

namespace game {

using guest::f32_be;
using guest::i16_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The tuning screen, as far as its car goes: the save data whose garage
// car it shows and the car data built from that car (its power curve).
struct TuningScreen {
  uint8_t unknown_0[18904];
  ptr<SaveData> save;
  uint8_t unknown_18908[4];
  CarData car_data;
};
GUEST_OFFSET(TuningScreen, save, 18904);
GUEST_OFFSET(TuningScreen, car_data, 18912);

// The garage stage: its scene model (a handle in the car scene's pool 7)
// and the shown car's sizes, which the stand's camera and floor use.
struct GarageStage {
  uint8_t unknown_0[4];
  i32_be next_model_block;  // The model block to load (CarModels::BlockOf), or -1.
  u32_be scene_model;       // A handle in the car scene's pool 7.
  uint8_t unknown_12[4];
  u32_be has_model;  // Set once a model was set up; released before the next.
  uint8_t unknown_20[6036];
  u32_be unknown_6056;  // Set to 1 when a model was set up.
  uint8_t unknown_6060[36];
  // From the car data: its +1736 and +1756, then each axle record's +52,
  // the differences, and each axle record's +208 and +212 (front, rear).
  f32_be heights[2];
  f32_be axle_heights[2];
  f32_be axle_angles[2];
  f32_be axle_values_212[2];
  f32_be angle_offsets[2];  // sin(axle angle in degrees) x axle height.
  f32_be height_differences[2];  // axle_heights - heights.
  uint8_t unknown_6144[8];
  f32_be dimensions[3];  // The car data's +4, +8, +12 (probably the wheelbase and tracks).
  uint8_t unknown_6164[12];
  Vector axle_positions[2];  // Each axle record's first three floats, w 1.0.
};
GUEST_OFFSET(GarageStage, next_model_block, 4);
GUEST_OFFSET(GarageStage, has_model, 16);
GUEST_OFFSET(GarageStage, unknown_6056, 6056);
GUEST_OFFSET(GarageStage, heights, 6096);
GUEST_OFFSET(GarageStage, axle_angles, 6112);
GUEST_OFFSET(GarageStage, angle_offsets, 6128);
GUEST_OFFSET(GarageStage, height_differences, 6136);
GUEST_OFFSET(GarageStage, dimensions, 6152);
GUEST_OFFSET(GarageStage, axle_positions, 6176);

// What holds the stage ([0x826C7E88] points at it).
struct GarageStageHolder {
  ptr<GarageStage> stage;
};

// The car dealer's list, as far as the shown car goes: the entry at
// first + offset in its table of entries.
struct CarDealerEntry {
  uint8_t unknown_0[8432];
  u32_be car_id;
};
GUEST_OFFSET(CarDealerEntry, car_id, 8432);

struct CarDealerList {
  uint8_t unknown_0[48];
  i32_be first;
  i32_be offset;
  uint8_t unknown_56[1080];
  u32_be entries;  // A table of CarDealerEntry pointers.
};
GUEST_OFFSET(CarDealerList, first, 48);
GUEST_OFFSET(CarDealerList, entries, 1136);

// An interior choice as the car keeps it (12 bytes; SetChoice turns it
// into the record's form).
struct CarInteriorChoice {
  i32_be a;
  u32_be b;
  u32_be c;  // 0: none.
};
GUEST_SIZE(CarInteriorChoice, 12);

// The car object's settings that the scene model's record is made from
// (Car + 1644: the same bytes as CarSetup's first 244, its settings before
// the spec copies; a view until CarSetup names them).
struct CarSetupWords {
  uint8_t unknown_0[16];
  u32_be unknown_16;
  u32_be unknown_20;
  uint8_t unknown_24[20];
  u32_be caliper;  // The brake calipers' choice (the record's +4124).
  u32_be unknown_48;
  uint8_t unknown_52[8];
  i32_be light_kit;  // 1-2: one kit, 3-5: two, 6: the third (the record's +3876, +3880).
  uint8_t unknown_64[80];
  u32_be unknown_144;
  uint8_t unknown_148[4];
  u32_be unknown_152[4];
  i32_be accessory;  // 3: none (then the record keeps its own).
  u32_be unknown_172;
  CarInteriorChoice choices[5];
  uint8_t unknown_236[8];
};
GUEST_SIZE(CarSetupWords, 244);
GUEST_OFFSET(CarSetupWords, caliper, 44);
GUEST_OFFSET(CarSetupWords, unknown_48, 48);
GUEST_OFFSET(CarSetupWords, light_kit, 60);
GUEST_OFFSET(CarSetupWords, unknown_144, 144);
GUEST_OFFSET(CarSetupWords, accessory, 168);
GUEST_OFFSET(CarSetupWords, choices, 176);

// A body kit's entry in the table at 0x82009318 (read-only; 24 bytes for
// each model block and kit): the aero part levels the kit sets in the
// record, -1 to keep the car's own.
struct BodyKitEntry {
  i32_be part_0;
  i32_be part_2;
  i32_be part_3;
  i32_be part_10;
  i32_be part_5;  // Unless the kit's flag 0x1 keeps the car's.
  i32_be part_4;  // Unless the kit's flag 0x2 keeps the car's.
};
GUEST_SIZE(BodyKitEntry, 24);

// The rival list screen's transition machine ([0x826C7EE4]): which level
// the screen is on, the transition running and the one queued after it.
struct RivalListMachine {
  static constexpr int32_t kNone = -1;

  i32_be level;       // 0 to 3, 4 leaving.
  i32_be next_level;  // The level a finished transition leaves the screen on.
  uint8_t unknown_8[4];
  i32_be running;  // The transition running, or -1.
  i32_be queued;   // The transition after it, or -1.
  u32_be unknown_20;
  u32_be step;  // The running transition's word: 0 on its first call.
};
GUEST_OFFSET(RivalListMachine, running, 12);
GUEST_OFFSET(RivalListMachine, step, 24);

// One of the rival list's entries (52 bytes): its build, which Car::FromBuild
// makes a car from.
struct RivalListEntry {
  uint8_t unknown_0[48];
  u32_be build;
};
GUEST_SIZE(RivalListEntry, 52);

// The rival list screen's data ([0x826C7EF0]; the game names the screen
// TXR_MenuRivalList), as far as its update reads it.
struct RivalListScreen {
  static constexpr int kEntries = 400;

  uint8_t unknown_0[0x2C784];
  RivalListEntry entries[kEntries];
  uint8_t unknown_0x318C4[0x3265C - 0x318C4];
  u32_be shows_car;    // Set: the stand shows the selected entry's car.
  u32_be car_shown;    // The stand's model was reloaded and the car set on it.
  u32_be selected;     // The entry shown.
  u32_be busy_shown;   // The stand's busy marker is shown.
};
GUEST_OFFSET(RivalListScreen, entries, 0x2C784);
GUEST_OFFSET(RivalListScreen, shows_car, 0x3265C);
GUEST_OFFSET(RivalListScreen, busy_shown, 0x32668);

}  // namespace game
