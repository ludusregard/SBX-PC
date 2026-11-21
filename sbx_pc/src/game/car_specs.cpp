// The car parameter tables as our own C++ (--native_car_specs, on by
// default): assets 72 to 83, set up and loaded once at boot into one
// block, byte-swapped by the loads' callbacks, then read by the getters all
// through the game; and Car::Refresh, which builds a car's working copy of
// its spec, tires, aero parts and wheels from the tables and its settings,
// with Tire::InchUp (the tire's size and grip for bigger rims). The
// archive, the heap and memcpy stay recompiled, called from here.
//
// SBX's car record has its settings 4 bytes earlier than ITC-Recomp's from
// +1652 on, so Car::Refresh and its helpers are written from SBX's code.

#include "base/settings.h"
#include "game/archive.h"
#include "game/car_spec.h"
#include "game/race_setup.h"

#include <bit>
#include <climits>
#include <cmath>

SBX_SETTING_BOOL(native_car_specs, true, "Overrides", "Cars",
                 "Our own C++ for the car parameter tables (their set-up, loads and byte "
                 "swaps, the getters, the aero parts) and Car::Refresh with Tire::InchUp "
                 "instead of the recompiled functions; the archive, the heap and memcpy "
                 "stay recompiled");

namespace {

using game::AeroPart;
using game::AeroPart36;
using game::AssetSize;
using game::Car;
using game::CarParams;
using game::CarSetup;
using game::CarSpec;
using game::EngineStage;
using game::Paint;
using game::Record83;
using game::SpecBlock1;
using game::SpecBlock2;
using game::SpecBlock264;
using game::SpecBlock2At132;
using game::SpecBlock2At8;
using game::SpecPaints;
using game::SpecVector;
using game::SpecWheel;
using game::SwapBytes;
using game::Tire;
using game::WheelColor;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::u32_be;

constexpr uint32_t kParams = 0x826B2B28;  // [it] the parameter object (writable)

// The loads' callbacks (Params::On...Loaded, below), by asset.
constexpr uint32_t kOnCarSpecsLoaded = 0x820CBFE8;  // 72
constexpr uint32_t kOnWheelsLoaded = 0x820CBE48;    // 73
constexpr uint32_t kOnLoaded = 0x820C9B90;          // 74 to 82
constexpr uint32_t kOnTable83Loaded = 0x820CBB30;   // 83

constexpr uint32_t kCarSpecsAsset = 72;
constexpr uint32_t kWheelsAsset = 73;
constexpr uint32_t kFirstPartsAsset = 74;  // To 82, one per parts table.
constexpr uint32_t kAsset83 = 83;

CarParams* Params() { return At<CarParams>(*At<u32_be>(kParams)); }

// A run of words swapped together (CarSpec::Swap3, Swap4, Swap5).
template <int N>
struct SpecWords {
  u32_be words[N];
};

// --- The swaps -------------------------------------------------------------
//
// The originals swap each word through a float register (lfs, stfs to the
// stack, the bytes stored reversed), which on the 360 keeps every word's
// bits as they are. The bytes are swapped here, which does the same. The
// recompiled round trip doesn't quite: a word that reads as a signaling NaN
// comes out quieted, so with the flag off such a word differs from the
// 360's. (The originals leave an address in r3; their callers are all
// here, so these return nothing.)

// CarSpec::SwapVector (sub_820C94F0): x, y, z swapped and w set to 1.0.
void SwapVector(SpecVector& v) {
  SwapBytes(v.xyz);
  v.w = 1.0f;
}

// CarSpec::Swap3, Swap4, Swap5 and SwapMatrix (16 words), on their own.
template <int N>
void SwapWords(SpecWords<N>& run) {
  SwapBytes(run.words);
}

// Wheel::SwapColor (sub_820CB2D0).
void SwapColor(WheelColor& color) {
  SwapBytes(color.unknown_0);
  SwapBytes(color.unknown_4);
  SwapBytes(color.unknown_8);
}

// Wheel::Swap (sub_820CB328): its two colors and four words.
void SwapWheel(SpecWheel& wheel) {
  SwapColor(wheel.colors[0]);
  SwapColor(wheel.colors[1]);
  SwapBytes(wheel.unknown_32);
  SwapBytes(wheel.model);
  SwapBytes(wheel.unknown_44);
}

// Paint::Swap (sub_820CB1F8): each paint's five halves, and its second
// shade by its type (solid: a dark gray; two-shade: the color's hue at
// 60% of its saturation, value 204; other types keep theirs); then the
// word after (the number of paints).
void SwapPaints(SpecPaints& paints) {
  for (Paint& paint : paints.paints) {
    SwapBytes(paint.type);
    SwapBytes(paint.hue);
    SwapBytes(paint.unknown_8);
    SwapBytes(paint.shade2_hue);
    SwapBytes(paint.unknown_16);
    if (paint.type == Paint::kSolid) {
      paint.shade2_hue = 0;
      paint.shade2_saturation = 0;
      paint.shade2_value = 25;
    } else if (paint.type == Paint::kTwoShade) {
      paint.shade2_hue = uint16_t(paint.hue);
      paint.shade2_saturation = uint8_t(int32_t(paint.saturation) * 60 / 100);
      paint.shade2_value = 204;
    }
  }
  SwapBytes(paints.unknown_268);
}

// CarSpec::SwapBlock1At264 (sub_820CA098): two runs of three words and 15
// words after.
void SwapBlock1At264(SpecBlock264& block) {
  SwapBytes(block.words_0);
  SwapBytes(block.words_16);
  SwapBytes(block.words_32);
}

// CarSpec::SwapBlock1 (sub_820CA2A0): its vectors, two matrices, the runs
// of five and three, the words at +368 (CarSpec::SwapBlock1At368,
// sub_820C9F38: four words, then four arrays of nine, the gear ratios
// among them) and the words after.
void SwapBlock1(SpecBlock1& block) {
  for (SpecVector& v : block.vectors) {
    SwapVector(v);
  }
  SwapBytes(block.matrices[0]);  // SwapMatrix
  SwapBytes(block.matrices[1]);
  SwapBytes(block.words_224);  // Swap5
  SwapBytes(block.words_244);
  SwapBlock1At264(block.block_264);
  SwapBytes(block.words_356);  // Swap3
  SwapBytes(block.words_368);
  SwapBytes(block.words_528);
}

// CarSpec::SwapBlock2At8 (sub_820CACC0): five words and the last swapped,
// the nine floats between set to 0.0.
void SwapBlock2At8(SpecBlock2At8& block) {
  SwapBytes(block.words_0);
  for (auto& v : block.zeros) {
    v = 0.0f;
  }
  SwapBytes(block.word_56);
}

// CarSpec::SwapBlock2At132 (sub_820CAE60): 42 words, three of them runs of
// five.
void SwapBlock2At132(SpecBlock2At132& block) {
  SwapBytes(block.words_0);
  for (auto& group : block.groups) {
    SwapBytes(group);  // Swap5
  }
  SwapBytes(block.words_100);
}

// CarSpec::SwapBlock2 (sub_820CBC10). The bytes at +68 and +128 and the
// model name aren't swapped.
void SwapBlock2(SpecBlock2& block) {
  SwapBlock2At8(block.block_8);
  SwapBytes(block.words_72);
  SwapBlock2At132(block.block_132);
  SwapBytes(block.words_300);  // Swap4
}

// EngineStage::Swap (sub_820CA780): its words from +8 (the first 8 bytes
// and the last 4 are bytes).
void SwapEngineStage(EngineStage& stage) { SwapBytes(stage.words); }

// Tire::Swap (sub_820CAA48): its words from +8.
void SwapTire(Tire& tire) {
  SwapBytes(tire.unknown_8);
  SwapBytes(tire.radius);
  SwapBytes(tire.width);
  SwapBytes(tire.unknown_52);
  SwapBytes(tire.grip_forward);
  SwapBytes(tire.grip_lateral);
  SwapBytes(tire.unknown_72);
  SwapBytes(tire.rim_diameter);
  SwapBytes(tire.aspect_ratio);
  SwapBytes(tire.rim_width);
}

// CarSpec::Swap (sub_820CBF08): the two blocks, 16 engine stages, 21 front
// and 21 rear tires (in pairs), the paints, the stock wheel.
void SwapCarSpec(CarSpec& spec) {
  SwapBlock1(spec.block1);
  SwapBlock2(spec.block2);
  for (EngineStage& stage : spec.stages) {
    SwapEngineStage(stage);
  }
  for (int i = 0; i < 21; i++) {
    SwapTire(spec.front_tires[i]);
    SwapTire(spec.rear_tires[i]);
  }
  SwapPaints(spec.paints);
  SwapWheel(spec.stock_wheel);
}

// Params::SwapCarSpecs (sub_820CBF90): every spec, the count and table
// read from the parameter object each time.
void SwapCarSpecs() {
  for (int32_t i = 0; i < int32_t(Params()->spec_count); i++) {
    SwapCarSpec(*Element<CarSpec>(Params()->specs, uint32_t(i)));
  }
}

// Params::SwapWheels (sub_820CBBB0): every wheel record (one at address 0
// skipped).
void SwapWheels() {
  for (int32_t i = 0; i < int32_t(Params()->wheel_count); i++) {
    if (SpecWheel* wheel = Element<SpecWheel>(Params()->wheels, uint32_t(i))) {
      SwapWheel(*wheel);
    }
  }
}

// Params::SwapTable83Record (sub_820CB3F8).
void SwapRecord83(Record83& record) { SwapBytes(record.words); }

// AeroPart::Swap (sub_820CB468) and AeroPart::Swap36 (sub_820CB588), on a
// copy of a record.
void SwapAeroPart(AeroPart& part) {
  SwapBytes(part.values);
  SwapBytes(part.scales);
  SwapBytes(part.unknown_24);
}

void SwapAeroPart36(AeroPart36& part) {
  SwapAeroPart(part.part);
  SwapBytes(part.unknown_28);
  SwapBytes(part.unknown_32);
}

// The archive's TOC (sub_8209D988), for asset `id`.
uint32_t Toc(Guest& g, uint32_t id) { return g.Call(sub_8209D988, id); }

// Archive::Load (sub_8209DF80): asset `id` into `to`, `callback` called
// with r6 = `context` once it's loaded; its serial.
uint32_t Load(Guest& g, uint32_t id, uint32_t to, uint32_t callback, uint32_t context) {
  return g.Call(sub_8209DF80, id, to, callback, context, 0u, 0u);
}

// --- The getters -----------------------------------------------------------

CarSpec* GetCarSpec(uint32_t id) { return Element<CarSpec>(Params()->specs, id); }

// Wheel number n (1 up), or none.
SpecWheel* GetWheel(int32_t n) {
  if (n < 1) {
    return nullptr;
  }
  return Element<SpecWheel>(Params()->wheels, uint32_t(n) - 1);
}

// The spec's tire: type 0 is record 0, else type x 5 + grade - 5.
Tire* GetTire(CarSpec* spec, int32_t type, uint32_t grade, bool rear) {
  const uint32_t index = type == 0 ? 0 : uint32_t(type) * 5 + grade - 5;
  return Element<Tire>(AddressOf(rear ? spec->rear_tires : spec->front_tires), index);
}

// --- The aero parts --------------------------------------------------------

// The parts block a caller passes (Params::GetCarParts' 9 words).
void ReadParts(uint32_t address, uint32_t parts[CarParams::kPartTables]) {
  const u32_be* words = At<u32_be>(address);
  for (int i = 0; i < CarParams::kPartTables; i++) {
    parts[i] = words[i];
  }
}

// memcpy (sub_8258CB90).
void Memcpy(Guest& g, void* to, uint32_t from, uint32_t size) {
  g.Call(sub_8258CB90, to, from, size);
}

// AeroParts::Get74 to Get82: the record for `level` (1 up) of table
// `table` copied to `out` and swapped (AeroPart::Swap, and Swap36); with
// no level, the defaults.
struct AeroGetter {
  int table;
  bool wide;           // The 36-byte parts (tables 81 and 82).
  bool positive_only;  // Get77: a level above 0 (ble), the others not 0.
};
constexpr AeroGetter kGet74{0, false, false}, kGet75{1, false, false}, kGet76{2, false, false},
    kGet77{3, false, true}, kGet78{4, false, false}, kGet79{5, false, false},
    kGet80{6, false, false}, kGet81{7, true, false}, kGet82{8, true, false};

// `out` is an AeroPart36; only the wide getters write its last two words.
void GetAeroPart(Guest& g, const AeroGetter& getter, AeroPart36& out,
                 const uint32_t parts[CarParams::kPartTables], int32_t level) {
  const uint32_t size = getter.wide ? sizeof(AeroPart36) : sizeof(AeroPart);
  const bool use = getter.positive_only ? level > 0 : level != 0;
  if (use) {
    Memcpy(g, &out, parts[getter.table] + size * uint32_t(level) - size, size);
    if (getter.wide) {
      SwapAeroPart36(out);
    } else {
      SwapAeroPart(out.part);
    }
    return;
  }
  for (auto& v : out.part.values) {
    v = 0.0f;
  }
  out.part.scales[0] = 1.0f;
  out.part.scales[1] = 1.0f;
  out.part.unknown_24 = 0;
  if (getter.wide) {
    out.unknown_28 = 0;
    out.unknown_32 = 0;
  }
}

void GetAeroPart(Guest& g, const AeroGetter& getter, AeroPart& out,
                 const uint32_t parts[CarParams::kPartTables], int32_t level) {
  GetAeroPart(g, getter, *At<AeroPart36>(AddressOf(&out)), parts, level);
}

// --- Car::Refresh ------------------------------------------------------------

// A fused multiply-add as the generated code does it (the product and sum
// in double, rounded to float once).
float Fma(float a, float b, float c) { return float(std::fma(double(a), double(b), double(c))); }

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

// The tire's radius (m) from its rim (inches), width (m) and aspect ratio.
float RadiusOf(float rim_diameter, float width, float aspect_ratio) {
  return Fma(width * aspect_ratio, 2000.0f, rim_diameter * 25.4f) * 0.0005f;
}

// How a tire's grip grows with its size: 1 + 3.67 sqrt(radius x width).
float GripFactor(float radius, float width) {
  return Fma(std::sqrt(radius * width), 3.67f, 1.0f);
}

// Tire::InchUp (sub_820C44C8): the tire on rims `inches` bigger (0:
// unchanged). The width grows 5% an inch, rounded down to 5 mm; the aspect
// ratio keeps the tire's radius; the rim width is 3.5 + (width mm - 135) /
// 20 inches (0.5 less from 175 mm), rounded down to a whole inch; and grip
// scales with GripFactor. Single-precision steps in the original's order.
void InchUp(Tire& tire, int32_t inches) {
  if (inches == 0) {
    return;
  }
  const float width = tire.width;
  const float rim = tire.rim_diameter;
  const float radius = tire.radius;
  const float grip = tire.grip_forward;
  const float old_radius = RadiusOf(rim, width, float(tire.aspect_ratio));

  const float new_rim = float(inches) + rim;
  tire.rim_diameter = new_rim;
  const float new_width = Fma(float(inches), 0.05f, 1.0f) * width;
  const float old_factor = GripFactor(old_radius, width);
  const int32_t width_mm = Truncate(new_width * 1000.0f);
  const float sidewalls = Fma(radius, 2.0f, -(new_rim * 0.0254f));
  const float grip_per_factor = grip / (old_factor * 0.5f * 1.05f);

  const float rounded_width = float(width_mm / 5 * 5) * 0.001f;
  tire.width = rounded_width;
  tire.aspect_ratio = sidewalls * 0.5f / rounded_width;

  double rim_width = std::fma(double(Fma(rounded_width, 1000.0f, -135.0f) * 0.1f), 0.5, 3.5);
  if (!(rounded_width < 0.175f)) {
    rim_width -= 0.5;
  }
  const int32_t half_inches = Truncate(float(rim_width) * 2.0f);
  tire.rim_width = float(int32_t(uint32_t(half_inches) * 5) / 10);

  const float new_radius = RadiusOf(new_rim, rounded_width, float(tire.aspect_ratio));
  const float grip_new = GripFactor(new_radius, rounded_width) * 0.5f * grip_per_factor * 1.05f;
  tire.grip_forward = grip_new;
  tire.grip_lateral = grip_new;
}

// Car::CopySpec (sub_820C4698): the spec's blocks, its engine stage, paints
// and the two tires into the car, the tires inched up, and how far that
// moved each tire's width.
void CopySpec(Guest& g, CarSetup& car, CarSpec* spec) {
  car.front_width_change = 0.0f;
  car.rear_width_change = 0.0f;
  Memcpy(g, &car.block1, AddressOf(&spec->block1), sizeof(SpecBlock1));
  Memcpy(g, &car.block2, AddressOf(&spec->block2), sizeof(SpecBlock2));
  Memcpy(g, &car.engine,
         AddressOf(spec->stages) + uint32_t(sizeof(EngineStage)) * uint32_t(car.engine_stage),
         sizeof(EngineStage));
  Memcpy(g, &car.paints, AddressOf(&spec->paints), sizeof(SpecPaints));
  Memcpy(g, &car.front_tire,
         AddressOf(GetTire(spec, car.front_tire_type, car.front_tire_grade, false)), sizeof(Tire));
  Memcpy(g, &car.rear_tire,
         AddressOf(GetTire(spec, car.rear_tire_type, car.rear_tire_grade, true)), sizeof(Tire));
  const float front = car.front_tire.width;
  const float rear = car.rear_tire.width;
  InchUp(car.front_tire, car.front_inch_up);
  InchUp(car.rear_tire, car.rear_inch_up);
  car.front_width_change = car.front_tire.width - front;
  car.rear_width_change = car.rear_tire.width - rear;
}

// Car::CopyAeroParts (sub_820C4378): each part by its level (the rear
// spoiler's -1 read as 0; the bonnet from table 78, or 79 by aero level
// 5). The original builds each in its frame and copies it in; the result
// in the car is the same.
void CopyAeroParts(Guest& g, CarSetup& car, const uint32_t parts[CarParams::kPartTables]) {
  const auto& level = car.aero_levels;
  const int32_t level_77 = level[3] == -1 ? 0 : int32_t(level[3]);
  GetAeroPart(g, kGet74, car.aero_74, parts, level[0]);
  GetAeroPart(g, kGet75, car.aero_75, parts, level[1]);
  GetAeroPart(g, kGet76, car.aero_76, parts, level[2]);
  GetAeroPart(g, kGet77, car.aero_77, parts, level_77);
  GetAeroPart(g, kGet80, car.aero_80, parts, level[6]);
  GetAeroPart(g, kGet81, car.aero_81, parts, level[9]);
  GetAeroPart(g, kGet82, car.aero_82, parts, level[10]);
  GetAeroPart(g, level[5] == 0 ? kGet78 : kGet79, car.aero_78, parts, level[4]);
}

// A wheel: the spec's stock one for 0, else table 73's.
uint32_t WheelOf(int32_t n, CarSpec* spec) {
  return n == 0 ? AddressOf(&spec->stock_wheel) : AddressOf(GetWheel(n));
}

}  // namespace

// --- The set-up and the loads (once, at boot) --------------------------------

// Params::Init: the parameter object made (operator new) into
// [0x826B2B28], no wheel model yet, then two passes over the twelve assets:
// the first works out the sizes (each table 0, there being no block yet),
// then the block (operator new[] of the total), and the second sets each
// table's address in it. The loads pending are counted up from 0 for each
// table, each pass. A total of 0 would loop for good, as the original.
void game::InitCarParams(Guest& g) {
  const uint32_t object = g.Call(sub_8258C000, uint32_t(sizeof(CarParams)));  // operator new
  *At<u32_be>(kParams) = object;
  At<CarParams>(object)->block = 0;
  Params()->spec_count = 0;
  Params()->spec_count = 0;
  Params()->wheel_count = 0;
  for (auto& first : Params()->first_wheel_of_model) {
    first = uint32_t(-1);
  }
  Params()->loads_pending = 0;
  if (Params()->block != 0) {
    return;
  }
  // The table `size` bytes into the block, or 0 while there's no block
  // (`at` the table before it, 0 then too).
  auto after = [](uint32_t at, uint32_t size) { return at != 0 ? Params()->block + size : 0; };
  uint32_t total = 0;
  for (;;) {
    if (total != 0) {
      Params()->block = g.Call(sub_8258C0C0, total);  // operator new[]
    }
    uint32_t table = Params()->block;
    Params()->loads_pending = 0;
    Params()->loads_pending += 1;
    Params()->specs = table;
    uint32_t size = AssetSize(Toc(g, kCarSpecsAsset), kCarSpecsAsset);
    table = after(table, size);

    Params()->loads_pending += 1;
    Params()->wheels = table;
    size += AssetSize(Toc(g, kWheelsAsset), kWheelsAsset);
    table = after(table, size);

    for (uint32_t k = 0; k < CarParams::kPartTables; k++) {
      Params()->loads_pending += 1;
      Params()->parts[k] = table;
      size += AssetSize(Toc(g, kFirstPartsAsset + k), kFirstPartsAsset + k);
      table = after(table, size);
    }

    Params()->loads_pending += 1;
    Params()->table_83 = table;
    total = AssetSize(Toc(g, kAsset83), kAsset83) + size;
    if (Params()->block != 0) {
      return;
    }
  }
}

// Params::Load: the twelve loads into the tables Init set, the parameter
// object as each callback's context.
void game::LoadCarParams(Guest& g) {
  Load(g, kCarSpecsAsset, Params()->specs, kOnCarSpecsLoaded, AddressOf(Params()));
  Load(g, kWheelsAsset, Params()->wheels, kOnWheelsLoaded, AddressOf(Params()));
  for (uint32_t k = 0; k < CarParams::kPartTables; k++) {
    Load(g, kFirstPartsAsset + k, Params()->parts[k], kOnLoaded, AddressOf(Params()));
  }
  Load(g, kAsset83, Params()->table_83, kOnTable83Loaded, AddressOf(Params()));
}

// Params::Free: the block and the object freed.
void game::FreeCarParams(Guest& g) {
  g.Call(sub_8258C940, Params()->block);  // operator delete[]
  g.Call(sub_8258C868, Params());         // operator delete
  *At<u32_be>(kParams) = 0;
}

// Params::GetCarParts: tables 74 to 82, at car `car`'s records.
void game::GetCarParts(uint32_t car, uint32_t parts[CarParams::kPartTables]) {
  static constexpr uint32_t kRecordBytes[CarParams::kPartTables] = {280, 280, 280, 280, 168,
                                                                    168, 84,  108, 72};
  CarParams* params = Params();
  for (int i = 0; i < CarParams::kPartTables; i++) {
    parts[i] = params->parts[i] + kRecordBytes[i] * car;
  }
}

void game::GetBodyKitPart(Guest& g, AeroPart36& out, const uint32_t parts[CarParams::kPartTables],
                          int32_t level) {
  g.ctx.fpscr.disableFlushMode();  // As the original's float loads leave it.
  GetAeroPart(g, kGet82, out, parts, level);
}

// Car::Refresh: its spec (by its id at +12), the parts of that car (of car
// 0 when the spec says it has none of its own), the aero parts, the two
// wheels.
void game::RefreshCar(Guest& g, Car& car) {
  g.ctx.fpscr.disableFlushMode();  // As the original's float loads leave it.
  CarSetup& setup = car.setup;
  CarSpec* spec = GetCarSpec(car.car_id);
  CopySpec(g, setup, spec);
  uint32_t parts[CarParams::kPartTables];
  GetCarParts(spec->block2.uses_car0_parts != 0 ? 0 : uint32_t(car.car_id), parts);
  CopyAeroParts(g, setup, parts);
  Memcpy(g, &setup.front_wheel_record, WheelOf(setup.front_wheel, spec), sizeof(SpecWheel));
  Memcpy(g, &setup.rear_wheel_record, WheelOf(setup.rear_wheel, spec), sizeof(SpecWheel));
}

#define SBX_CAR_SPECS_FALLBACK(name)     \
  REX_FUNC_PROLOGUE();                   \
  if (!SBX_GET(native_car_specs)) {  \
    __imp__##name(ctx, base);            \
    return;                              \
  }                                      \
  Guest g(ctx, base);

// --- The set-up and the loads (from BootData::Load) ---

extern "C" REX_FUNC(sub_820C9700) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9700)
  game::InitCarParams(g);
}

extern "C" REX_FUNC(sub_820CC048) {
  SBX_CAR_SPECS_FALLBACK(sub_820CC048)
  game::LoadCarParams(g);
}

extern "C" REX_FUNC(sub_820C9B30) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9B30)
  game::FreeCarParams(g);
}

// --- The loads' callbacks (through Archive::Load's function pointer): r6 =
// the parameter object. They turn flush-to-zero off, as the originals' lfs
// leave it.

// Table 72: the spec count, then each spec swapped.
extern "C" REX_FUNC(sub_820CBFE8) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBFE8)
  CarParams* object = At<CarParams>(ctx.r6.u32);
  const uint32_t toc = Toc(g, kCarSpecsAsset);
  ctx.fpscr.disableFlushMode();
  object->spec_count = AssetSize(toc, kCarSpecsAsset) / uint32_t(sizeof(CarSpec));
  SwapCarSpecs();
  object->loads_pending -= 1;
}

// Table 73: the wheel count, each wheel swapped, each record's model up by
// one, and each model's first record kept.
extern "C" REX_FUNC(sub_820CBE48) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBE48)
  CarParams* object = At<CarParams>(ctx.r6.u32);
  const uint32_t toc = Toc(g, kWheelsAsset);
  ctx.fpscr.disableFlushMode();
  object->wheel_count = AssetSize(toc, kWheelsAsset) / uint32_t(sizeof(SpecWheel));
  SwapWheels();
  int32_t model = 1;
  object->first_wheel_of_model[1] = 0;
  for (int32_t i = 0; i < int32_t(object->wheel_count); i++) {
    SpecWheel* wheel = Element<SpecWheel>(object->wheels, uint32_t(i));
    wheel->model += 1;
    if (wheel->model != model) {
      model = wheel->model;
      *Element<u32_be>(AddressOf(object->first_wheel_of_model), uint32_t(model)) = uint32_t(i);
    }
  }
  object->model_count = uint32_t(model + 1);
  object->loads_pending -= 1;
}

// Tables 74 to 82: nothing to swap here (their getters swap a copy).
extern "C" REX_FUNC(sub_820C9B90) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9B90)
  At<CarParams>(ctx.r6.u32)->loads_pending -= 1;
}

// Table 83: its 40-byte records swapped.
extern "C" REX_FUNC(sub_820CBB30) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBB30)
  CarParams* object = At<CarParams>(ctx.r6.u32);
  const uint32_t toc = Toc(g, kAsset83);
  ctx.fpscr.disableFlushMode();
  const uint32_t count = AssetSize(toc, kAsset83) / uint32_t(sizeof(Record83));
  for (uint32_t i = 0; int32_t(count) > 0 && i < count; i++) {
    SwapRecord83(*Element<Record83>(object->table_83, i));
  }
  object->loads_pending -= 1;
}

// --- The swaps on their own: r3 the record (the two loops none). Only the
// loads' callbacks above call them; some of their callers keep r4 or r7
// across the call, which these don't touch.

#define SBX_CAR_SPECS_SWAP(name, Type, swap)  \
  extern "C" REX_FUNC(name) {                 \
    SBX_CAR_SPECS_FALLBACK(name)              \
    ctx.fpscr.disableFlushMode();             \
    swap(*At<Type>(ctx.r3.u32));              \
  }

SBX_CAR_SPECS_SWAP(sub_820C94F0, SpecVector, SwapVector)
SBX_CAR_SPECS_SWAP(sub_820C9CB0, SpecWords<16>, SwapWords)  // SwapMatrix
SBX_CAR_SPECS_SWAP(sub_820CAD90, SpecWords<5>, SwapWords)   // Swap5
SBX_CAR_SPECS_SWAP(sub_820CA220, SpecWords<3>, SwapWords)   // Swap3
SBX_CAR_SPECS_SWAP(sub_820CB150, SpecWords<4>, SwapWords)   // Swap4
SBX_CAR_SPECS_SWAP(sub_820C9F38, SpecWords<40>, SwapWords)  // SwapBlock1At368 (its +368 words)
SBX_CAR_SPECS_SWAP(sub_820CA098, SpecBlock264, SwapBlock1At264)
SBX_CAR_SPECS_SWAP(sub_820CA2A0, SpecBlock1, SwapBlock1)
SBX_CAR_SPECS_SWAP(sub_820CACC0, SpecBlock2At8, SwapBlock2At8)
SBX_CAR_SPECS_SWAP(sub_820CAE60, SpecBlock2At132, SwapBlock2At132)
SBX_CAR_SPECS_SWAP(sub_820CBC10, SpecBlock2, SwapBlock2)
SBX_CAR_SPECS_SWAP(sub_820CA780, EngineStage, SwapEngineStage)
SBX_CAR_SPECS_SWAP(sub_820CAA48, Tire, SwapTire)
SBX_CAR_SPECS_SWAP(sub_820CB1F8, SpecPaints, SwapPaints)
SBX_CAR_SPECS_SWAP(sub_820CB328, SpecWheel, SwapWheel)
SBX_CAR_SPECS_SWAP(sub_820CB2D0, WheelColor, SwapColor)
SBX_CAR_SPECS_SWAP(sub_820CB3F8, Record83, SwapRecord83)
SBX_CAR_SPECS_SWAP(sub_820CB468, AeroPart, SwapAeroPart)
SBX_CAR_SPECS_SWAP(sub_820CB588, AeroPart36, SwapAeroPart36)
SBX_CAR_SPECS_SWAP(sub_820CBF08, CarSpec, SwapCarSpec)

#undef SBX_CAR_SPECS_SWAP

// Params::SwapCarSpecs and Params::SwapWheels.
extern "C" REX_FUNC(sub_820CBF90) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBF90)
  ctx.fpscr.disableFlushMode();
  SwapCarSpecs();
}

extern "C" REX_FUNC(sub_820CBBB0) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBBB0)
  ctx.fpscr.disableFlushMode();
  SwapWheels();
}

// --- The getters

// Params::GetCarSpec: r3 the car id.
extern "C" REX_FUNC(sub_820C9C38) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9C38)
  ctx.r3.u64 = AddressOf(GetCarSpec(ctx.r3.u32));
}

// Params::GetWheel: r3 the wheel number; 0 below 1.
extern "C" REX_FUNC(sub_820C9C50) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9C50)
  ctx.r3.u64 = AddressOf(GetWheel(ctx.r3.s32));
}

// Params::FirstWheelOfModel: r3 the model; its first record + 1, or 0.
extern "C" REX_FUNC(sub_820C9C80) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9C80)
  const int32_t model = ctx.r3.s32;
  if (model < 1) {
    ctx.r3.u64 = 0;
    return;
  }
  const u32_be* first = Element<u32_be>(AddressOf(Params()->first_wheel_of_model), uint32_t(model));
  ctx.r3.u64 = uint32_t(*first + 1);
}

// Params::Exists.
extern "C" REX_FUNC(sub_820C9B78) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9B78)
  ctx.r3.u64 = Params() != nullptr ? 1 : 0;
}

// CarSpec::GetTire: r3 the spec, r4 the type, r5 the grade, r6 rear.
extern "C" REX_FUNC(sub_820C96C8) {
  SBX_CAR_SPECS_FALLBACK(sub_820C96C8)
  // Worked out as guest addresses (the spec isn't read).
  const uint32_t spec = ctx.r3.u32;
  const int32_t type = ctx.r4.s32;
  const uint32_t index = type == 0 ? 0 : uint32_t(type) * 5 + ctx.r5.u32 - 5;
  const uint32_t tires = ctx.r6.s32 == 0 ? offsetof(CarSpec, front_tires) : offsetof(CarSpec, rear_tires);
  ctx.r3.u64 = uint32_t(index * sizeof(Tire) + spec + tires);
}

// Params::GetCarParts: r3 the 9 words out (left in r3, which its callers
// read), r4 the car id.
extern "C" REX_FUNC(sub_820C9BA0) {
  SBX_CAR_SPECS_FALLBACK(sub_820C9BA0)
  uint32_t parts[CarParams::kPartTables];
  game::GetCarParts(ctx.r4.u32, parts);
  u32_be* out = At<u32_be>(ctx.r3.u32);
  for (int i = 0; i < CarParams::kPartTables; i++) {
    out[i] = parts[i];
  }
}

// The aero part getters: r3 the part out (returned), r4 the parts (9
// pointers), r5 the level.
#define SBX_AERO_GETTER(name, getter)                        \
  extern "C" REX_FUNC(name) {                                \
    SBX_CAR_SPECS_FALLBACK(name)                             \
    const uint32_t out = ctx.r3.u32;                         \
    uint32_t parts[CarParams::kPartTables];                  \
    ReadParts(ctx.r4.u32, parts);                            \
    const int32_t level = ctx.r5.s32;                        \
    ctx.fpscr.disableFlushMode();                            \
    GetAeroPart(g, getter, *At<AeroPart36>(out), parts, level); \
    ctx.r3.u64 = out;                                        \
  }

SBX_AERO_GETTER(sub_820CB5E8, kGet74)
SBX_AERO_GETTER(sub_820CB670, kGet75)
SBX_AERO_GETTER(sub_820CB6F8, kGet76)
SBX_AERO_GETTER(sub_820CB780, kGet77)
SBX_AERO_GETTER(sub_820CB808, kGet78)
SBX_AERO_GETTER(sub_820CB890, kGet79)
SBX_AERO_GETTER(sub_820CB918, kGet80)
SBX_AERO_GETTER(sub_820CB9A0, kGet81)
SBX_AERO_GETTER(sub_820CBA30, kGet82)

#undef SBX_AERO_GETTER

// r3 the parts: how many of the last parts table's (+32) first two
// records (the body kits) have a first float other than 0.0. The original
// copies each to its frame and swaps it there; only that float is read.
extern "C" REX_FUNC(sub_820CBAC0) {
  SBX_CAR_SPECS_FALLBACK(sub_820CBAC0)
  ctx.fpscr.disableFlushMode();
  const u32_be* parts = At<u32_be>(ctx.r3.u32);
  const AeroPart36* kits = At<AeroPart36>(parts[kGet82.table]);
  uint32_t count = 0;
  for (int i = 0; i < 2; i++) {
    const uint32_t raw = kits[i].part.values[0].raw;  // Little-endian in the table.
    if (std::bit_cast<float>(raw) != 0.0f) {
      count++;
    }
  }
  ctx.r3.u64 = count;
}

// --- Car::Refresh and its steps

// Tire::InchUp: r3 the tire, r4 the inches.
extern "C" REX_FUNC(sub_820C44C8) {
  SBX_CAR_SPECS_FALLBACK(sub_820C44C8)
  ctx.fpscr.disableFlushMode();
  InchUp(*At<Tire>(ctx.r3.u32), ctx.r4.s32);
}

// Car::CopySpec: r3 the car, r4 its spec.
extern "C" REX_FUNC(sub_820C4698) {
  SBX_CAR_SPECS_FALLBACK(sub_820C4698)
  ctx.fpscr.disableFlushMode();
  CopySpec(g, At<Car>(ctx.r3.u32)->setup, At<CarSpec>(ctx.r4.u32));
}

// Car::CopyAeroParts: r3 the car, r4 its parts (Params::GetCarParts').
extern "C" REX_FUNC(sub_820C4378) {
  SBX_CAR_SPECS_FALLBACK(sub_820C4378)
  ctx.fpscr.disableFlushMode();
  uint32_t parts[CarParams::kPartTables];
  ReadParts(ctx.r4.u32, parts);
  CopyAeroParts(g, At<Car>(ctx.r3.u32)->setup, parts);
}

// Car::Refresh: r3 the car.
extern "C" REX_FUNC(sub_820C47A8) {
  SBX_CAR_SPECS_FALLBACK(sub_820C47A8)
  game::RefreshCar(g, *At<Car>(ctx.r3.u32));
}
