// Garage wheels in the parking area.
//
// The game builds each car scene from one of two model sets: 0, set a, the
// garage's full models, or 1, set b, the race's lighter ones (CarScene::SetUp,
// sub_820C8B88, the record's first word). The parking area's scene
// (ParkingArea::SetUpScene, sub_82225548) asks for set b, as the race's
// (sub_8215A250) does; only the garage stage (sub_8217E9F0) asks for set a.
// So the parked cars are set b cars: their bodies are a little lighter than
// the garage's, and their wheels are the race's simple ones.
//
// Each wheel has a part for each set (docs/game_data.md, "Wheels"): wheel i's
// set a part is 587 + 3i (`A0_12/20`, the nuts, `punch_12/20`; 1,300 to 2,300
// vertices a rim) and its set b part 588 + 3i (`B0`-`B2_12/20`, `B_space`;
// about 61 vertices each). Three places pick one by the model set:
//
// 1. Wheels::SetUp (sub_820D8DD8, src/game/car_models.cpp), by the first
//    pool set's +8: Wheels::PickSetA (sub_820D8860) or PickSetB
//    (sub_820D8900) for the part to load.
// 2. WheelPoints::SetUp (sub_820E8250), by the car scene's +4: links the
//    loaded part to `rims_a` (shape names at 0x825F2964) or `rims_b`
//    (0x825F2980).
// 3. SceneWheels::Draw (sub_820ECA60, then sub_820EC668 or sub_820EC7A0),
//    by the car scene's +4: steps `rims_a` or `rims_b`.
//
// --parking_area_garage_wheels (on by default): in the parking area, 1
// picks set a's part, and 2 and 3 run with the car scene's +4 at 0 for the
// call (put back after), so all three agree on set a's wheels. The bodies
// stay set b's, with their shadow part (set a has none). The tires (581,
// 583), the calipers (886) and the wheel textures (586 + 3i) are the same in
// both sets. Nothing else under 2 and 3 reads the car scene's +4 (their
// callees read only its +5024).
//
// The flag is taken when a car scene is set up, so turning it on or off
// applies from the next scene (leaving and entering the parking area).

#include "hooks/parking_area_wheels.h"

#include "base/settings.h"
#include "guest.h"

#include <rex/logging.h>

SBX_SETTING_BOOL(parking_area_garage_wheels, true, "Hooks", "Cars",
                 "Give the parking area's cars the garage's detailed wheels instead of the "
                 "race's simple ones (from the next time the parking area is entered)");

namespace {

constexpr uint32_t kCarScene = 0x826B2B24;  // Holds the car scene (writable).
constexpr uint32_t kSceneModelSet = 4;      // The car scene's model set.

bool garage_wheels = false;  // Taken at the last car scene set-up.

// Runs `original` with the car scene's model set at 0 (set a) for the call
// when the parking area wants garage wheels.
void AsSetA(PPCContext& ctx, uint8_t* base, PPCFunc* original) {
  const uint32_t scene = guest::LoadU32(base, kCarScene);
  if (!garage_wheels || scene == 0) {
    original(ctx, base);
    return;
  }
  const uint32_t field = scene + kSceneModelSet;
  const uint32_t set = guest::LoadU32(base, field);
  guest::StoreU32(base, field, 0);
  original(ctx, base);
  guest::StoreU32(base, field, set);
}

}  // namespace

namespace hooks {

bool ParkingArea_GarageWheels() { return garage_wheels; }

}  // namespace hooks

// ParkingArea::SetUpScene: the parking area's course scenery and car scene
// (set b, seven car models).
extern "C" REX_FUNC(sub_82225548) {
  garage_wheels = SBX_GET(parking_area_garage_wheels);
  if (garage_wheels) {
    REXLOG_INFO("parking_area_garage_wheels: parking area scene: garage wheels");
  }
  __imp__sub_82225548(ctx, base);
}

// The race's car scene set-up (set b).
extern "C" REX_FUNC(sub_8215A250) {
  garage_wheels = false;
  __imp__sub_8215A250(ctx, base);
}

// GarageStage's car scene set-up (set a: already the garage's wheels).
extern "C" REX_FUNC(sub_8217E9F0) {
  garage_wheels = false;
  __imp__sub_8217E9F0(ctx, base);
}

// WheelPoints::SetUp: r3 the points' links.
extern "C" REX_FUNC(sub_820E8250) { AsSetA(ctx, base, __imp__sub_820E8250); }

// SceneWheels::Draw: r3 the scene wheels.
extern "C" REX_FUNC(sub_820ECA60) { AsSetA(ctx, base, __imp__sub_820ECA60); }
