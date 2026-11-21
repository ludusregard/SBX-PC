// The race camera as our own C++ (--native_race_camera, on by default): the
// mode switch, the update that reads the view button, the race start's and
// the restore's view, the cockpit camera, and the viewports' small setters.
// The chase, hood, TV and scripted cameras' own updates, Viewport::Update and
// the matrix building stay recompiled. See game/race_camera.h for how a
// frame's camera is made.
//
// RaceCamera::SetMode (sub_82157188) and RaceCamera::Update (sub_821573E0)
// keep subtick's overrides (subtick/camera_cuts.cpp, per_frame_logic.cpp),
// which call the functions here (hooks/native_calls.h).

#include "game/race_camera.h"

#include "base/settings.h"
#include "game/race_setup.h"
#include "game/race_states.h"
#include "game/save_data.h"
#include "hooks/native_calls.h"

#include <cmath>

SBX_SETTING_BOOL(native_race_camera, true, "Overrides", "Race",
                 "Our own C++ for the race camera's mode switch, update, start and restore, "
                 "the cockpit camera and the viewports' small setters instead of the "
                 "recompiled functions");

namespace {

using game::CameraPass;
using game::CarSceneItem;
using game::CockpitCameraObject;
using game::CockpitPoint;
using game::ControlRecord;
using game::GameObject;
using game::Matrix;
using game::MessageDispatcher;
using game::RaceData;
using game::RaceModeMember;
using game::Vector;
using game::Viewport;
using game::Viewports;
using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::u32_be;

bool On() { return SBX_GET(native_race_camera); }

// The race camera's modes (RaceModeMember::camera_mode, Viewport::mode).
enum CameraMode : uint32_t {
  kChase = 0,
  kCockpit = 1,
  kHood = 2,
  kMode3 = 3,  // Not identified.
  kTv = 4,
  kScripted = 5,
};

// RaceData::play_mode values the camera checks.
constexpr uint32_t kPlaySplitScreen = 4;
constexpr uint32_t kPlayMode6 = 6;

// The pad buttons the race camera reads.
constexpr uint32_t kViewButton = 0x40;
constexpr uint32_t kChaseResetButton = 0x80;

// RaceData::camera_flags.
constexpr uint32_t kTvInViews = 0x80;
constexpr uint32_t kCameraFlag20 = 0x20;
constexpr uint32_t kCameraFlag40 = 0x40;

// The cockpit camera's vtable (its update is CockpitCamera::Update below).
constexpr uint32_t kCockpitCameraVtable = 0x82006AC4;

// The display: [0x8265CD68], whose mode (virtual 7, then 3) has the screen
// height at +296.
constexpr uint32_t kDisplay = 0x8265CD68;
struct DisplayMode {
  uint8_t unknown_0[296];
  guest::i32_be unknown_296;  // The screen height (the camera's field of view scales with it).
};

// The player car's scene (its parts and models), and a word whose +4 picks
// the scene's second car model for the cockpit (with play mode 6).
constexpr uint32_t kPlayerCarScene = 0x826B2B24;
constexpr uint32_t kSecondCarModel = 0x826B72E0;

// The cockpit camera's field of view (before scaling for the screen) and
// its near clip.
constexpr float kCockpitFov = 55.0f;
constexpr float kCockpitNearClip = 0.1f;

// The look stick turns the cockpit camera by up to 60 degrees each way.
constexpr float kLookStickScale = 3.0518509447574615e-05f;  // 1 / 32767
constexpr float kLookRange = -1.0471975803375244f;           // -60 degrees

// Posted when RaceCamera::Update's step for camera_flags 0x20/0x40 changes
// state 2 or 4 (what reads it isn't traced).
constexpr uint32_t kMessage58 = 58;

constexpr float kPi = 3.14159274f;
constexpr float kTwoPi = 6.28318548f;

Viewports* GetViewports() { return At<Viewports>(*At<u32_be>(game::kViewports)); }
RaceData* GetRaceData() { return At<RaceData>(*At<u32_be>(game::kRaceData)); }
CameraPass* MainCameraPass() { return At<CameraPass>(game::kCameraPasses); }

// --- Small vector math (the game's row-vector convention: v * M) ---

struct Vec4 {
  float x, y, z, w;
};

Vec4 Load(const Vector& v) { return {v.x, v.y, v.z, v.w}; }

// The cockpit camera's car-space defaults (its start writes them).
constexpr Vec4 kCockpitEyeOffset = {-0.32f, 1.0f, -0.3f, 1.0f};
constexpr Vec4 kCockpitTargetOffset = {1.0f, 1.0f, -0.3f, 1.0f};
constexpr Vec4 kCockpitUpOffset = {0.0f, 1.0f, 0.0f, 1.0f};
constexpr Vec4 kCockpitAlternateTargetOffset = {-0.8f, 1.1f, 0.0f, 1.0f};
void Store(Vector& to, const Vec4& v) {
  to.x = v.x;
  to.y = v.y;
  to.z = v.z;
  to.w = v.w;
}

struct Mat4 {
  Vec4 rows[4];
};

Mat4 Load(const Matrix& m) { return {{Load(m.right), Load(m.up), Load(m.forward), Load(m.position)}}; }

float Component(const Vec4& v, int i) {
  switch (i) {
    case 0: return v.x;
    case 1: return v.y;
    case 2: return v.z;
    default: return v.w;
  }
}

// v * m: the point (or direction, w = 0) v moved into m's space.
Vec4 Transform(const Vec4& v, const Mat4& m) {
  Vec4 out;
  float* o = &out.x;
  for (int c = 0; c < 4; c++) {
    o[c] = v.x * Component(m.rows[0], c) + v.y * Component(m.rows[1], c) +
           v.z * Component(m.rows[2], c) + v.w * Component(m.rows[3], c);
  }
  return out;
}

// a * b.
Mat4 Multiply(const Mat4& a, const Mat4& b) {
  Mat4 out;
  for (int r = 0; r < 4; r++) {
    out.rows[r] = Transform(a.rows[r], b);
  }
  return out;
}

Mat4 Translation(const Vec4& position) {
  return {{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {position.x, position.y, position.z, 1}}};
}

// A turn about the up (y) axis.
Mat4 RotationY(float angle) {
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  return {{{c, 0, -s, 0}, {0, 1, 0, 0}, {s, 0, c, 0}, {0, 0, 0, 1}}};
}

// The direction from `from` to `to`, of length 1 (all 0 if they're the
// same point). w is as the game leaves it (1 scaled with the rest); only
// x, y and z are used.
Vec4 Direction(const Vector& from, const Vector& to) {
  Vec4 d = {to.x - from.x, to.y - from.y, to.z - from.z, 1.0f};
  const float length_squared = d.x * d.x + d.y * d.y + d.z * d.z;
  if (length_squared == 0.0f) {
    return {0, 0, 0, 0};
  }
  const float scale = 1.0f / std::sqrt(length_squared);
  return {d.x * scale, d.y * scale, d.z * scale, d.w * scale};
}

// --- Camera modes' shared steps ---

// The car's pose a camera mode follows: points[0] its position,
// points[4..7] its rotation (CameraMode::FollowCar, sub_82151358, fills them).
Vec4 CarPosition(const GameObject& camera) { return Load(camera.points[0]); }
Mat4 CarRotation(const GameObject& camera) {
  return {{Load(camera.points[4]), Load(camera.points[5]), Load(camera.points[6]),
           Load(camera.points[7])}};
}

void FollowCar(Guest& g, GameObject* camera) {
  g.Call(sub_82151358, &camera->points[0], camera->car);  // CameraMode::FollowCar
}

// The screen's height, from the display's mode.
int32_t ScreenHeight(Guest& g) {
  const uint32_t display = *At<u32_be>(kDisplay);
  const uint32_t mode = g.CallVirtual(At<uint8_t>(display), 7);
  const uint32_t settings = g.CallVirtual(At<uint8_t>(mode), 3);
  return At<DisplayMode>(settings)->unknown_296;
}

// A camera's field of view for this screen (CameraMode::FovForScreen,
// sub_82150A50, from the base angle in degrees and the screen height).
float FovForScreen(Guest& g, float degrees) {
  g.ctx.f1.f64 = double(degrees);
  g.ctx.f2.f64 = double(float(ScreenHeight(g)));
  g.Call(sub_82150A50);
  return float(g.ctx.f1.f64);
}

void SetFov(Guest& g, Viewport* viewport, float fov) {
  g.ctx.f1.f64 = double(fov);
  g.Call(sub_820AAE48, viewport);  // Viewport::SetFov
}

// Viewport::SetNearClip: the main camera pass's +384 (likely its near
// clip). Every viewport shares pass 0.
void SetNearClip(float value) { MainCameraPass()->unknown_384 = value; }

// Viewport::SetRoll: the roll of every view but view 18, brought into
// -pi..pi; the rear-view mirror (view 1) rolls the other way.
void SetRoll(Viewport* viewport, float angle) {
  float roll = angle + kPi;
  if (std::isfinite(roll)) {
    while (roll > kTwoPi) {
      roll -= kTwoPi;
    }
    while (roll < 0.0f) {
      roll += kTwoPi;
    }
  }
  roll -= kPi;
  for (int view = 0; view < Viewport::kViews; view++) {
    if (view == 18) {
      continue;
    }
    viewport->views[view].roll = view == 1 ? -roll : roll;
  }
}

// --- The cockpit camera (mode 1) ---

// The cockpit points for the car the cockpit camera shows, from the car
// model in the player car's scene. The game's own table (kCockpitPoints)
// is in writable data, so it's read live.
bool FindCockpitPoint(Guest& g, CockpitPoint* point) {
  RaceData* race = GetRaceData();
  int32_t which_model = 0;
  if (race->play_mode == kPlayMode6 && race->unknown_444820 == 2) {
    which_model = At<u32_be>(kSecondCarModel)[1] == 0 ? 1 : 0;
  }
  const uint32_t item_address = g.Call(sub_820C5B28, *At<u32_be>(kPlayerCarScene), 2, which_model);
  const CarSceneItem* item = At<CarSceneItem>(item_address);  // CarScene::FindItem
  if (item == nullptr) {
    return false;
  }
  *point = *guest::Element<CockpitPoint>(game::kCockpitPoints, item->model_block);
  return true;
}

// The field of view, near clip and roll every cockpit update sets.
void SetCockpitLens(Guest& g, Viewport* viewport) {
  SetFov(g, viewport, FovForScreen(g, kCockpitFov));
  SetNearClip(kCockpitNearClip);
  SetRoll(viewport, 0.0f);
}

// CockpitCamera::Start: r3 the camera, r4 the viewport, r5 the car. The
// camera set up and placed in the car. Returns the camera.
void StartCockpit(Guest& g, CockpitCameraObject* cockpit, Viewport* viewport, uint32_t car) {
  GameObject& camera = cockpit->camera.object;
  camera.car = 0;
  camera.viewport = 0;
  for (Vector& point : camera.points) {
    point.w = 1.0f;
  }
  for (Vector& point : camera.points_272) {
    point.w = 1.0f;
  }
  camera.vtable = kCockpitCameraVtable;
  cockpit->camera.unknown_1088.w = 1.0f;

  camera.car = car;
  camera.viewport = AddressOf(viewport);
  SetCockpitLens(g, viewport);
  Store(viewport->eye_offset, kCockpitEyeOffset);
  Store(viewport->target_offset, kCockpitTargetOffset);
  Store(viewport->up_offset, kCockpitUpOffset);
  Store(viewport->alternate_target_offset, kCockpitAlternateTargetOffset);
  g.Call(sub_820BFB20, cockpit->camera.unknown_352, 0, 30);
  FollowCar(g, &camera);
  cockpit->camera.unknown_1088 = camera.points[12];
  cockpit->look_angle = 0.0f;
}

// CockpitCamera::Update: r3 the camera. Each tick the eye and target are
// the car model's cockpit points moved with the car, then the target is
// turned about the up axis by the look stick's angle.
void UpdateCockpit(Guest& g, CockpitCameraObject* cockpit) {
  GameObject& camera = cockpit->camera.object;
  FollowCar(g, &camera);
  Viewport* viewport = At<Viewport>(camera.viewport);
  SetCockpitLens(g, viewport);

  // If the car model isn't found (the game doesn't check), the start's
  // defaults stand in.
  CockpitPoint point;
  if (!FindCockpitPoint(g, &point)) {
    point.eye = viewport->eye_offset;
    point.target = viewport->target_offset;
  }
  const Mat4 rotation = CarRotation(camera);
  const Mat4 world = Multiply(rotation, Translation(CarPosition(camera)));
  Store(viewport->eye, Transform(Load(point.eye), world));
  Store(viewport->target, Transform(Load(point.target), world));
  viewport->alternate_target = viewport->target;
  Store(viewport->up, Transform(Load(viewport->up_offset), rotation));

  // Looking around: the direction to the target turned by the look angle.
  const Vec4 turned = Transform(Direction(viewport->eye, viewport->target),
                                RotationY(cockpit->look_angle));
  viewport->target.x = viewport->eye.x + turned.x;
  viewport->target.y = viewport->eye.y + turned.y;
  viewport->target.z = viewport->eye.z + turned.z;

  {
    // The route lookup at the car's position (sub_820C2068); its result
    // isn't used here. Kept as the game makes it.
    guest::StackFrame frame(g.ctx, 112);
    Vector* position = At<Vector>(frame.Local(80));
    *position = camera.points[0];
    g.Call(sub_820C2068, position, 1);
  }

  // The reflection cube is built from 1 m in front of the eye.
  const Vec4 ahead = Direction(viewport->eye, viewport->target);
  viewport->reflection_center.x = viewport->eye.x + ahead.x;
  viewport->reflection_center.y = viewport->eye.y + ahead.y;
  viewport->reflection_center.z = viewport->eye.z + ahead.z;
  viewport->reflection_center.w = 1.0f;
}

// --- The race camera ---

// RaceCamera::ResetRolls (sub_821509F8): both viewports level.
void ResetRolls(Guest& g) {
  Viewports* viewports = GetViewports();
  SetRoll(viewports->players[0], 0.0f);
  SetRoll(viewports->players[1], 0.0f);
}

// A camera mode started (each mode's Start snaps its camera to the car).
// Returns the camera, as each start does.
uint32_t StartCamera(Guest& g, RaceModeMember* member, uint32_t mode, Viewport* viewport,
                     uint32_t car, int player) {
  switch (mode) {
    case kChase:
      return g.Call(sub_82152620, &member->chase_camera, viewport, car);  // ChaseCamera::Start
    case kCockpit:
      return g.Call(sub_82152A28, &member->cockpit_camera, viewport, car);  // CockpitCamera::Start
    case kHood:
      return g.Call(sub_82152D30, &member->hood_cameras[player], viewport, car);  // HoodCamera::Start
    case kMode3:
      return g.Call(sub_82156F08, &member->unknown_camera, viewport, car);
    case kTv:
      return g.Call(sub_82153280, &member->tv_cameras, viewport, car);  // TvCameras::Start
    case kScripted:
      return g.Call(sub_82154C58, &member->scripted_camera, viewport, car);  // ScriptedCamera::Start
    default:
      return 0;
  }
}

// RaceCamera::SetMode: r3 the race mode's member, r4 the mode. Both
// viewports take the mode, and the mode's camera starts (in split screen,
// mode 2 starts one for each player).
void SetMode(Guest& g, RaceModeMember* member, uint32_t mode) {
  uint32_t cars[2];
  cars[0] = g.Call(sub_820C94B8, 0);  // Racers::Vehicle
  cars[1] = g.Call(sub_820C94B8, 1);
  Viewports* viewports = GetViewports();
  Viewport* player1 = viewports->players[0];
  Viewport* player2 = viewports->players[1];
  member->camera_mode = mode;
  player1->mode = mode;
  player2->mode = member->camera_mode;
  g.Call(sub_82290970, &member->unknown_68528[0], cars[0]);
  ResetRolls(g);
  member->active_cameras[0] = nullptr;
  member->active_cameras[1] = nullptr;

  const uint32_t followed = cars[member->followed_racer & 1];
  uint32_t last = 0;
  if (mode == kHood && viewports->split_screen != 0) {
    last = StartCamera(g, member, kHood, player1, cars[0], 0);
    member->active_cameras[0].address = last;
    last = StartCamera(g, member, kHood, player2, cars[1], 1);
    member->active_cameras[1].address = last;
  } else if (mode <= kScripted) {
    last = StartCamera(g, member, mode, player1, followed, 0);
    member->active_cameras[0].address = last;
  }
  g.ctx.r3.u64 = last;
}

// The next view on the view button: chase, cockpit, hood; with the TV
// cameras in the views (replays, likely) chase, TV, cockpit, hood. Split
// screen has no view button. Modes 3 and 5 aren't stepped.
void StepView(Viewport* viewport, bool tv_in_views, bool split_screen) {
  const uint32_t mode = viewport->mode;
  if (tv_in_views) {
    switch (mode) {
      case kChase: viewport->mode = kTv; break;
      case kTv: viewport->mode = kCockpit; break;
      case kCockpit: viewport->mode = kHood; break;
      case kHood: viewport->mode = kChase; break;
      default: break;
    }
    return;
  }
  if (split_screen) {
    return;
  }
  switch (mode) {
    case kChase: viewport->mode = kCockpit; break;
    case kCockpit: viewport->mode = kHood; break;
    case kHood: viewport->mode = kChase; break;
    default: break;
  }
}

// Queues a message on the dispatcher (id, then two 0 arguments).
void PostMessage(MessageDispatcher* dispatcher, uint32_t id) {
  dispatcher->queued += 1;
  const uint32_t slot = (dispatcher->read + dispatcher->queued - 1) % MessageDispatcher::kQueue;
  game::Message& message = dispatcher->queue[slot];
  message.id = id;
  message.arguments[0] = 0;
  message.arguments[1] = 0;
}

// The step for camera_flags 0x20/0x40 (in play with unknown_827892 = 2):
// what it is in game isn't traced. State 1 reports the followed car's
// speed against one of the blocks; 2 and 4 point the TV cameras' other
// kind at a block or none, and post message 58.
void Step827892(Guest& g, RaceModeMember* member, MessageDispatcher* dispatcher) {
  g.Call(sub_82290E38, &member->unknown_68528[0]);
  const int32_t block = member->unknown_77360;
  bool post = false;
  switch (member->unknown_77364) {
    case 1: {
      g.Call(sub_820BFF60, member->unknown_77372);  // Vehicle::GetSpeedKmh
      const float speed = float(g.ctx.f1.f64);
      g.ctx.f1.f64 = double(speed - member->unknown_68528[block].unknown_1064);
      game::RaceProgress* progress = At<game::RaceProgress>(*At<u32_be>(game::kRaceProgress));
      g.Call(sub_82101AE0, &progress->entry.parts[0], int32_t(int16_t(block)));
      break;
    }
    case 2:
      member->tv_cameras.unknown_448 = AddressOf(&member->unknown_68528[block]);
      post = true;
      break;
    case 4:
      member->tv_cameras.unknown_448 = 0;
      post = true;
      break;
    default:
      break;
  }
  if (post) {
    PostMessage(dispatcher, kMessage58);
  }
  if (member->unknown_77364 != 0) {
    g.Call(sub_820A00B0, 0, 667, 0x40FF0000u);
  }
}

// RaceCamera::Update: r3 the race mode's member, r4 the message dispatcher.
// Once a tick in a race: the view button, the chase camera's reset, the
// cockpit's look stick, then the active camera's update and the viewports'.
void Update(Guest& g, RaceModeMember* member, MessageDispatcher* dispatcher) {
  const uint32_t controller = member->followed_controller & 0xFF;
  const ControlRecord pad = *At<ControlRecord>(At<u32_be>(game::kControlRecords)[controller]);
  Viewports* viewports = GetViewports();
  Viewport* player1 = viewports->players[0];
  RaceData* race = GetRaceData();
  const bool tv_in_views = (race->camera_flags & kTvInViews) != 0;

  if ((pad.buttons & kViewButton) != 0) {
    StepView(player1, tv_in_views, viewports->split_screen != 0);
  }
  // With the TV cameras in the views, they keep following the camera's
  // target while another view is in use.
  if (tv_in_views && player1->mode != kTv && member->active_cameras[0] != nullptr) {
    g.Call(sub_82154918, &member->tv_cameras, &member->active_cameras[0]->unknown_260);
  }
  if (player1->mode != member->camera_mode) {
    g.Call(sub_82157188, member, uint32_t(player1->mode));  // RaceCamera::SetMode
  }
  if ((pad.buttons & kChaseResetButton) != 0 && member->camera_mode == kChase) {
    member->chase_camera.reset_behind = 1;
  }
  if (member->camera_mode == kCockpit) {
    member->cockpit_camera.look_angle = float(int16_t(pad.look)) * kLookStickScale * kLookRange;
  }
  if (race->unknown_827892 == 2 &&
      (race->camera_flags & (kCameraFlag20 | kCameraFlag40)) != 0) {
    Step827892(g, member, dispatcher);
  }

  if (member->active_cameras[0] != nullptr) {
    g.CallVirtual(member->active_cameras[0].get(), 0);  // the camera's Update
  }
  if (viewports->split_screen != 0 && member->active_cameras[1] != nullptr) {
    g.CallVirtual(member->active_cameras[1].get(), 0);
  }
  g.Call(sub_820AADF8, viewports->players[0]);  // Viewport::Update
  if (viewports->split_screen != 0) {
    g.Call(sub_820AADF8, viewports->players[1]);
  }
}

// Which racer the camera follows with play mode 6 (one of two players).
// Returns true if this decides it.
bool FollowsSecondRacer(Guest& g, RaceData* race, bool* second) {
  if (race->play_mode != kPlayMode6) {
    return false;
  }
  guest::StackFrame frame(g.ctx, 224);  // The result's size isn't known; the original's frame.
  const uint32_t found = g.Call(sub_820FFED8, frame.Local(80), &race->unknown_444832, -1);
  if (*At<u32_be>(found) == 1) {
    return false;
  }
  *second = At<u32_be>(kSecondCarModel)[1] == 0;
  return true;
}

// RaceCamera::Start: r3 the race mode's member. The view from the options
// (0 hood, 1 cockpit, 2 chase), hood for both in split screen.
void Start(Guest& g, RaceModeMember* member) {
  Viewports* viewports = GetViewports();
  Viewport* player1 = viewports->players[0];
  Viewport* player2 = viewports->players[1];
  const uint32_t save = g.Call(sub_82105790, 0);  // GameData::Get: the save
  const uint32_t option = At<game::SaveData>(save)->options.camera_view;
  RaceData* race = GetRaceData();
  if (race->play_mode == kPlaySplitScreen) {
    viewports->split_screen = 1;
  }
  bool second = false;
  if (FollowsSecondRacer(g, race, &second)) {
    member->followed_racer = second ? 1 : 0;
    member->followed_controller = second ? 1 : 0;
  }
  if (viewports->split_screen == 0) {
    switch (option) {
      case 0: member->start_view = kHood; break;
      case 1: member->start_view = kCockpit; break;
      default: member->start_view = kChase; break;
    }
    player1->mode = member->start_view;
  } else {
    member->start_view = kHood;
    player1->mode = kHood;
    player2->mode = member->start_view;
  }
  g.Call(sub_82157188, member, uint32_t(member->start_view));  // RaceCamera::SetMode
  member->saved_view = member->start_view;
}

// RaceCamera::Restore: r3 the race mode's member. The player's saved view
// back (taken by the next Update), or hood for both in split screen.
void Restore(Guest& g, RaceModeMember* member) {
  Viewports* viewports = GetViewports();
  Viewport* player1 = viewports->players[0];
  Viewport* player2 = viewports->players[1];
  RaceData* race = GetRaceData();
  bool second = false;
  if (!FollowsSecondRacer(g, race, &second)) {
    second = false;
  }
  member->followed_racer = second ? 1 : 0;
  member->followed_controller = second ? 1 : 0;
  if (race->play_mode == kPlaySplitScreen) {
    viewports->split_screen = 1;
    player1->mode = kHood;
    player2->mode = kHood;
    g.Call(sub_82157188, member, uint32_t(kHood));  // RaceCamera::SetMode
    return;
  }
  player1->mode = member->saved_view;
}

}  // namespace

#define SBX_RACE_CAMERA_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                 \
  if (!On()) {                         \
    __imp__##name(ctx, base);          \
    return;                            \
  }                                    \
  Guest g(ctx, base);

// RaceCamera::SetMode (sub_82157188), from subtick/camera_cuts.cpp's override.
void hooks::NativeRaceCamera_SetMode(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_82157188(ctx, base);
    return;
  }
  Guest g(ctx, base);
  SetMode(g, At<RaceModeMember>(ctx.r3.u32), ctx.r4.u32);
}

// RaceCamera::Update (sub_821573E0), from subtick/per_frame_logic.cpp's override.
void hooks::NativeRaceCamera_Update(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_821573E0(ctx, base);
    return;
  }
  Guest g(ctx, base);
  Update(g, At<RaceModeMember>(ctx.r3.u32), At<MessageDispatcher>(ctx.r4.u32));
}

// RaceCamera::Start: r3 the race mode's member.
extern "C" REX_FUNC(sub_82157728) {
  SBX_RACE_CAMERA_FALLBACK(sub_82157728)
  Start(g, At<RaceModeMember>(ctx.r3.u32));
}

// RaceCamera::Restore: r3 the race mode's member.
extern "C" REX_FUNC(sub_82157868) {
  SBX_RACE_CAMERA_FALLBACK(sub_82157868)
  Restore(g, At<RaceModeMember>(ctx.r3.u32));
}

// RaceCamera::ResetRolls.
extern "C" REX_FUNC(sub_821509F8) {
  SBX_RACE_CAMERA_FALLBACK(sub_821509F8)
  ResetRolls(g);
}

// CockpitCamera::Start: r3 the camera, r4 the viewport, r5 the car.
// Returns the camera.
extern "C" REX_FUNC(sub_82152A28) {
  SBX_RACE_CAMERA_FALLBACK(sub_82152A28)
  CockpitCameraObject* cockpit = At<CockpitCameraObject>(ctx.r3.u32);
  StartCockpit(g, cockpit, At<Viewport>(ctx.r4.u32), ctx.r5.u32);
  ctx.r3.u64 = AddressOf(cockpit);
}

// CockpitCamera::Update (the cockpit camera's vtable slot 0): r3 the camera.
extern "C" REX_FUNC(sub_82151B18) {
  SBX_RACE_CAMERA_FALLBACK(sub_82151B18)
  UpdateCockpit(g, At<CockpitCameraObject>(ctx.r3.u32));
}

// Viewport::SetRoll: r3 the viewport, f1 the roll (radians).
extern "C" REX_FUNC(sub_820AA288) {
  SBX_RACE_CAMERA_FALLBACK(sub_820AA288)
  SetRoll(At<Viewport>(ctx.r3.u32), float(ctx.f1.f64));
}

// Viewport::SetNearClip: f1 the value (pass 0's +384).
extern "C" REX_FUNC(sub_820AA320) {
  SBX_RACE_CAMERA_FALLBACK(sub_820AA320)
  SetNearClip(float(ctx.f1.f64));
}

// CameraPass::SetFromViewport: r3 the camera pass, r4 the player, r5 the
// view. The view's eye, target, up, field of view and roll copied in and
// the pass's matrices built (CameraPass::Update).
extern "C" REX_FUNC(sub_820AA460) {
  SBX_RACE_CAMERA_FALLBACK(sub_820AA460)
  CameraPass* pass = At<CameraPass>(ctx.r3.u32);
  const game::ViewportView& view = GetViewports()->players[ctx.r4.u32]->views[ctx.r5.u32];
  pass->eye = view.eye;
  pass->target = view.target;
  pass->up = view.up;
  g.ctx.f1.f64 = double(float(view.fov));
  g.Call(sub_820AA330, pass);  // CameraPass::SetFov
  pass->unknown_388 = view.unknown_72;
  pass->roll = view.roll;
  g.Call(sub_820A3B98, pass);  // CameraPass::Update
}
