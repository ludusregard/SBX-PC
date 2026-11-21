// The car parameter tables (assets 72 to 83) as they sit in memory: the
// parameter object at [0x826B2B28], each car's 8,432-byte spec, the wheels
// and the aero parts. The tables are loaded little-endian and swapped in
// place once loaded; the aero parts' tables are swapped as they're copied
// out. Most of the spec's words are only swapped here, so they're named by
// where they sit.

#pragma once

#include "game/guest_types.h"

#include <bit>

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u16_be;
using guest::u32_be;

// A field's bytes reversed in place, its bits kept.
template <typename T>
void SwapBytes(guest::be<T>& field) {
  field.raw = std::byteswap(field.raw);
}

template <typename T, size_t N>
void SwapBytes(guest::be<T> (&fields)[N]) {
  for (auto& field : fields) {
    SwapBytes(field);
  }
}

// Three words swapped, w set to 1.0 (sub_820C94F0).
struct SpecVector {
  u32_be xyz[3];
  f32_be w;
};
GUEST_SIZE(SpecVector, 16);

struct SpecBlock264 {
  u32_be words_0[3];
  uint8_t unknown_12[4];
  u32_be words_16[3];
  uint8_t unknown_28[4];
  u32_be words_32[15];
};
GUEST_SIZE(SpecBlock264, 92);

// The spec's first block: dimensions, mass, wheelbase, tracks and the like
// (Vehicle::SetupFromCar reads it).
struct SpecBlock1 {
  uint8_t unknown_0[16];
  SpecVector vectors[5];
  u32_be matrices[2][16];
  u32_be words_224[5];
  u32_be words_244[5];
  SpecBlock264 block_264;
  u32_be words_356[3];
  u32_be words_368[40];
  u32_be words_528[28];
};
GUEST_SIZE(SpecBlock1, 640);
GUEST_OFFSET(SpecBlock1, matrices, 96);
GUEST_OFFSET(SpecBlock1, block_264, 264);
GUEST_OFFSET(SpecBlock1, words_528, 528);

struct SpecBlock2At8 {
  u32_be words_0[5];
  f32_be zeros[9];  // Set to 0.0 by the swap.
  u32_be word_56;
};
GUEST_SIZE(SpecBlock2At8, 60);

struct SpecBlock2At132 {
  u32_be words_0[10];
  u32_be groups[3][5];
  u32_be words_100[17];
};
GUEST_SIZE(SpecBlock2At132, 168);

// The spec's second block.
struct SpecBlock2 {
  uint8_t unknown_0[8];
  SpecBlock2At8 block_8;
  uint8_t unknown_68[4];
  u32_be words_72[14];
  uint8_t unknown_128[3];
  uint8_t uses_car0_parts;  // Nonzero: the car has no aero parts of its own.
  SpecBlock2At132 block_132;
  u32_be words_300[4];
  uint8_t unknown_316[24];
};
GUEST_SIZE(SpecBlock2, 340);
GUEST_OFFSET(SpecBlock2, block_8, 8);
GUEST_OFFSET(SpecBlock2, words_72, 72);
GUEST_OFFSET(SpecBlock2, uses_car0_parts, 131);
GUEST_OFFSET(SpecBlock2, block_132, 132);
GUEST_OFFSET(SpecBlock2, words_300, 300);

// An engine stage (rev limit, torque and the like).
struct EngineStage {
  uint8_t unknown_0[8];
  u32_be words[48];
  uint8_t unknown_200[4];
};
GUEST_SIZE(EngineStage, 204);

// A tire (21 front and 21 rear in a spec, by type and grade). Lengths in
// meters, the rim in inches; Tire::InchUp changes the size and grip.
struct Tire {
  uint8_t unknown_0[8];
  u32_be unknown_8[9];
  f32_be radius;  // The rim's radius plus the sidewall.
  f32_be width;
  u32_be unknown_52[3];
  f32_be grip_forward;
  f32_be grip_lateral;
  u32_be unknown_72[2];
  f32_be rim_diameter;
  f32_be aspect_ratio;
  f32_be rim_width;
};
GUEST_SIZE(Tire, 92);
GUEST_OFFSET(Tire, radius, 44);
GUEST_OFFSET(Tire, width, 48);
GUEST_OFFSET(Tire, grip_forward, 64);
GUEST_OFFSET(Tire, rim_diameter, 80);
GUEST_OFFSET(Tire, rim_width, 88);

struct Paint {
  static constexpr uint16_t kSolid = 0;
  static constexpr uint16_t kTwoShade = 1;

  u16_be type;
  uint8_t unknown_2[2];
  u16_be hue;
  uint8_t saturation;
  uint8_t unknown_7;
  u16_be unknown_8;
  uint8_t unknown_10[2];
  u16_be shade2_hue;  // The second shade: set by the swap from the type.
  uint8_t shade2_saturation;
  uint8_t shade2_value;
  u16_be unknown_16;
  uint8_t unknown_18[2];
};
GUEST_SIZE(Paint, 20);
GUEST_OFFSET(Paint, shade2_hue, 12);

struct SpecPaints {
  uint8_t unknown_0[8];
  Paint paints[13];
  u32_be unknown_268;
};
GUEST_SIZE(SpecPaints, 272);

struct WheelColor {
  u16_be unknown_0;
  uint8_t unknown_2[2];
  u16_be unknown_4;
  uint8_t unknown_6[2];
  u32_be unknown_8;
};
GUEST_SIZE(WheelColor, 12);

// A wheel: a spec's stock one, or a record of table 73.
struct SpecWheel {
  uint8_t unknown_0[8];
  WheelColor colors[2];
  u32_be unknown_32[2];
  i32_be model;  // Counted from 0 in the file, from 1 once loaded.
  u32_be unknown_44;
};
GUEST_SIZE(SpecWheel, 48);
GUEST_OFFSET(SpecWheel, model, 40);

// A car's spec (table 72).
struct CarSpec {
  SpecBlock1 block1;
  SpecBlock2 block2;
  EngineStage stages[16];
  Tire front_tires[21];
  Tire rear_tires[21];
  SpecPaints paints;
  SpecWheel stock_wheel;
  uint8_t unknown_8428[4];
};
GUEST_SIZE(CarSpec, 8432);
GUEST_OFFSET(CarSpec, block2, 640);
GUEST_OFFSET(CarSpec, stages, 980);
GUEST_OFFSET(CarSpec, front_tires, 4244);
GUEST_OFFSET(CarSpec, rear_tires, 6176);
GUEST_OFFSET(CarSpec, paints, 8108);
GUEST_OFFSET(CarSpec, stock_wheel, 8380);

// An aero part (tables 74 to 80). With no level it's the defaults: four
// 0.0, two 1.0, then 0.
struct AeroPart {
  f32_be values[4];
  f32_be scales[2];
  u32_be unknown_24;
};
GUEST_SIZE(AeroPart, 28);

// Tables 81 and 82's parts (the body kits): two more words.
struct AeroPart36 {
  AeroPart part;
  u32_be unknown_28;
  u32_be unknown_32;
};
GUEST_SIZE(AeroPart36, 36);

// A record of table 83: ten words (all 1.0 in the data; no reader found).
struct Record83 {
  u32_be words[10];
};
GUEST_SIZE(Record83, 40);

// The parameter object at [0x826B2B28] (468 bytes).
struct CarParams {
  static constexpr int kPartTables = 9;  // Tables 74 to 82.

  u32_be loads_pending;
  u32_be block;   // The one block all twelve tables are loaded into.
  u32_be wheels;  // Table 73 (SpecWheel records, 48 bytes each).
  u32_be specs;   // Table 72 (CarSpec records).
  u32_be parts[kPartTables];
  u32_be table_83;  // 40-byte records.
  u32_be spec_count;
  u32_be wheel_count;
  u32_be first_wheel_of_model[100];  // Each wheel model's first record.
  u32_be model_count;  // The last model + 1.
};
GUEST_SIZE(CarParams, 468);
GUEST_OFFSET(CarParams, block, 4);
GUEST_OFFSET(CarParams, parts, 16);
GUEST_OFFSET(CarParams, table_83, 52);
GUEST_OFFSET(CarParams, first_wheel_of_model, 64);

struct Car;

// car_specs.cpp's C++, for the boot's load and unload (data_load.cpp) and the
// garage (car_makers.cpp).

// Params::Init (sub_820C9700) and Params::Load (sub_820CC048): the
// parameter object and its block made, then the twelve tables loaded into
// it. Params::Free (sub_820C9B30): both freed.
void InitCarParams(guest::Guest& g);
void LoadCarParams(guest::Guest& g);
void FreeCarParams(guest::Guest& g);

// Params::GetCarParts (sub_820C9BA0): tables 74 to 82, at car `car`'s
// records.
void GetCarParts(uint32_t car, uint32_t parts[CarParams::kPartTables]);

// AeroParts::Get82 (sub_820CBA30): the body kit part for `level` (1 up) of
// the car's parts (`parts`, GetCarParts'), copied to `out` and swapped; with
// no level, the defaults.
void GetBodyKitPart(guest::Guest& g, AeroPart36& out, const uint32_t parts[CarParams::kPartTables],
                    int32_t level);

// Car::Refresh (sub_820C47A8): the car's working copy of its spec, tires,
// aero parts and wheels, from the tables and its settings.
void RefreshCar(guest::Guest& g, Car& car);

}  // namespace game
