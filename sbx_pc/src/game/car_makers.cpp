// The garage's cars and stage as our own C++ (--native_car_makers, on by
// default): a garage car's reset and the garage cars' static set-up; the
// garage stage's car (a car object built in the frame, its scene model's
// record filled from it, the stage's car sizes, the model's reload and the
// record's re-applies); a scene model record's paint, wheels, interior
// choices, parts, decals and part order from a car
// (SceneModelRecord::FromCar); the tuning screen's car data; the
// stand's busy marker.
//
// The scene pools, the scene models' applies and loads, the car physics'
// set-up and the UI movies stay recompiled (or their own subsystems' C++),
// called from here.

#include "game/car_makers.h"
#include "base/settings.h"
#include "game/car_spec.h"
#include "game/race_setup.h"
#include "game/scene_models.h"

#include <bit>
#include <climits>
#include <cmath>

SBX_SETTING_BOOL(native_car_makers, true, "Overrides", "Cars",
                 "Our own C++ for the garage cars' resets, the garage stage's car (its set-up, "
                 "sizes, model reload and re-applies), a scene model record's paint, wheels, "
                 "choices, parts, decals and part order from a car, the tuning screen's car data and the "
                 "stand's busy marker instead of the recompiled functions");

namespace {

using game::Car;
using game::CarData;
using game::CarDealerEntry;
using game::CarDealerList;
using game::CarOptions;
using game::CarSetupWords;
using game::GarageStage;
using game::GarageStageHolder;
using game::Paint;
using game::SaveGarage;
using game::SavedCar;
using game::SceneCarRecord;
using game::SceneModel;
using game::SceneModelRecord;
using game::SpecWheel;
using game::Tire;
using game::TuningScreen;
using game::Vector;
using game::WheelColor;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::f32_be;
using guest::Guest;
using guest::u32_be;

// A car in the garage (two of them, kept one after the other).
struct GarageCar {
  uint8_t car[1880];  // The car itself (Car::Reset).
  u32_be unknown_1880;
  f32_be unknown_1884[2];
  u32_be unknown_1892[2];
  struct Settings {
    u32_be words[15];
  } settings;  // Copied from the defaults at 0x826B2D24.
  u32_be unknown_1960;
  u32_be unknown_1964;
};
GUEST_SIZE(GarageCar, 1968);
GUEST_OFFSET(GarageCar, settings, 1900);

constexpr uint32_t kGarageCars = 0x8280DEC8;
constexpr uint32_t kDefaultSettings = 0x826B2D24;

constexpr uint32_t kCarScene = 0x826B2B24;    // [it] the car scene (writable)
constexpr uint32_t kStand = 0x826C7E88;       // [it]'s +0: the stage's holder (writable)
constexpr uint32_t kReloadCount = 0x826C7E8C; // ReloadModel's frame count (writable)
constexpr uint32_t kCarDealer = 0x826C7E9C;   // [it] the car dealer's list (writable)
constexpr uint32_t kMovieTable = 0x826B59AC;  // The UI movies (writable)

constexpr uint32_t kStagePool = 7;  // The car scene's pool the stage's model is in.
constexpr uint32_t kGarageMovie = 6;

// A car record's part slots (SceneCarRecord::part_order): which part goes
// in each, in order.
struct PartOrder {
  u32_be slots[11];
};
constexpr uint32_t kDefaultPartOrder[11] = {0, 0, 0, 1, 2, 3, 4, 5, 0, 6, 7};

constexpr float kByteToUnit = 1.0f / 255.0f;  // A color byte as 0 to 1.
static_assert(std::bit_cast<uint32_t>(kByteToUnit) == 0x3B808081);
constexpr float kDarkGray = 25.0f / 255.0f;
static_assert(std::bit_cast<uint32_t>(kDarkGray) == 0x3DC8C8C9);
constexpr float kDegreesToRadians = std::bit_cast<float>(0x3C8EFA35u);  // At 0x82002920.

bool On() { return SBX_GET(native_car_makers); }

// A float truncated to an integer as the 360 does it (fctiwz): NaN and
// values below the range give INT_MIN, values above it INT_MAX.
int32_t Truncate(double value) {
  if (std::isnan(value) || value <= double(INT32_MIN)) {
    return INT32_MIN;
  }
  if (value >= double(INT32_MAX)) {
    return INT32_MAX;
  }
  return int32_t(value);
}

// --- The garage cars ---------------------------------------------------------

void ResetGarageCar(Guest& g, GarageCar* car) {
  g.Call(sub_820CD590, car->car);  // Car::Reset
  car->unknown_1880 = 0;
  car->unknown_1884[0] = 0.0f;
  car->unknown_1884[1] = 0.0f;
  car->unknown_1892[0] = 0;
  car->unknown_1892[1] = 0;
  car->unknown_1960 = 1;
  car->settings = *At<GarageCar::Settings>(kDefaultSettings);
}

// --- A scene model record from a car -----------------------------------------

// CarModels::BlockOf (sub_82158AD0): a car id's model block (the car
// models' order isn't the car specs'); 0 for an id out of range.
uint32_t BlockOf(uint32_t car_id) {
  static constexpr uint8_t kBlocks[71] = {
      1,  18, 19, 2,  4,  3,  10, 9,  21, 5,  8,  7,  6,  12, 11, 13, 14, 20,
      15, 22, 17, 16, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 35,
      35, 35, 35, 35, 35, 35, 35, 35, 35, 35, 41, 42, 43, 44, 36, 37, 38, 39,
      11, 12, 15, 13, 10, 5,  4,  6,  9,  1,  2,  3,  0,  14, 8,  7,  40};
  return car_id - 1 < 71 ? kBlocks[car_id - 1] : 0;
}

// CarRecord::ResetBytes (sub_820D06C0): the default part order.
void ResetPartOrder(PartOrder& order) {
  for (int i = 0; i < 11; i++) {
    order.slots[i] = kDefaultPartOrder[i];
  }
}

// CarRecord::SetBytes (sub_821590E0): the record's part order from the
// car's option bytes (signed), with 10 meaning the twelfth part in two of
// them.
void SetPartOrder(SceneCarRecord& record, const CarOptions& options) {
  auto option = [&](int i) { return int32_t(int8_t(options.bytes[i])); };
  ResetPartOrder(*At<PartOrder>(AddressOf(record.part_order)));
  record.part_order[0] = option(10) != 0 ? 2 : 0;
  record.part_order[1] = option(1);
  record.part_order[2] = option(5);
  record.part_order[3] = option(2);
  record.part_order[4] = option(3);
  record.part_order[5] = option(4) == 10 ? 12 : option(4);
  record.part_order[6] = option(6);
  record.part_order[7] = option(7);
  record.part_order[9] = option(8);
  record.part_order[10] = option(9);
  record.part_order[8] = options.bytes[6] == 10 ? 12 : 11;
  record.unknown_3676[1] = option(0);
}

// SceneModelRecord::SetChoice (sub_821591C8): an interior choice {a, b,
// c} as the record keeps it: none for c 0, else c - 1 and 3a - 3 plus a
// step for (a, b) of (1, 0) or (2, 1): 1, and (1, 1): 2.
struct InteriorChoice {
  u32_be chosen;
  u32_be index;
  u32_be variant;
};

void SetChoice(InteriorChoice& out, int32_t a, uint32_t b, uint32_t c) {
  if (c == 0) {
    out.chosen = 0;
    return;
  }
  out.index = c - 1;
  out.chosen = 1;
  int32_t step = 0;
  if (a == 1) {
    step = b == 0 ? 1 : b == 1 ? 2 : 0;
  } else if (a == 2) {
    step = b == 1 ? 1 : 0;
  }
  out.variant = uint32_t(a * 3 + step - 3);
}

// SceneModelRecord::SetWheels (sub_82159260): the wheels' record from the
// car: the wheel numbers (counted from 0, -1 the stock wheel), two
// choices, the brake calipers (the spec sheet's, when it has one), and
// each tire's rim (inches), width (mm) and aspect ratio (percent).
void SetWheels(SceneModelRecord& record, Car& car) {
  game::WheelsRecord& wheels = record.wheels;
  wheels.unknown_16.unknown_0 = car.setup.front_wheel_record.model - 1;
  wheels.unknown_32.unknown_0 = car.setup.rear_wheel_record.model - 1;
  const CarSetupWords& words = *At<CarSetupWords>(AddressOf(&car.setup));
  wheels.unknown_68[0] = uint32_t(words.unknown_48);
  wheels.caliper_texture = uint32_t(words.caliper);
  const game::SpecBlock2& sheet = car.setup.block2;  // Car::GetSpecSheet
  if (sheet.block_132.words_100[1] != 0) {
    wheels.caliper_texture = uint32_t(sheet.block_132.words_100[1]);
  }
  for (int rear = 0; rear < 2; rear++) {
    const Tire& tire = rear ? car.setup.rear_tire : car.setup.front_tire;
    game::Record12& size = rear ? wheels.unknown_56 : wheels.unknown_44;
    size.unknown_0 = Truncate(float(tire.rim_diameter));
    size.unknown_4 = float(tire.width) * 1000.0f;
    size.unknown_8 = float(tire.aspect_ratio) * 100.0f;
  }
}

// A color (a hue, then saturation and value bytes) as the record keeps it.
void SetColor(Vector& to, uint16_t hue, uint8_t saturation, uint8_t value) {
  to.x = float(hue);
  to.y = float(saturation) * kByteToUnit;
  to.z = float(value) * kByteToUnit;
}

// A paint's three colors: its color, its second color, its second shade.
void SetPaintColors(SceneCarRecord& record, const Paint& paint) {
  SetColor(record.unknown_16[0], paint.hue, paint.saturation, paint.unknown_7);
  SetColor(record.unknown_16[1], paint.unknown_8, paint.unknown_10[0], paint.unknown_10[1]);
  SetColor(record.unknown_16[2], paint.shade2_hue, paint.shade2_saturation, paint.shade2_value);
}

// A wheel's three colors (the first of its first color record, both of
// its second).
void SetWheelColors(Vector* to, const SpecWheel& wheel) {
  const WheelColor& a = wheel.colors[0];
  const WheelColor& b = wheel.colors[1];
  SetColor(to[0], a.unknown_0, a.unknown_2[0], a.unknown_2[1]);
  SetColor(to[1], b.unknown_0, b.unknown_2[0], b.unknown_2[1]);
  SetColor(to[2], b.unknown_4, b.unknown_6[0], b.unknown_6[1]);
}

// SceneModelRecord::SetPaint (sub_82158C70): the record's paint from the
// car: `pick` the paint (-1 the car's own pick, -2 the car's custom color
// over its first paint); traffic (kind 3) keeps only the pick. Then the
// wheels' colors from the car's copies of its wheels.
void SetPaint(SceneModelRecord& record, const Car& car, int32_t pick) {
  static constexpr uint32_t kTraffic = 3;
  static constexpr int32_t kCustom = -2;
  if (pick == -1) {
    pick = int16_t(uint16_t(car.unknown_0));
  }
  SceneCarRecord& paint = record.car;
  const game::SpecPaints& paints = car.setup.paints;
  if (record.kind == kTraffic) {
    paint.unknown_160[0] = uint32_t(pick);
  } else if (pick == kCustom) {
    SetPaintColors(paint, paints.paints[0]);
    // The custom color in both colors; the second shade by its type
    // (solid: dark gray; two-shade: its hue at 60% saturation, value 204).
    SetColor(paint.unknown_16[0], car.unknown_8, car.unknown_10, car.unknown_11);
    SetColor(paint.unknown_16[1], car.unknown_8, car.unknown_10, car.unknown_11);
    if (car.unknown_6 == Paint::kSolid) {
      paint.unknown_16[2].x = 0.0f;
      paint.unknown_16[2].y = 0.0f;
      paint.unknown_16[2].z = kDarkGray;
    } else if (car.unknown_6 == Paint::kTwoShade) {
      paint.unknown_16[2].x = float(uint16_t(car.unknown_8));
      paint.unknown_16[2].z = 0.8f;
      paint.unknown_16[2].y = float(car.unknown_10) * 0.6f * kByteToUnit;
    }
  } else {
    SetPaintColors(paint, *Element<Paint>(AddressOf(paints.paints), uint32_t(pick)));
  }

  game::WheelsRecord& wheels = record.wheels;
  const SpecWheel& front = car.setup.front_wheel_record;
  const SpecWheel& rear = car.setup.rear_wheel_record;
  wheels.unknown_4[0] = 1;
  SetWheelColors(&paint.unknown_16[3], front);
  SetWheelColors(&paint.unknown_16[6], rear);
  wheels.unknown_16.unknown_8.raw = front.colors[0].unknown_8.raw;
  wheels.unknown_32.unknown_8.raw = rear.colors[0].unknown_8.raw;
  wheels.unknown_16.unknown_4.raw = front.colors[1].unknown_8.raw;
  wheels.unknown_32.unknown_4.raw = rear.colors[1].unknown_8.raw;
}

// --- SceneModelRecord::FromCar -------------------------------------------------

constexpr uint32_t kBodyKits = 0x82009318;  // BodyKitEntry table (read-only data)

// The record's kinds: the players' cars (flags 0x81, Race::AddRacer), the
// other cars (flag 0x2) and traffic, which keeps none of the car's parts.
constexpr uint32_t kPlayerCar = 2;
constexpr uint32_t kOtherCar = 1;
constexpr uint32_t kTrafficCar = 3;

// FromCar's frame, as memcpy sees it: +80 a scratch area (the parts'
// pointers copied, then the body kit's entry, then the tire grades'
// table), +128 the parts' pointers (Params::GetCarParts), then the body
// kit's part over them (AeroParts::Get82).
struct FromCarFrame {
  uint8_t linkage[80];
  union {
    u32_be parts_copy[9];
    game::BodyKitEntry body_kit_entry;
    u32_be grade_classes[5];
    u32_be scratch[12];
  };
  union {
    u32_be parts[9];
    game::AeroPart36 body_kit;
  };
  uint8_t unknown_164[92];
};
GUEST_SIZE(FromCarFrame, 256);
GUEST_OFFSET(FromCarFrame, parts, 128);
GUEST_OFFSET(FromCarFrame, body_kit, 128);

// A decal of the car as a record layer: its image and word, its position
// (two floats and a third) and its scale. The floats are copied as bits.
void SetLayer(game::RecordLayer& layer, const game::Decal& decal) {
  layer.unknown_0 = decal.image;
  layer.unknown_4 = decal.unknown_28;
  layer.unknown_16.x = decal.unknown_8[0];
  layer.unknown_16.y = decal.unknown_8[1];
  layer.unknown_32 = decal.unknown_8[2];
  layer.unknown_48.x = decal.scale[0];
  layer.unknown_48.y = decal.scale[1];
}

// The car's decals into the record's layers: its six first ones (2, 1, 0
// into layers 0 to 2 and 5, 4, 3 into 3 to 5), then its forty others in
// eight runs of five, in the original's order.
void SetDecals(SceneCarRecord& record, const Car& car) {
  for (int i = 0; i < 3; i++) {
    SetLayer(record.layers[i], car.decals_96[2 - i]);
    SetLayer(record.layers[i + 3], car.decals_96[5 - i]);
  }
  struct Run {
    int first_layer;
    int first_decal;
  };
  static constexpr Run kRuns[8] = {{6, 0},   {18, 5},  {12, 10}, {24, 15},
                                   {30, 20}, {36, 25}, {48, 30}, {42, 35}};
  for (const Run& run : kRuns) {
    for (int i = 0; i < 5; i++) {
      SetLayer(record.layers[run.first_layer + i], car.decals_288[run.first_decal + i]);
    }
  }
}

// An interior choice of the car into the record (SetChoice).
void SetChoice(u32_be& out, const game::CarInteriorChoice& choice) {
  SetChoice(*At<InteriorChoice>(AddressOf(&out)), choice.a, choice.b, choice.c);
}

// The player's cockpit and interior: the cockpit record reset and set from
// the aero levels, the light kits, the interior's five choices (two of
// them in the second set's slots once there's a second light kit, or the
// spec sheet asks for it) and the accessory (3: none, the record's kept).
void SetInterior(Guest& g, SceneModelRecord& record, const Car& car) {
  const CarSetupWords& words = *At<CarSetupWords>(AddressOf(&car.setup));
  const auto& level = car.setup.aero_levels;
  game::CockpitRecord& cockpit = record.cockpit;
  game::InteriorRecord& interior = record.interior;
  g.Call(sub_820D3100, &cockpit);  // CockpitRecord::Reset
  cockpit.unknown_0[0] = uint32_t(int32_t(level[4]));
  cockpit.unknown_0[1] = uint32_t(int32_t(level[4]));
  cockpit.unknown_0[4] = uint32_t(int32_t(level[9]));
  cockpit.unknown_0[3] = uint32_t(int32_t(level[7]));
  cockpit.unknown_0[2] = uint32_t(int32_t(level[6]));
  interior.unknown_100 = int32_t(words.unknown_152[0]);
  interior.unknown_0[24] = words.unknown_152[1];
  interior.unknown_0[23] = words.unknown_152[2];
  record.car.unknown_3676[0] = words.unknown_152[3];

  bool second_set = false;
  const int32_t kits = words.light_kit;
  if (kits <= 0 || kits > 6) {
    interior.unknown_104[1] = 0;
    interior.unknown_104[0] = 0;
  } else if (kits <= 2) {
    interior.unknown_104[1] = 1;
    interior.unknown_104[0] = 0;
  } else {
    second_set = true;
    interior.unknown_104[1] = 1;
    interior.unknown_104[0] = kits == 6 ? 2 : 1;
  }
  if (car.setup.block2.block_132.words_100[0] == 1) {  // Car::GetSpecSheet's +232
    interior.unknown_104[0] = 1;
    second_set = true;
  }
  if (second_set) {
    SetChoice(interior.unknown_0[8], words.choices[3]);
    SetChoice(interior.unknown_0[11], words.choices[4]);
  } else {
    SetChoice(interior.unknown_0[2], words.choices[3]);
    SetChoice(interior.unknown_0[5], words.choices[4]);
  }
  SetChoice(interior.unknown_0[20], words.choices[0]);
  SetChoice(interior.unknown_0[17], words.choices[1]);
  SetChoice(interior.unknown_0[14], words.choices[2]);
  if (words.accessory != 3) {
    interior.accessory = words.accessory;
    interior.unknown_116 = words.unknown_172;
  }
}

// The car's parts into the record: the engine stage (and its class: 2 from
// stage 7, 1 from 3), two power parts, the lights and mirror, and the aero
// parts: the car's own levels, or with a body kit (aero level 10) the
// kit's entry for the model block over them (-1 keeps the car's), its
// flags (AeroParts::Get82's +24) choosing what else the car keeps.
void SetParts(Guest& g, SceneModelRecord& record, const Car& car, FromCarFrame& frame) {
  const CarSetupWords& words = *At<CarSetupWords>(AddressOf(&car.setup));
  const auto& level = car.setup.aero_levels;
  auto& part = record.parts.unknown_0;
  part[11] = uint32_t(int32_t(car.setup.engine_stage));
  const int32_t stage = car.setup.engine_stage;
  record.interior.unknown_0[1] = stage >= 7 ? 2 : stage >= 3 ? 1 : 0;
  part[1] = words.unknown_16;
  part[12] = words.unknown_20;
  part[7] = uint32_t(int32_t(level[6]));
  part[8] = uint32_t(int32_t(level[7]));
  part[9] = uint32_t(int32_t(level[8]));
  const int32_t kit = uint32_t(int32_t(level[10]));
  if (kit == 0) {
    part[0] = uint32_t(int32_t(level[0]));
    part[2] = uint32_t(int32_t(level[2]));
    part[3] = uint32_t(int32_t(level[1]));
    part[4] = level[3] == -1 ? 13u : uint32_t(int32_t(level[3]));
    part[5] = uint32_t(int32_t(level[4]));
    record.car.unknown_3676[3] = level[5] == 1 ? 1 : 0;
    part[6] = 0;
    part[10] = uint32_t(int32_t(level[9]));
    return;
  }
  static constexpr uint32_t kKeepsBonnet = 0x1;   // The car's bonnet level and kind.
  static constexpr uint32_t kKeepsSpoiler = 0x2;  // The car's rear spoiler level.
  static constexpr uint32_t kShowsKit = 0x8;      // The kit's level in part 6.
  uint32_t parts[game::CarParams::kPartTables];
  game::GetCarParts(car.car_id, parts);
  for (int i = 0; i < game::CarParams::kPartTables; i++) {
    frame.parts[i] = parts[i];
  }
  g.Call(sub_8258CB90, frame.parts_copy, frame.parts, 36u);  // memcpy
  for (int i = 0; i < game::CarParams::kPartTables; i++) {
    parts[i] = frame.parts_copy[i];
  }
  game::GetBodyKitPart(g, frame.body_kit, parts, kit);
  const uint32_t flags = frame.body_kit.part.unknown_24;
  const uint32_t index = uint32_t(int32_t(record.car.unknown_0)) * 2 + uint32_t(kit - 1);
  g.Call(sub_8258CB90, &frame.body_kit_entry,
         kBodyKits + uint32_t(sizeof(game::BodyKitEntry)) * index,
         uint32_t(sizeof(game::BodyKitEntry)));  // memcpy
  const game::BodyKitEntry& entry = frame.body_kit_entry;
  if (entry.part_0 != -1) {
    part[0] = uint32_t(int32_t(entry.part_0));
  }
  if (entry.part_2 != -1) {
    part[2] = uint32_t(int32_t(entry.part_2));
  }
  if (entry.part_3 != -1) {
    part[3] = uint32_t(int32_t(entry.part_3));
  }
  if (flags & kKeepsSpoiler) {
    part[4] = level[3] == -1 ? 13u : uint32_t(int32_t(level[3]));
  } else if (entry.part_4 != -1) {
    part[4] = uint32_t(int32_t(entry.part_4));
  }
  if (flags & kKeepsBonnet) {
    part[5] = uint32_t(int32_t(level[4]));
    record.car.unknown_3676[3] = level[5] == 1 ? 1 : 0;
  } else if (entry.part_5 != -1) {
    part[5] = uint32_t(int32_t(entry.part_5));
  }
  part[6] = (flags & kShowsKit) ? uint32_t(kit) : 0u;
  if (entry.part_10 != -1) {
    part[10] = uint32_t(int32_t(entry.part_10));
  }
}

// SceneModelRecord::FromCar (sub_82159880): the record reset and made from
// the car: its kind and model block, wheels and paint (`pick`), for the
// players' cars the cockpit and interior, for cars (not traffic) the parts
// and decals, the class flag (the car's +4 of 1, 2 or 4), for cars the pool
// 4 record from the car's three slots and each tire's grade class, last
// the part order. Returns SetBytes' (the record's part order).
//
// The tire grades' class is looked up in a five-word table in the frame,
// unchecked, as the original does; a grade of 5 reads the frame's next
// word (the body kit entry's last, or whatever the stack held).
void FromCar(Guest& g, SceneModelRecord& record, Car& car, uint32_t pick) {
  guest::StackFrame stack(g.ctx, sizeof(FromCarFrame));
  FromCarFrame& frame = *At<FromCarFrame>(stack.Local(0));
  g.Call(sub_820D94D8, &record);  // SceneModelRecord::Reset
  const uint32_t flags = car.unknown_3888;
  const bool is_car = (flags & 0x81) != 0 || (flags & 0x2) != 0;
  record.kind = (flags & 0x81) != 0 ? kPlayerCar : (flags & 0x2) != 0 ? kOtherCar : kTrafficCar;
  record.car.unknown_0 = int32_t(BlockOf(car.car_id));
  SetWheels(record, car);
  SetPaint(record, car, int32_t(pick));
  const CarSetupWords& words = *At<CarSetupWords>(AddressOf(&car.setup));
  if (record.kind == kPlayerCar) {
    SetInterior(g, record, car);
  }
  if (is_car) {
    SetParts(g, record, car, frame);
  }
  record.car.unknown_3676[2] = words.unknown_144;
  if (is_car) {
    SetDecals(record.car, car);
  }
  const int32_t car_class = car.unknown_4;
  record.parts.unknown_56 = car_class == 1 || car_class == 2 || car_class == 4 ? 1 : 0;
  if (is_car) {
    static constexpr uint32_t kGradeClasses[5] = {0, 1, 1, 2, 2};
    for (int i = 0; i < 5; i++) {
      frame.grade_classes[i] = kGradeClasses[i];
    }
    game::Pool4Record& pool4 = record.pool4;
    for (int i = 0; i < 3; i++) {
      pool4.colors[i].unknown_0 = car.slots[i].unknown_0;
      for (int k = 0; k < 3; k++) {
        pool4.colors[i].rgb[k] = car.slots[i].values[k];
      }
    }
    pool4.texture = car.slots[0].unknown_4;
    const uint32_t table = AddressOf(frame.grade_classes);
    record.wheels.unknown_4[2] = *Element<u32_be>(table, car.setup.front_tire_grade);
    record.wheels.unknown_28 = *Element<u32_be>(table, car.setup.rear_tire_grade);
  }
  SetPartOrder(record.car, car.options);
}

// --- The garage stage ----------------------------------------------------------

uint32_t CarScene() { return *At<u32_be>(kCarScene); }

GarageStageHolder* StandHolder() { return At<GarageStageHolder>(*At<u32_be>(*At<u32_be>(kStand))); }

// The stage's scene model (ScenePools::GetB, sub_820C59D8).
SceneModel* StageModel(Guest& g, GarageStageHolder* holder) {
  const uint32_t scene = CarScene();
  return At<SceneModel>(g.Call(sub_820C59D8, scene, kStagePool, holder->stage->scene_model));
}

// GarageStage::CopyRecord (sub_8217DB48): `record` copied to the stage's
// scene model's record (SceneModelRecord::Copy). Returns Copy's r3.
uint32_t CopyRecord(Guest& g, GarageStageHolder* holder, uint32_t record) {
  SceneModel* model = StageModel(g, holder);
  return g.Call(sub_820DAF58, &model->record, record);  // SceneModelRecord::Copy
}

// GarageStage::ReapplyRecord (sub_82176E48) and ReapplyRecordNoDecals
// (sub_82176EB0): the stage's scene model's paint, wheel paint, record and
// light kit applied again, then (with `decals`) its decals reloaded.
// Returns the last call's r3.
uint32_t ReapplyRecord(Guest& g, GarageStageHolder* holder, bool decals) {
  SceneModel* model = StageModel(g, holder);
  g.Call(sub_820D9830, model);  // SceneModel::ApplyPaint
  g.Call(sub_820D9918, model);  // SceneModel::ApplyWheelPaint
  g.Call(sub_820DA420, model);  // SceneModel::ApplyRecord
  const uint32_t r3 = g.Call(sub_820D99D8, model);  // SceneModel::ApplyLightKit
  if (!decals) {
    return r3;
  }
  return g.Call(sub_820D9A10, model);  // SceneModel::ReloadDecals
}

// GarageStage::FitCar (sub_82176C90): the car's data built in the frame
// (CarData::CarData, Vehicle::SetupFromCar), then the stage's car sizes
// from it. f1 is left as the second sin's (as the original leaves it).
void FitCar(Guest& g, GarageStageHolder* holder, uint32_t car) {
  guest::StackFrame frame(g.ctx, 1968);
  CarData* data = At<CarData>(frame.Local(80));
  g.Call(sub_820AEE68, data);        // CarData::CarData
  g.Call(sub_820C2608, data, car);   // Vehicle::SetupFromCar
  const game::DrivetrainAxle* axles = data->axles;
  GarageStage* stage = holder->stage;
  stage->heights[0] = data->unknown_1736[0];
  stage->heights[1] = data->unknown_1756;
  stage->axle_heights[0] = axles[0].values[13];
  stage->axle_heights[1] = axles[1].values[13];
  for (int i = 0; i < 2; i++) {
    stage->height_differences[i] = float(stage->axle_heights[i]) - float(stage->heights[i]);
  }
  stage->dimensions[0] = data->unknown_0.y;
  stage->dimensions[1] = data->unknown_0.z;
  stage->dimensions[2] = data->unknown_0.w;
  stage->axle_angles[0] = axles[0].values[52];
  stage->axle_angles[1] = axles[1].values[52];
  stage->axle_values_212[0] = axles[0].values[53];
  stage->axle_values_212[1] = axles[1].values[53];
  for (int i = 0; i < 2; i++) {
    g.ctx.f1.f64 = double(float(axles[i].values[52]) * kDegreesToRadians);
    g.Call(sub_8258E140);  // sin
    holder->stage->angle_offsets[i] = float(g.ctx.f1.f64) * float(axles[i].values[13]);
  }
  for (int i = 0; i < 2; i++) {
    Vector& position = holder->stage->axle_positions[i];
    position.x = axles[i].values[0];
    position.y = axles[i].values[1];
    position.z = axles[i].values[2];
    position.w = 1.0f;
  }
}

// GarageStage::SetCar (sub_8217DC40) and SetCarClass (sub_8217DCB0): the
// stage's scene model's record made from the car (SceneModelRecord::
// FromCar, `pick` the paint), copied onto itself, then either the stage
// fitted to the car and the record re-applied, or (SetCarClass) the
// record's class flag set from `pick` (1, 2 or 4) and the record
// re-applied without its decals. Returns the re-apply's r3.
uint32_t SetCar(Guest& g, GarageStageHolder* holder, uint32_t car, uint32_t pick) {
  SceneModel* model = StageModel(g, holder);
  FromCar(g, model->record, *At<Car>(car), pick);
  CopyRecord(g, holder, AddressOf(&model->record));
  FitCar(g, holder, car);
  return ReapplyRecord(g, holder, true);
}

uint32_t SetCarClass(Guest& g, GarageStageHolder* holder, uint32_t car, uint32_t pick) {
  SceneModel* model = StageModel(g, holder);
  FromCar(g, model->record, *At<Car>(car), pick);
  const int32_t kind = int32_t(pick);
  model->record.parts.unknown_56 = kind == 1 || kind == 2 || kind == 4 ? 1 : 0;
  CopyRecord(g, holder, AddressOf(&model->record));
  return ReapplyRecord(g, holder, false);
}

// GarageStage::ShowCarById (sub_82180B40) and ShowCarByIdWithClass
// (sub_82180BE0): the car dealer's shown car built in the frame (Car::Init,
// Car::SetModel by its id, flag 0x1, Car::Refresh) and set on the stage
// (SetCar or SetCarClass, `pick` passed on).
void ShowCarById(Guest& g, uint32_t pick, bool with_class) {
  guest::StackFrame frame(g.ctx, 4016);
  StageModel(g, StandHolder());  // Its answer isn't used.
  const CarDealerList* list = At<CarDealerList>(*At<u32_be>(kCarDealer));
  const u32_be* entries = At<u32_be>(list->entries);
  const uint32_t car_id = At<CarDealerEntry>(entries[list->first + list->offset])->car_id;
  Car* car = At<Car>(frame.Local(80));
  g.Call(sub_820CD378, car);          // Car::Init
  g.Call(sub_820CD648, car, car_id);  // Car::SetModel
  car->unknown_3888 |= 1;
  game::RefreshCar(g, *car);
  GarageStageHolder* holder = StandHolder();
  if (with_class) {
    SetCarClass(g, holder, AddressOf(car), pick);
  } else {
    SetCar(g, holder, AddressOf(car), pick);
  }
}

// GarageStage::ReloadModel (sub_8217D768): counts the frames since the
// last reload (`block` the model block to load next, -1 to keep it); the
// car scene updates each frame. From the fourth frame on, once the stage's
// scene model has finished loading, the old model is released and a new
// one set up for the block: 1 then, else 0.
uint32_t ReloadModel(Guest& g, GarageStageHolder* holder, int32_t block) {
  guest::StackFrame frame(g.ctx, 4400);
  const uint32_t scene = CarScene();
  SceneModel* model = At<SceneModel>(g.Call(sub_820C59D8, scene, kStagePool, holder->stage->scene_model));
  u32_be* count = At<u32_be>(kReloadCount);
  model->unknown_4296 = 0;
  *count += 1;
  if (block != -1) {
    holder->stage->next_model_block = block;
  }
  const auto loaded = [&] { return int32_t(g.Call(sub_820D9FE0, model)) != 0; };  // SceneModel::PollLoad
  if (!(*count >= 4 && loaded())) {
    g.Call(sub_820C93A8);  // CarScene::Update
  }
  if (*count < 4) {
    return 0;
  }
  *count = 4;
  if (!loaded()) {
    return 0;
  }
  if (holder->stage->has_model != 0) {
    g.Call(sub_820C5A40, scene, kStagePool, holder->stage->scene_model);  // ScenePools::ReleaseB
    holder->stage->has_model = 0;
  }
  g.Call(sub_820C93A8);  // CarScene::Update
  SceneModelRecord* record = At<SceneModelRecord>(frame.Local(80));
  g.Call(sub_820DAF08, record);  // SceneModelRecord::SceneModelRecord
  g.Call(sub_820D94D8, record);  // SceneModelRecord::Reset
  record->car.unknown_0 = holder->stage->next_model_block;
  holder->stage->scene_model = g.Call(sub_820C5868, scene, kStagePool, 0u);  // ScenePools::ClaimB
  g.Call(sub_820C58E0, scene, kStagePool, holder->stage->scene_model, record);  // ScenePools::SetUpB
  model->unknown_4296 = 1;
  *count = 0;
  holder->stage->unknown_6056 = 1;
  holder->stage->has_model = 1;
  holder->stage->next_model_block = -1;
  return 1;
}

// --- The tuning screen's car ---------------------------------------------------

// CarData::FromGarageCar (sub_82177AB0): a car object built in the frame
// from a garage car (Car::Init, its first 1,880 bytes copied over,
// Car::Refresh), added as a racer (Race::AddRacer, which sets its flags),
// then `data` set up from it (Car::SetupVehicle, then Vehicle::SetupFromCar
// again). r3 and f1 as the last call leaves them.
void FromGarageCar(Guest& g, uint32_t garage_car, uint32_t data) {
  guest::StackFrame frame(g.ctx, 4016);
  Car* car = At<Car>(frame.Local(80));
  g.Call(sub_820CD378, car);  // Car::Init
  g.Call(sub_8258CB90, car, garage_car, SavedCar::kCarBytes);  // memcpy
  game::RefreshCar(g, *car);
  // Race::AddRacer reads r3, r5, r6 and f1.
  g.ctx.r5.u64 = 0;
  g.ctx.r6.u64 = 0;
  g.ctx.f1.f64 = 0.0;
  g.Call(sub_82101910, car);       // Race::AddRacer
  g.Call(sub_820CD368, car, data); // Car::SetupVehicle
  g.Call(sub_820C2608, data, car); // Vehicle::SetupFromCar
}

// TuningScreen::SetUpCarData (sub_821DD738): the screen's car data from
// the selected garage car (the save's garage list walked from its head to
// the selected place).
void TuningScreenSetUpCarData(Guest& g, TuningScreen& screen) {
  SaveGarage& garage = screen.save->garage;
  using Node = game::SaveListIndex::Node;
  const uint32_t nodes = AddressOf(garage.list.nodes);
  const Node* node = Element<Node>(nodes, uint32_t(int32_t(garage.list.head)));
  for (uint32_t n = garage.selected; n != 0; n--) {
    node = Element<Node>(nodes, uint32_t(int32_t(node->next)));
  }
  const SavedCar* car = Element<SavedCar>(AddressOf(garage.cars), uint32_t(int32_t(node->slot)));
  FromGarageCar(g, AddressOf(car), AddressOf(&screen.car_data));
}

// --- The stand's busy marker -------------------------------------------------

// The garage's UI movie (UiMovies::Find, its key passed by address, in
// the caller's frame).
uint32_t GarageMovie(Guest& g, const guest::StackFrame& frame) {
  u32_be* key = At<u32_be>(frame.Local(80));
  *key = kGarageMovie;
  return *At<u32_be>(g.Call(sub_8211B5C0, kMovieTable, key));
}

// The marker's named animations.
constexpr uint32_t kMarkerLoop = 85;
constexpr uint32_t kMarkerShow = 86;
constexpr uint32_t kMarkerHide = 87;

void Finish(Guest& g, uint32_t movie, uint32_t animation) {
  g.Call(sub_8211E0E0, movie, animation);  // UiMovie::FinishNamed
}

void Play(Guest& g, uint32_t movie, uint32_t animation) {
  g.Call(sub_8211DC88, movie, animation, 0u, 0u, 0u, 0u);  // UiMovie::PlayNamed
}

// GarageScreen::ShowBusyMarker (sub_8219D230): shown, then looped.
void ShowBusyMarker(Guest& g) {
  guest::StackFrame frame(g.ctx, 112);
  const uint32_t movie = GarageMovie(g, frame);
  Finish(g, movie, kMarkerShow);
  Finish(g, movie, kMarkerHide);
  Play(g, movie, kMarkerShow);
  Play(g, movie, kMarkerLoop);
  g.Call(sub_8211DFA0, movie, kMarkerLoop);  // UiMovie::LoopNamed
}

// GarageScreen::HideBusyMarker (sub_8219D2D0) and sub_8219D350 (the same,
// the hide finished at once too).
void HideBusyMarker(Guest& g, bool finish_hide) {
  guest::StackFrame frame(g.ctx, 112);
  const uint32_t movie = GarageMovie(g, frame);
  Finish(g, movie, kMarkerShow);
  Finish(g, movie, kMarkerHide);
  Finish(g, movie, kMarkerLoop);
  Play(g, movie, kMarkerHide);
  if (finish_hide) {
    Finish(g, movie, kMarkerHide);
  }
}

// --- The rival list screen ---------------------------------------------------

constexpr uint32_t kRivalListMachine = 0x826C7EE4;  // [it] (writable)
constexpr uint32_t kRivalListScreen = 0x826C7EF0;   // [it] (writable)

// The screen's 21 transitions, by number (each gets the machine's step
// word; nonzero once it's done).
Guest::Function* const kTransitions[21] = {
    sub_8219EDF8, sub_8219AA48, sub_8219ABF0, sub_8219E028, sub_8219E180, sub_8219AD70,
    sub_8219F658, sub_8219AE18, sub_8219D8C0, sub_8219F370, sub_8219EF58, sub_8219E330,
    sub_8219AF28, sub_8219B038, sub_8219DA40, sub_8219F058, sub_8219F778, sub_8219E3E8,
    sub_8219B0F8, sub_8219DBB0, sub_8219E498};

// A finished transition: the queued one runs next, or with none queued the
// screen settles on the transition's level.
void NextTransition(game::RivalListMachine& machine) {
  if (machine.queued == game::RivalListMachine::kNone) {
    machine.level = int32_t(machine.next_level);
    machine.running = game::RivalListMachine::kNone;
  } else {
    machine.running = int32_t(machine.queued);
    machine.queued = game::RivalListMachine::kNone;
    machine.unknown_20 = 0;
  }
  machine.step = 0;
}

// The rival list screen's update (sub_8219F8A0, through its vtable): the
// running transition (none: the movie runs unless the screen is leaving),
// then the stand: the busy marker shown while the garage stage is busy,
// the stage's model reloaded until it's ready and the selected entry's
// car built in the frame (Car::Init, Car::FromBuild, flag 0x1,
// Car::Refresh) and set on it (GarageStage::SetCar, then Command 54), and
// once shown the stage stepped. With no car to show, a shown busy marker
// is put away.
//
// The original builds its 21 transitions' addresses in its frame (+96)
// and calls the running one from there; nothing else reads that table, so
// it's a plain table here. A transition number past 20 would call
// whatever followed the table in the frame: nothing runs then.
void RivalListUpdate(Guest& g) {
  guest::StackFrame frame(g.ctx, 4224);
  u32_be* key = At<u32_be>(frame.Local(80));
  *key = kGarageMovie;
  const uint32_t movie = *At<u32_be>(g.Call(sub_8211B5C0, kMovieTable, key));  // UiMovies::Find
  game::RivalListMachine& machine = *At<game::RivalListMachine>(*At<u32_be>(kRivalListMachine));
  const int32_t running = machine.running;
  if (running == game::RivalListMachine::kNone) {
    if (machine.level != 4) {
      g.Call(sub_8211E378, movie, 0u);  // UiMovie::Run
    }
  } else if (running < 0) {
    NextTransition(machine);
  } else if (running < 21) {
    const uint32_t done = g.Call(kTransitions[running], &machine.step);
    if (int32_t(done) != 0) {
      NextTransition(*At<game::RivalListMachine>(*At<u32_be>(kRivalListMachine)));
    }
  }

  auto screen = [] { return At<game::RivalListScreen>(*At<u32_be>(kRivalListScreen)); };
  if (screen()->shows_car == 0) {
    if (screen()->busy_shown != 0) {
      HideBusyMarker(g, true);  // sub_8219D350
      screen()->busy_shown = 0;
    }
    return;
  }
  const bool busy = int32_t(g.Call(sub_82177C30, StandHolder())) != 0;  // GarageStage::IsBusy
  if (busy && screen()->busy_shown == 0) {
    ShowBusyMarker(g);
    screen()->busy_shown = 1;
  } else if (!busy && screen()->busy_shown != 0) {
    HideBusyMarker(g, false);
    screen()->busy_shown = 0;
  }
  if (screen()->car_shown != 0) {
    g.Call(sub_8217DD48, StandHolder(), 0u, 1u);  // GarageStage::Step
    return;
  }
  screen()->car_shown = ReloadModel(g, StandHolder(), -1);
  if (screen()->car_shown == 0) {
    return;
  }
  const game::RivalListEntry* entry =
      Element<game::RivalListEntry>(AddressOf(screen()->entries), screen()->selected);
  Car* car = At<Car>(frame.Local(192));
  g.Call(sub_820CD378, car);  // Car::Init
  g.Call(sub_820CDA98, car, entry->build);  // Car::FromBuild
  car->unknown_3888 |= 1;
  game::RefreshCar(g, *car);
  SetCar(g, StandHolder(), AddressOf(car), uint32_t(-1));
  g.Call(sub_8217DFF0, StandHolder(), 54u);  // GarageStage::Command
}

}  // namespace

#define SBX_CAR_MAKERS_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();                  \
  if (!On()) {                          \
    __imp__##name(ctx, base);           \
    return;                             \
  }                                     \
  Guest g(ctx, base);

// GarageCar::Reset: r3 the garage car.
extern "C" REX_FUNC(sub_82110370) {
  SBX_CAR_MAKERS_FALLBACK(sub_82110370)
  ResetGarageCar(g, At<GarageCar>(ctx.r3.u32));
}

// GarageCars::StaticInit: both garage cars reset.
extern "C" REX_FUNC(sub_825BF2C8) {
  SBX_CAR_MAKERS_FALLBACK(sub_825BF2C8)
  for (int i = 0; i < 2; i++) {
    GarageCar* car = Element<GarageCar>(kGarageCars, i);
    ResetGarageCar(g, car);
    car->unknown_1964 = 0;
  }
}

// --- A scene model record from a car

// CarRecord::ResetBytes: r3 the record's part order. Returns it.
extern "C" REX_FUNC(sub_820D06C0) {
  SBX_CAR_MAKERS_FALLBACK(sub_820D06C0)
  ResetPartOrder(*At<PartOrder>(ctx.r3.u32));
}

// CarRecord::SetBytes: r3 the car record, r4 the car's options.
extern "C" REX_FUNC(sub_821590E0) {
  SBX_CAR_MAKERS_FALLBACK(sub_821590E0)
  SetPartOrder(*At<SceneCarRecord>(ctx.r3.u32), *At<CarOptions>(ctx.r4.u32));
}

// CarModels::BlockOf: r3 a car id.
extern "C" REX_FUNC(sub_82158AD0) {
  SBX_CAR_MAKERS_FALLBACK(sub_82158AD0)
  ctx.r3.u64 = BlockOf(ctx.r3.u32);
}

// CarModels::BlockOf's case for car id 1 (the jump table's first entry),
// a function of its own.
extern "C" REX_FUNC(sub_82158B04) {
  SBX_CAR_MAKERS_FALLBACK(sub_82158B04)
  ctx.r3.u64 = BlockOf(1);
}

// SceneModelRecord::SetChoice: r3 the choice out (left in r3), r4:r5 the
// 12-byte value {a, b, c} in two doublewords (a and b in r4, c the high
// word of r5).
extern "C" REX_FUNC(sub_821591C8) {
  SBX_CAR_MAKERS_FALLBACK(sub_821591C8)
  SetChoice(*At<InteriorChoice>(ctx.r3.u32), int32_t(ctx.r4.u64 >> 32), ctx.r4.u32,
            uint32_t(ctx.r5.u64 >> 32));
}

// SceneModelRecord::SetWheels: r3 the record, r4 the car.
extern "C" REX_FUNC(sub_82159260) {
  SBX_CAR_MAKERS_FALLBACK(sub_82159260)
  ctx.fpscr.disableFlushMode();
  SetWheels(*At<SceneModelRecord>(ctx.r3.u32), *At<Car>(ctx.r4.u32));
}

// SceneModelRecord::FromCar: r3 the record, r4 the car, r5 the paint pick.
extern "C" REX_FUNC(sub_82159880) {
  SBX_CAR_MAKERS_FALLBACK(sub_82159880)
  ctx.fpscr.disableFlushMode();
  FromCar(g, *At<SceneModelRecord>(ctx.r3.u32), *At<Car>(ctx.r4.u32), ctx.r5.u32);
}

// SceneModelRecord::SetPaint: r3 the record (left in r3), r4 the car, r5
// the paint pick.
extern "C" REX_FUNC(sub_82158C70) {
  SBX_CAR_MAKERS_FALLBACK(sub_82158C70)
  ctx.fpscr.disableFlushMode();
  SetPaint(*At<SceneModelRecord>(ctx.r3.u32), *At<Car>(ctx.r4.u32), ctx.r5.s32);
}

// --- The garage stage: r3 the stage's holder

// GarageStage::CopyRecord: r4 the record.
extern "C" REX_FUNC(sub_8217DB48) {
  SBX_CAR_MAKERS_FALLBACK(sub_8217DB48)
  ctx.r3.u64 = CopyRecord(g, At<GarageStageHolder>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_82176E48) {
  SBX_CAR_MAKERS_FALLBACK(sub_82176E48)
  ctx.r3.u64 = ReapplyRecord(g, At<GarageStageHolder>(ctx.r3.u32), true);
}

extern "C" REX_FUNC(sub_82176EB0) {
  SBX_CAR_MAKERS_FALLBACK(sub_82176EB0)
  ctx.r3.u64 = ReapplyRecord(g, At<GarageStageHolder>(ctx.r3.u32), false);
}

// GarageStage::FitCar: r4 the car. r3 and f1 as its calls leave them.
extern "C" REX_FUNC(sub_82176C90) {
  SBX_CAR_MAKERS_FALLBACK(sub_82176C90)
  ctx.fpscr.disableFlushMode();
  FitCar(g, At<GarageStageHolder>(ctx.r3.u32), ctx.r4.u32);
}

// GarageStage::SetCar and SetCarClass: r4 the car, r5 the paint pick.
extern "C" REX_FUNC(sub_8217DC40) {
  SBX_CAR_MAKERS_FALLBACK(sub_8217DC40)
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = SetCar(g, At<GarageStageHolder>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

extern "C" REX_FUNC(sub_8217DCB0) {
  SBX_CAR_MAKERS_FALLBACK(sub_8217DCB0)
  ctx.r3.u64 = SetCarClass(g, At<GarageStageHolder>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// GarageStage::ReloadModel: r4 the model block, or -1.
extern "C" REX_FUNC(sub_8217D768) {
  SBX_CAR_MAKERS_FALLBACK(sub_8217D768)
  ctx.r3.u64 = ReloadModel(g, At<GarageStageHolder>(ctx.r3.u32), ctx.r4.s32);
}

// GarageStage::ShowCarById and ShowCarByIdWithClass: r3 the paint pick.
// r3 (and f1) as their last call leaves them.
extern "C" REX_FUNC(sub_82180B40) {
  SBX_CAR_MAKERS_FALLBACK(sub_82180B40)
  ctx.fpscr.disableFlushMode();
  ShowCarById(g, ctx.r3.u32, false);
}

extern "C" REX_FUNC(sub_82180BE0) {
  SBX_CAR_MAKERS_FALLBACK(sub_82180BE0)
  ShowCarById(g, ctx.r3.u32, true);
}

// --- The tuning screen's car

// CarData::FromGarageCar: r3 not read, r4 the garage car, r5 the car data.
extern "C" REX_FUNC(sub_82177AB0) {
  SBX_CAR_MAKERS_FALLBACK(sub_82177AB0)
  FromGarageCar(g, ctx.r4.u32, ctx.r5.u32);
}

// TuningScreen::SetUpCarData: r3 the tuning screen.
extern "C" REX_FUNC(sub_821DD738) {
  SBX_CAR_MAKERS_FALLBACK(sub_821DD738)
  TuningScreenSetUpCarData(g, *At<TuningScreen>(ctx.r3.u32));
}

// --- The stand's busy marker (no arguments; r3 as the last call leaves it)

extern "C" REX_FUNC(sub_8219D230) {
  SBX_CAR_MAKERS_FALLBACK(sub_8219D230)
  ShowBusyMarker(g);
}

extern "C" REX_FUNC(sub_8219D2D0) {
  SBX_CAR_MAKERS_FALLBACK(sub_8219D2D0)
  HideBusyMarker(g, false);
}

extern "C" REX_FUNC(sub_8219D350) {
  SBX_CAR_MAKERS_FALLBACK(sub_8219D350)
  HideBusyMarker(g, true);
}

// The rival list screen's update (no arguments).
extern "C" REX_FUNC(sub_8219F8A0) {
  SBX_CAR_MAKERS_FALLBACK(sub_8219F8A0)
  ctx.fpscr.disableFlushMode();
  RivalListUpdate(g);
}
