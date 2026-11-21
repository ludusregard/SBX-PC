// The scenery's lists as our own C++ (--native_scenery_lists, on by
// default): a course set's model lists (their load, point buffers and
// release), which of the set lists' entries are drawn for the race (and
// the course layout that decides it), a scenery model's draw state reset,
// a texture animation's current frame, the texture list's store, a pass's
// billboard set-up, the depth pass's vertex declaration and the scene
// lights' constants and directions; the draws: a model shape's visibility
// test, its draw (SceneryModel::DrawShape, Model::DrawShape) and record
// (SceneryModel::RecordShape), its depth draw and depth record, and the
// point list's draw.
//
// The functions course_scenery.cpp calls are declared in course_scenery.h.
// Recorder::RecordObject's C++ is here for subtick's wrapper to call; the
// draws call it through its address, so the wrapper runs.

#include "base/settings.h"
#include "game/course_scenery.h"
#include "game/data_file.h"
#include "game/math.h"
#include "game/model.h"
#include "game/race_camera.h"
#include "game/race_setup.h"
#include "game/shader_objects.h"
#include "hooks/native_calls.h"

SBX_SETTING_BOOL(native_scenery_lists, true, "Overrides", "Scene",
                 "Our own C++ for the scenery lists (the model lists' loads and releases, "
                 "which set list entries are drawn, the course layout, resets and small "
                 "setters, the shapes' draws, depth draws and depth records, the point "
                 "list's draw, the scenery shape recorder, the lights' constants) instead "
                 "of the recompiled functions");

namespace {

using game::Animation;
using game::AnimationSet;
using game::Material;
using game::MaterialCount;
using game::Mesh;
using game::Model;
using game::ModelList;
using game::PointList;
using game::RaceData;
using game::SceneryDrawPass;
using game::SceneryModel;
using game::Vector;
using game::VertexWeights;
using game::XPRList;
using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::f32_be;
using guest::u16_be;
using guest::u32_be;

// The point lists' 20 point buffers, 256 bytes each in one static block.
constexpr uint32_t kPointBuffers = 0x826076B0;
struct PointBuffer {
  uint8_t unknown_0[256];  // Four groups of four 16-byte points.
};
GUEST_SIZE(PointBuffer, 256);

// A texture animation's frame: two words, handed to the texture provider.
struct AnimationFrame {
  u32_be unknown_0;
  u32_be texture;
};
GUEST_SIZE(AnimationFrame, 8);

// The race data's first words, which RaceData (race_setup.h) keeps
// unnamed.
struct RaceDataHead {
  uint8_t unknown_0[4];
  i32_be course;
  uint8_t unknown_8[12];
  u32_be course_flags;    // Read by Race::CourseLayout in mode 2.
  u32_be set_list_flags;  // Which of the course set's +2124 list entries are on.
};
GUEST_OFFSET(RaceDataHead, course, 4);
GUEST_OFFSET(RaceDataHead, set_list_flags, 24);

constexpr uint32_t kRaceData = 0x826B580C;  // Holds the race data's address.

RaceData* TheRace() { return At<RaceData>(*At<u32_be>(kRaceData)); }
const RaceDataHead* HeadOf(const RaceData* race) { return reinterpret_cast<const RaceDataHead*>(race); }

// RaceData::camera_flags bits read here.
constexpr uint32_t kTvCameras = 0x80;           // No set list entries drawn.
constexpr uint32_t kSetListExtras = 0x20 | 0x40;  // The +2652 list's entries 16 to 23 allowed.

// The depth pass's vertex declarations (written at run time).
constexpr uint32_t kDepthDeclarations = 0x8296DD18;
struct DepthDeclarations {
  uint8_t unknown_0[20];
  u32_be other;
  u32_be stream_11;
};
GUEST_OFFSET(DepthDeclarations, stream_11, 24);
constexpr uint32_t kDepthStreamType = 11;

// VertexDeclarations::ForDepth: the depth pass's declaration for a
// mesh's second stream type.
uint32_t DepthDeclaration(uint32_t stream_type) {
  const DepthDeclarations* declarations = At<DepthDeclarations>(kDepthDeclarations);
  return stream_type == kDepthStreamType ? declarations->stream_11 : declarations->other;
}

// The render state the scene lights' projector record copies: the eye
// (the one the reflections are worked out against), then the projectors'
// rows and matrices. All in the game's writable data, set every frame.
constexpr uint32_t kEyePosition = 0x8264EA60;     // x, y, z.
constexpr uint32_t kProjectorRows = 0x8296DDE0;   // 6 vectors.
constexpr uint32_t kProjectorMatrices = 0x8296DF40;  // 16 vectors.

struct ProjectorRecord {
  Vector eye;  // w 1.
  Vector projector_rows[6];
  Vector projector_matrices[16];
};
GUEST_SIZE(ProjectorRecord, 368);

// The command list the scene's draws are recorded on (written at run
// time), and a command's first slot: its id, its payload's slots and an
// argument.
constexpr uint32_t kCommandList = 0x8296DD78;  // Holds the list's address.
struct CommandHeader {
  u32_be command;
  u32_be count;
  u32_be argument;
  uint8_t unknown_12[4];
};
GUEST_SIZE(CommandHeader, 16);

// A render state's slot (an id and its value) and a draw's (two words
// with two halfwords between).
struct StateWords {
  u32_be state;
  u32_be value;
  uint8_t unknown_8[8];
};
GUEST_SIZE(StateWords, 16);
struct DrawWords {
  u32_be word_0;
  u16_be unknown_4;
  u16_be unknown_6;
  u32_be word_8;
  uint8_t unknown_12[4];
};
GUEST_SIZE(DrawWords, 16);
constexpr uint32_t kSetRenderState = 1;

// Commands recorded here.
constexpr uint32_t kSetPixelConstants = 7;  // Its argument: the first register and the count.
constexpr uint32_t kEndCommands = 36;
constexpr uint32_t kSceneLightRegisters = 0x0101003F;  // Pixel constants 0 to 62.

// The scene's view and eye, written each frame.
constexpr uint32_t kSceneView = 0x8264EA00;  // A Matrix.
constexpr uint32_t kSceneEye = 0x8264EA40;   // A Vector.

// The scene lights' directions and colors (written every frame).
constexpr uint32_t kSunDirections = 0x8296DDB0;        // 3 vectors, normalized.
constexpr uint32_t kLightColors = 0x8296DE40;          // 8 vectors.
constexpr uint32_t kLightColors2 = 0x8296DEC0;         // 8 vectors.
constexpr uint32_t kProjectorDirections = 0x8296E040;  // 8 vectors, normalized.
constexpr uint32_t kProjectorColors = 0x8296E0C0;      // 8 vectors.

struct DirectionConstants {
  Vector sun_directions[3];
  Vector light_colors[8];
  Vector light_colors_2[8];
  Vector projector_directions[8];
  Vector projector_colors[8];
};
GUEST_SIZE(DirectionConstants, 35 * 16);

void RecordDirections(Guest& g, DirectionConstants* out);

// The scene lights' pixel constants (c0 to c62), as recorded for each
// object.
struct SceneLightConstants {
  game::Matrix view;            // c0-c3.
  Vector eye;                   // c4.
  DirectionConstants directions;  // c5-c39 (SceneLights::RecordDirections).
  ProjectorRecord projectors;   // c40-c62.
};
GUEST_SIZE(SceneLightConstants, 63 * 16);

bool On() { return SBX_GET(native_scenery_lists); }

// --- The model lists ---

// --- Which set list entries are drawn ---

// Race::CourseLayout: which of the course set's layouts the race's course
// uses (-1 for none): by the course in modes 8, 3, 4 and 7 (or with the
// current stage's type 1), by the course flags in mode 2.
int32_t CourseLayout(Guest& g) {
  RaceData* race = TheRace();
  if (race == nullptr) {
    return -1;
  }
  const RaceDataHead* head = HeadOf(race);
  const int32_t course = head->course;
  const uint32_t flags = head->course_flags;
  const int32_t mode = race->unknown_827892;
  const int32_t stage_type = int32_t(g.Call(sub_820FFD38, race->unknown_444832, -1));  // Stages::Type

  if (mode == 8) {
    static constexpr int32_t kLayouts[8] = {16, 17, 18, 19, 23, 23, 22, 23};
    return uint32_t(course) < 8 ? kLayouts[course] : -1;
  }
  if (mode == 3 || mode == 4 || mode == 7 || stage_type == 1) {
    static constexpr int32_t kLayouts[23] = {16, 17, 18, 19, 20, 21, 22, 23, -1, -1, -1, 4,
                                             5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15};
    return uint32_t(course) < 23 ? kLayouts[course] : -1;
  }
  if (mode == 2) {
    if (flags & 0x20) {
      return 2;
    }
    if ((flags & 0x4) && (flags & 0x8)) {
      return 3;
    }
    return (flags & 0x10) ? 1 : 0;
  }
  return -1;
}

// --- The scene lights ---

// SceneLights::RecordProjectors: the eye and the projectors copied in.
void RecordProjectors(ProjectorRecord* record) {
  const Vector* eye = At<Vector>(kEyePosition);
  record->eye = Vector{eye->x, eye->y, eye->z, 1.0f};
  const Vector* rows = At<Vector>(kProjectorRows);
  for (int i = 0; i < 6; i++) {
    record->projector_rows[i] = rows[i];
  }
  const Vector* matrices = At<Vector>(kProjectorMatrices);
  for (int i = 0; i < 16; i++) {
    record->projector_matrices[i] = matrices[i];
  }
}

// Recorder::RecordSceneLights: the scene lights' pixel constants recorded
// for an object, in a chunk of their own on the command list. Returns the
// command's first slot.
uint32_t RecordSceneLights(Guest& g) {
  auto list = [] { return uint32_t(*At<u32_be>(kCommandList)); };
  g.Call(sub_82572568, list());  // CommandList::OpenChunk
  const uint32_t header_slot = g.Call(sub_82572638, list(), 1u);  // CommandList::AllocateSlots
  CommandHeader* header = At<CommandHeader>(header_slot);
  header->command = kSetPixelConstants;
  header->count = 63;
  header->argument = kSceneLightRegisters;
  SceneLightConstants* constants =
      At<SceneLightConstants>(g.Call(sub_82572638, list(), 63u));  // CommandList::AllocateSlots
  g.Call(sub_820A0E18, &constants->view, kSceneView);  // Matrix::Copy
  constants->eye = *At<Vector>(kSceneEye);
  RecordDirections(g, &constants->directions);
  RecordProjectors(&constants->projectors);
  CommandHeader* end = At<CommandHeader>(g.Call(sub_82572638, list(), 1u));  // CommandList::AllocateSlots
  end->count = 0;
  end->command = kEndCommands;
  end->argument = 0;
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk
  return header_slot;
}

// --- Model::IsShapeVisible ---

// A corner is past the side of the view when its x or y (divided by w) is
// beyond this.
constexpr float kViewEdge = 1.01f;  // 0x82002B70, and -1.01 at 0x82069BA4.

// The frame Model::IsShapeVisible works in: the product of the world and
// view-projection matrices at +96.
constexpr uint32_t kVisibilityFrame = 192;
constexpr uint32_t kVisibilityProduct = 96;

// A four-float dot product as the recompiled vector unit's vmsum4fp128
// makes it (the same dp_ps, so the sum's order is the same).
float Dot4(const float a[4], const float b[4]) {
  float out[4];
  simde_mm_storeu_ps(out, simde_mm_dp_ps(simde_mm_loadu_ps(a), simde_mm_loadu_ps(b), 0xFF));
  return out[0];
}

// Model::IsShapeVisible: whether any of shape `shape`'s bounding box is in
// view: the corners through world x view-projection (Matrix::Multiply into
// the frame), divided by w (a corner behind the eye flipped); false when
// all eight are past the same side, or all behind the eye or all in front
// of the near plane's other side. The product is made in a guest frame
// (Matrix::Multiply is recompiled).
bool IsShapeVisible(Guest& g, Model* model, game::Matrix* view_projection, game::Matrix* world,
                    uint32_t shape) {
  g.ctx.fpscr.disableFlushMode();
  guest::StackFrame frame(g.ctx, kVisibilityFrame);
  game::Matrix* product = At<game::Matrix>(frame.Local(kVisibilityProduct));
  product->right.w = 1.0f;
  product->up.w = 1.0f;
  product->forward.w = 1.0f;
  product->position.w = 1.0f;
  g.Call(sub_8209BE70, product, world, view_projection);  // Matrix::Multiply

  const Vector* rows[4] = {&product->right, &product->up, &product->forward, &product->position};
  float columns[4][4];  // x, y, z, w.
  for (int r = 0; r < 4; r++) {
    columns[0][r] = rows[r]->x;
    columns[1][r] = rows[r]->y;
    columns[2][r] = rows[r]->z;
    columns[3][r] = rows[r]->w;
  }

  int behind = 0, horizontal = 0, vertical = 0;
  for (const Vector& corner : model->MeshAt(shape)->corners) {
    const float point[4] = {corner.x, corner.y, corner.z, corner.w};
    // The vector unit flushes denormals.
    g.ctx.fpscr.enableFlushMode();
    const float clip_x = Dot4(point, columns[0]);
    const float clip_z = Dot4(point, columns[2]);
    const float clip_y = Dot4(point, columns[1]);
    const float clip_w = Dot4(point, columns[3]);
    g.ctx.fpscr.disableFlushModeUnconditional();

    const float inverse_w = 1.0f / clip_w;
    float x = clip_x * inverse_w;
    float y = clip_y * inverse_w;
    const float z = clip_z * inverse_w;
    if (clip_w < 0.0f) {
      x = x * -1.0f;
      y = y * -1.0f;
      behind++;
    } else if (z < 0.0f) {
      behind--;
    }
    if (x > kViewEdge) {
      horizontal++;
    } else if (x < -kViewEdge) {
      horizontal--;
    }
    if (y > kViewEdge) {
      vertical++;
    } else if (y < -kViewEdge) {
      vertical--;
    }
  }
  auto all = [](int count) { return count == 8 || count == -8; };
  return !(all(horizontal) || all(vertical) || all(behind));
}

// --- Model::DrawShapeDepth ---

// The render queue the draws are handed to (its vtable's slot 4 adds an
// entry) and the sort keys it's given (all written at run time).
constexpr uint32_t kRenderQueue = 0x8296DD7C;   // Holds the queue's address.
constexpr uint32_t kDepthShapeKey = 0x8296DD5C;  // Model::DrawShapeDepth's.

// Recorder::RecordObject's command: its header, then the object's vertex
// constants (c0-c3 the world x view-projection). The draws sort by the
// object's depth, its position's z there.
struct ObjectCommand {
  CommandHeader header;
  game::Matrix world_view_projection;
};
GUEST_OFFSET(ObjectCommand, world_view_projection, 16);

// The object's vertex constants (c0-c8), the payload of Recorder::RecordObject's
// command (its slots follow the header's).
struct ObjectConstants {
  game::Matrix world_view_projection;  // c0-c3.
  game::Matrix world;                  // c4-c7.
  Vector eye;                          // c8: the scene's eye.
};
GUEST_SIZE(ObjectConstants, 9 * 16);

constexpr uint32_t kVertexConstantsC0 = 0x00010009;  // Nine vertex constants from c0.

// Recorder::RecordObject: an object's vertex constants recorded in a chunk
// of their own: c0-c3 the world matrix times the view-projection
// (Matrix::Multiply), c4-c7 the world matrix (Matrix::Copy), c8 the
// scene's eye. Returns the command's first slot.
uint32_t RecordObject(Guest& g, uint32_t view_projection, uint32_t world) {
  g.ctx.fpscr.disableFlushMode();
  auto list = [] { return uint32_t(*At<u32_be>(kCommandList)); };
  g.Call(sub_82572568, list());  // CommandList::OpenChunk
  CommandHeader* header = At<CommandHeader>(g.Call(sub_82572638, list(), 1u));  // CommandList::AllocateSlots
  header->command = kSetPixelConstants;
  header->count = 9;
  header->argument = kVertexConstantsC0;
  ObjectConstants* constants =
      At<ObjectConstants>(g.Call(sub_82572638, list(), 9u));  // CommandList::AllocateSlots
  g.Call(sub_8209BE70, &constants->world_view_projection, world, view_projection);  // Matrix::Multiply
  g.Call(sub_820A0E18, &constants->world, world);  // Matrix::Copy
  constants->eye = *At<Vector>(kSceneEye);
  CommandHeader* end = At<CommandHeader>(g.Call(sub_82572638, list(), 1u));  // CommandList::AllocateSlots
  end->count = 0;
  end->command = kEndCommands;
  end->argument = 0;
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk
  return AddressOf(header);
}

// A render queue entry: two slots (16 bytes each), each a kind and the
// chunk it draws (3 the object's constants, 4 a shape's draw).
struct QueueSlot {
  uint8_t unknown_0[2];
  u16_be kind;
  u32_be chunk;
  uint8_t unknown_8[8];
};
GUEST_SIZE(QueueSlot, 16);
struct QueueEntry {
  QueueSlot constants;
  QueueSlot draw;
};
constexpr uint16_t kQueueConstants = 3;
constexpr uint16_t kQueueDraw = 4;

// --- PointList::Draw ---

// PointList::Draw's frame (512 bytes), at the original's offsets: the
// callees get the world matrix (+80), the range (+144 or +160), the
// translation (+192), the rotation (+224) and the view-projection (+288).
// The two vectors at +144 are the point's position and the range, for
// groups 0-2 in that order, for group 3 the other way round.
union PointOrRange {
  Vector position;
  game::DrawRange range;
};
struct PointDrawFrame {
  uint8_t linkage[80];
  game::Matrix world;
  PointOrRange vectors_144[2];
  uint8_t unknown_176[16];
  Vector translation;
  uint8_t unknown_208[16];
  game::Matrix rotation;
  game::Matrix view_projection;
  uint8_t unknown_352[160];
};
GUEST_SIZE(PointDrawFrame, 512);
GUEST_OFFSET(PointDrawFrame, translation, 192);
GUEST_OFFSET(PointDrawFrame, view_projection, 288);

constexpr float kHiddenPointY = -1000.0f;          // 0x820067E0: points below aren't drawn.
constexpr float kDegreesToRadians = 0.0174532924f;  // 0x82002920.
constexpr float kQuarterTurnBack = -1.57079637f;    // 0x82002A2C.
constexpr float kPointNear = 10000.0f;              // 0x820027CC.
constexpr float kPointFar = 50000.0f;               // 0x820067DC.
constexpr int32_t kPointGroupBytes = 64;            // Four 16-byte points.
constexpr int32_t kPointBytes = 16;
constexpr int32_t kLastModelGroup = 3;              // Model 1's group.

const game::Matrix kIdentityMatrix{{1.0f, 0.0f, 0.0f, 0.0f},
                                   {0.0f, 1.0f, 0.0f, 0.0f},
                                   {0.0f, 0.0f, 1.0f, 0.0f},
                                   {0.0f, 0.0f, 0.0f, 1.0f}};

// A point list model's draw state: as SceneryModel::ResetDrawState, then
// the list's texture provider and animation set.
SceneryModel* PointModel(PointList* list, uint32_t position) {
  SceneryModel* model = At<SceneryModel>(*list->list.models.ItemAtPosition(position));
  game::SceneryModelResetDrawState(model);
  model->model.texture_provider.address = list->texture_provider;
  model->animations = list->animations;
  return model;
}

// One point's draw: its position (w 1), and when it's not hidden the world
// matrix turned about y by the point's w (degrees) and a quarter turn back,
// moved to it, and the model's shape 0 drawn (SceneryModel::DrawShape, no
// depth pass, the range 10,000 to 50,000).
void DrawAtPoint(Guest& g, PointDrawFrame* frame, SceneryModel* model, const Vector* point,
                 Vector* position, game::DrawRange* range) {
  position->x = point->x;
  position->y = point->y;
  position->z = point->z;
  if (point->y < kHiddenPointY) {
    return;
  }
  frame->translation = *position;
  const float angle = point->w * kDegreesToRadians;
  frame->world = kIdentityMatrix;
  g.ctx.f1.f64 = angle;
  g.Call(sub_820A68E8, &frame->rotation);                          // Matrix::RotateY
  g.Call(sub_8209BE70, &frame->world, &frame->world, &frame->rotation);  // Matrix::Multiply
  g.ctx.f1.f64 = kQuarterTurnBack;
  g.Call(sub_820A68E8, &frame->rotation);                          // Matrix::RotateY
  g.Call(sub_8209BE70, &frame->world, &frame->world, &frame->rotation);  // Matrix::Multiply
  g.Call(sub_820B0C50, &frame->world, &frame->world,
         &frame->translation);  // Matrix::MultiplyTranslation
  range->near = kPointNear;
  range->far = kPointFar;
  range->unknown_8 = 0.0f;
  range->unknown_12 = 0.0f;
  game::SceneryModelDrawShape(g, model, &frame->view_projection, &frame->world, 0, 0, nullptr,
                              range, 0);
}

// --- Model::RecordShapeDepth ---

// The depth pass's pixel constant c43 (written at run time).
constexpr uint32_t kDepthPixelConstant = 0x8264EA70;
constexpr uint32_t kPixelConstantC43 = 0x01012B01;  // One pixel constant, c43.

// The depth record's commands and render states.
constexpr uint32_t kSetVertexDeclaration = 10;
constexpr uint32_t kSetStreamSource = 11;
constexpr uint32_t kSetIndices = 22;
constexpr uint32_t kSetMaterialIndices = 12;
constexpr uint32_t kDrawIndexed = 18;
constexpr uint32_t kRenderState100 = 100;
constexpr uint32_t kRenderStateCull = 56;

// Records on the scene's command list a slot at a time
// (CommandList::AllocateSlots), the list read from [kCommandList] for each.
template <typename T>
T* SceneSlot(Guest& g) {
  return At<T>(g.Call(sub_82572638, uint32_t(*At<u32_be>(kCommandList)), 1u));  // CommandList::AllocateSlots
}
CommandHeader* SceneCommand(Guest& g, uint32_t id, uint32_t count) {
  CommandHeader* command = SceneSlot<CommandHeader>(g);
  command->command = id;
  command->count = count;
  command->argument = 0;
  return command;
}

// A stream's slot (its index, buffer, offset and stride) and the indices'.
struct StreamWords {
  u32_be stream;
  u32_be buffer;
  u32_be offset;
  u32_be stride;
};
GUEST_SIZE(StreamWords, 16);
struct IndicesWords {
  uint8_t unknown_0[4];
  u32_be buffer;
  u32_be unknown_8;  // 1.
  uint8_t unknown_12[4];
};
GUEST_SIZE(IndicesWords, 16);

// Model::RecordShapeDepth: shape `shape`'s group `group` recorded without
// shaders or textures (probably the shadows' depth): its depth vertex
// declaration, its stream (the skinned stream for a skinned mesh), pixel
// c43, a skinned mesh's indices, then each material that passes the mask
// (`mask` 0 for all; materials with alpha only with `with_alpha`): render
// states 100 and cull, its indices and its draw. Returns the chunk's first
// slot, or 0 when the group has no materials.
uint32_t RecordShapeDepth(Guest& g, Model* model, uint32_t shape, uint32_t group, uint32_t mask,
                          uint32_t with_alpha) {
  Mesh* mesh = model->MeshAt(shape);
  game::MaterialList* materials = mesh->materials;
  if (materials->group_counts[group != 0 ? 1 : 0] == 0) {
    return 0;
  }
  const uint32_t stream =
      mesh->skinned == 1 ? uint32_t(mesh->skin_stream_1) : AddressOf(&mesh->streams[1]);
  auto list = [] { return uint32_t(*At<u32_be>(kCommandList)); };
  g.Call(sub_82572568, list());  // CommandList::OpenChunk

  CommandHeader* first = SceneCommand(g, kSetVertexDeclaration, 1);
  u32_be* declaration = SceneSlot<u32_be>(g);
  *declaration = DepthDeclaration(mesh->stream_type_1);

  SceneCommand(g, kSetStreamSource, 1);
  StreamWords* source = SceneSlot<StreamWords>(g);
  source->stream = 1;
  source->buffer = stream;
  source->offset = 0;
  source->stride = mesh->stream_stride_1;

  CommandHeader* constant = SceneSlot<CommandHeader>(g);
  constant->command = kSetPixelConstants;
  constant->count = 1;
  constant->argument = kPixelConstantC43;
  *SceneSlot<Vector>(g) = *At<Vector>(kDepthPixelConstant);

  if (mesh->skinned == 1) {
    const VertexWeights* weights = mesh->weights;
    if (weights->index_buffer != 0) {
      SceneCommand(g, kSetIndices, 1);
      IndicesWords* indices = SceneSlot<IndicesWords>(g);
      indices->unknown_8 = 1;
      indices->buffer = weights->index_buffer;
    }
  }

  // The group's materials: group 1's after group 0's.
  materials = model->MeshAt(shape)->materials;
  const uint32_t first_material =
      AddressOf(materials->materials) +
      (group != 0 ? uint32_t(materials->group_counts[0]) * uint32_t(sizeof(Material)) : 0);
  uint32_t index = 0;
  while (int32_t(index) < int32_t(MaterialCount(model, shape, int32_t(group)))) {
    const Material* material = guest::Element<Material>(first_material, index);
    index++;
    if (mask != 0 && (material->pass_mask & mask) == 0) {
      continue;
    }
    if (material->has_alpha != 0 && uint8_t(with_alpha) == 0) {
      continue;
    }
    SceneCommand(g, kSetRenderState, 2);
    StateWords* state = SceneSlot<StateWords>(g);
    state->state = kRenderState100;
    state->value = material->render_state_100;
    state = SceneSlot<StateWords>(g);
    state->state = kRenderStateCull;
    state->value = material->cull_mode;
    SceneCommand(g, kSetMaterialIndices, 1);
    *SceneSlot<u32_be>(g) = AddressOf(&material->index_buffer);
    SceneCommand(g, kDrawIndexed, 1);
    DrawWords* draw = SceneSlot<DrawWords>(g);
    draw->word_0 = material->draw_word_0;
    draw->unknown_4 = 0;
    draw->unknown_6 = 0;
    draw->word_8 = material->draw_word_8;
  }

  CommandHeader* end = SceneSlot<CommandHeader>(g);
  end->count = 0;
  end->argument = 0;
  end->command = kEndCommands;
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk
  return AddressOf(first);
}

// --- SceneryModel::DrawShape and Model::DrawShape ---

// The sort keys of the draws (written at run time).
constexpr uint32_t kGroup0Key = 0x8296DD6C;     // Opaque materials, nearest first.
constexpr uint32_t kGroup1Key = 0x8296DD60;     // The rest, farthest first.
constexpr uint32_t kShapeDepthKey = 0x8296DD68;  // The shapes' depth pass.

// A render queue entry of three slots: the object's constants, the scene
// lights' and the shape's draw.
struct LitQueueEntry {
  QueueSlot constants;
  QueueSlot lights;
  QueueSlot draw;
};
GUEST_SIZE(LitQueueEntry, 48);

// Which of the two draws: a scenery model's (SceneryModel::RecordShape,
// its groups counted by Model::MaterialCount) or a plain model's
// (Model::RecordShape, its groups read from the mesh).
enum class ShapeRecorder { kScenery, kModel };

struct ShapeDraw {
  ShapeRecorder recorder;
  uint32_t record_object_return;  // Where RecordObject's wrapper sees the call from.
  uint32_t pixel_constant;        // The recorders' optional pixel c76 (a Vector), or 0.
  uint32_t vertex_range;          // The optional range for vertex c86 (a DrawRange), or 0.
  uint32_t shared_pair_offset;    // (Scenery only) added to the shared shader pair.
};

uint32_t SceneryRecordShape(Guest& g, SceneryModel* scenery, uint32_t shape, uint32_t group,
                            uint32_t set_shaders, uint32_t pixel_constant, uint32_t vertex_range,
                            uint32_t pair_offset);

bool GroupHasMaterials(const ShapeDraw& draw, Model* model, uint32_t shape, uint32_t group) {
  if (draw.recorder == ShapeRecorder::kScenery) {
    return int32_t(MaterialCount(model, shape, int32_t(group))) != 0;
  }
  return model->MeshAt(shape)->materials->group_counts[group] != 0;
}

uint32_t RecordShape(Guest& g, const ShapeDraw& draw, Model* model, uint32_t shape,
                     uint32_t group) {
  if (draw.recorder == ShapeRecorder::kScenery) {
    return SceneryRecordShape(g, At<SceneryModel>(AddressOf(model)), shape, group, 1,
                              draw.pixel_constant, draw.vertex_range, draw.shared_pair_offset);
  }
  return game::ModelRecordShape(g, model, shape, int32_t(group), 1, draw.pixel_constant,
                                draw.vertex_range);
}

void QueueAdd(Guest& g, uint32_t entry, uint32_t key, float depth) {
  g.ctx.f1.f64 = depth;
  g.CallVirtual(At<u32_be>(*At<u32_be>(kRenderQueue)), 4, entry,
                uint32_t(*At<u32_be>(key)));  // RenderQueue::Add
}

// SceneryModel::DrawShape and Model::DrawShape: shape `shape` of `model`
// drawn: nothing for a model without meshes (+28) or a shape out of view;
// else its object constants (Recorder::RecordObject) and the scene lights'
// (Recorder::RecordSceneLights), and for each group with materials an
// entry of them and the shape's record handed to the render queue (group 0
// at minus the object's depth, group 1 at it); with `depth_pass`, also
// group 0's depth record (Model::RecordShapeDepth).
void DrawShape(Guest& g, const ShapeDraw& draw, Model* model, game::Matrix* view_projection,
               game::Matrix* world, uint32_t shape, uint32_t depth_pass) {
  g.ctx.fpscr.disableFlushMode();
  if (model->header == 0) {
    return;
  }
  if (!IsShapeVisible(g, model, view_projection, world, shape)) {
    return;
  }
  auto list = [] { return uint32_t(*At<u32_be>(kCommandList)); };
  g.Call(sub_82572568, list());  // CommandList::OpenChunk
  LitQueueEntry* entries[2];
  entries[0] = At<LitQueueEntry>(g.Call(sub_82572638, list(), 3u));  // CommandList::AllocateSlots
  entries[1] = At<LitQueueEntry>(g.Call(sub_82572638, list(), 3u));  // CommandList::AllocateSlots
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk

  uint32_t object;
  {
    guest::ReturnAddress from(g, draw.record_object_return);
    object = g.Call(sub_8257ACD0, model, view_projection, world);  // Recorder::RecordObject (wrapped)
  }
  const uint32_t lights = RecordSceneLights(g);
  auto depth = [object] { return float(At<ObjectCommand>(object)->world_view_projection.position.z); };

  for (uint32_t group = 0; group < 2; group++) {
    if (!GroupHasMaterials(draw, model, shape, group)) {
      continue;
    }
    LitQueueEntry* entry = entries[group];
    entry->constants.kind = kQueueConstants;
    entry->constants.chunk = object;
    entry->lights.kind = kQueueConstants;
    entry->lights.chunk = lights;
    entry->draw.kind = kQueueDraw;
    entry->draw.chunk = RecordShape(g, draw, model, shape, group);
    if (group == 0) {
      QueueAdd(g, AddressOf(entry), kGroup0Key, -depth());
    } else {
      QueueAdd(g, AddressOf(entry), kGroup1Key, depth());
    }
  }

  if (int32_t(depth_pass) == 0) {
    return;
  }
  g.Call(sub_82572568, list());  // CommandList::OpenChunk
  QueueEntry* entry = At<QueueEntry>(g.Call(sub_82572638, list(), 2u));  // CommandList::AllocateSlots
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk
  if (!GroupHasMaterials(draw, model, shape, 0)) {
    return;
  }
  entry->constants.kind = kQueueConstants;
  entry->constants.chunk = object;
  entry->draw.kind = kQueueDraw;
  entry->draw.chunk = RecordShapeDepth(g, model, shape, 0, 0, 0);
  QueueAdd(g, AddressOf(entry), kShapeDepthKey, -depth());
}

// Where each draw's call of Recorder::RecordObject returns to in the
// original (its wrapper tells the callers apart).
constexpr uint32_t kSceneryDrawShapeReturn = 0x821466A0;
constexpr uint32_t kModelDrawShapeReturn = 0x8257C4B8;

// --- SceneLights::RecordDirections ---

// A vector scaled by 1 / the length of its x, y, z (all four lanes), with
// one Newton step on the reciprocal square root; a zero length gives zeros.
// Written with the vector unit's own steps as the recompiled code makes
// them (the lanes in its order, w first), so the result is bit for bit the
// same.
Vector Normalized(const Vector& v) {
  const simde__m128 value = simde_mm_set_ps(v.x, v.y, v.z, v.w);
  const simde__m128 half = simde_mm_mul_ps(simde_mm_cvtepi32_ps(simde_mm_set1_epi32(1)),
                                           simde_mm_castsi128_ps(simde_mm_set1_epi32(0x3F000000)));
  const simde__m128 length_squared = simde_mm_dp_ps(value, value, 0xEF);  // vmsum3fp
  simde__m128 inverse = simde_mm_div_ps(simde_mm_set1_ps(1), simde_mm_sqrt_ps(length_squared));  // vrsqrtefp
  const simde__m128 half_length = simde_mm_mul_ps(length_squared, half);
  const simde__m128 squared = simde_mm_mul_ps(inverse, inverse);
  const simde__m128 step = simde_mm_xor_ps(
      simde_mm_sub_ps(simde_mm_mul_ps(half_length, squared), half),
      simde_mm_castsi128_ps(simde_mm_set1_epi32(int(0x80000000))));  // vnmsubfp
  inverse = simde_mm_add_ps(simde_mm_mul_ps(inverse, step), inverse);  // vmaddfp
  const simde__m128 scaled = simde_mm_mul_ps(value, inverse);
  const simde__m128i zero = simde_mm_castps_si128(simde_mm_cmpeq_ps(length_squared, simde_mm_setzero_ps()));
  const simde__m128i picked = simde_mm_or_si128(
      simde_mm_andnot_si128(zero, simde_mm_castps_si128(scaled)),
      simde_mm_and_si128(zero, simde_mm_castps_si128(length_squared)));  // vsel
  float lanes[4];
  simde_mm_storeu_ps(lanes, simde_mm_castsi128_ps(picked));
  return Vector{lanes[3], lanes[2], lanes[1], lanes[0]};
}

// SceneLights::RecordDirections: the scene lights' pixel constants c5-c39:
// the sun directions normalized, the light colors copied, the projector
// directions normalized, their colors copied.
void RecordDirections(Guest& g, DirectionConstants* out) {
  g.ctx.fpscr.enableFlushMode();  // The vector unit's mode, left on as the original leaves it.
  const Vector* suns = At<Vector>(kSunDirections);
  for (int i = 0; i < 3; i++) {
    out->sun_directions[i] = Normalized(suns[i]);
  }
  const Vector* colors = At<Vector>(kLightColors);
  const Vector* colors_2 = At<Vector>(kLightColors2);
  for (int i = 0; i < 8; i++) {
    out->light_colors[i] = colors[i];
  }
  for (int i = 0; i < 8; i++) {
    out->light_colors_2[i] = colors_2[i];
  }
  const Vector* projectors = At<Vector>(kProjectorDirections);
  for (int i = 0; i < 8; i++) {
    out->projector_directions[i] = Normalized(projectors[i]);
  }
  const Vector* projector_colors = At<Vector>(kProjectorColors);
  for (int i = 0; i < 8; i++) {
    out->projector_colors[i] = projector_colors[i];
  }
}

// --- SceneryModel::RecordShape ---

AnimationFrame* CurrentFrame(AnimationSet* set, uint32_t index);

// The recorder's frame (304 bytes) at the original's offsets: the model's
// draw hook gets the blend index's address (+80) and may change it.
struct ShapeRecordFrame {
  uint8_t linkage[80];
  i32_be blend;
  u32_be header;       // The chunk's first slot.
  uint8_t unknown_88[8];
  f32_be texture_z[4];  // Pixel c67.
  Vector c64;           // Pixel c64: the material's color_80.x, then 1, 0, 0.
  uint8_t unknown_128[176];
};
GUEST_SIZE(ShapeRecordFrame, 304);
GUEST_OFFSET(ShapeRecordFrame, blend, 80);
GUEST_OFFSET(ShapeRecordFrame, texture_z, 96);
GUEST_OFFSET(ShapeRecordFrame, c64, 112);

// A texture's slot (its stage and the texture) and a sampler state's.
struct TextureWords {
  u32_be stage;
  u32_be texture;
  uint8_t unknown_8[8];
};
GUEST_SIZE(TextureWords, 16);

// A sampler state's slot.
struct SamplerStateWords {
  u32_be sampler;
  u32_be state;
  u32_be value;
  uint8_t unknown_12[4];
};
GUEST_SIZE(SamplerStateWords, 16);

// Commands, constants and states the recorder uses.
constexpr uint32_t kSetSamplerState = 2;
constexpr uint32_t kSetVertexShader = 8;
constexpr uint32_t kSetPixelShader = 9;
constexpr uint32_t kSetTextures = 4;
constexpr uint32_t kSetBlend = 3;
constexpr uint32_t kVertexConstantsC9 = 0x00010905;  // Five vertex constants from c9.
constexpr uint32_t kPixelConstantsC63 = 0x01013F05;  // Five pixel constants from c63.
constexpr uint32_t kVertexConstantC86 = 0x00015601;  // One vertex constant, c86.
constexpr uint32_t kPixelConstantC76 = 0x01014C01;   // One pixel constant, c76.
constexpr uint32_t kBlendTable = 0x82008DAC;         // A render state 3 value per blend.
constexpr uint32_t kDefaultVertexRange = 0x8264EA80;  // c86 and c76 put back after a draw.
constexpr uint32_t kDefaultPixelConstant = 0x8264EA90;
constexpr uint32_t kEnvironmentStage = 6;

void SamplerWords(Guest& g, uint32_t sampler, uint32_t state, uint32_t value) {
  SamplerStateWords* slot = SceneSlot<SamplerStateWords>(g);
  slot->sampler = sampler;
  slot->state = state;
  slot->value = value;
}

void SetTexture(Guest& g, uint32_t stage, uint32_t texture) {
  TextureWords* slot = SceneSlot<TextureWords>(g);
  slot->texture = texture;
  slot->stage = stage;
}

// SceneryModel::RecordShape: shape `shape`'s group `group` recorded as a
// chunk: the vertex declaration, its streams (two or three), a skinned
// mesh's indices, sampler states 28 (the mesh's), 16, 20 (4) and 36 (3) of
// samplers 0-3; then for each material: its shaders (with `set_shaders`:
// the model's own pair, or the shared pair moved on by `pair_offset`),
// vertex c9-c13 (the textures' colors and the material's), the model's draw
// hook (its slot 1), pixel c63-c67, the blend, group 0's render states 96
// and 100, the cull mode, its textures (from the texture provider, through
// the animation set for animated ones) and their sampler states 0, 4 and
// 24, the environment textures, the optional vertex c86 (`vertex_range`)
// and pixel c76 (`pixel_constant`), the draw, and the optional constants
// put back; then sampler states 16 and 20 back to 1. Returns the chunk's
// first slot, or 0 when the group has no materials.
uint32_t SceneryRecordShape(Guest& g, SceneryModel* scenery, uint32_t shape, uint32_t group,
                            uint32_t set_shaders, uint32_t pixel_constant, uint32_t vertex_range,
                            uint32_t pair_offset) {
  g.ctx.fpscr.disableFlushMode();
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(ShapeRecordFrame)));
  ShapeRecordFrame* frame = At<ShapeRecordFrame>(stack.Local(0));
  Model* model = &scenery->model;
  auto material_count = [&] { return int32_t(MaterialCount(model, shape, int32_t(group))); };
  if (material_count() == 0) {
    return 0;
  }

  Mesh* mesh = model->MeshAt(shape);
  uint32_t stream_1 = AddressOf(&mesh->streams[1]);
  uint32_t stream_2 = AddressOf(&mesh->streams[2]);
  if (mesh->skinned == 1) {
    stream_1 = mesh->skin_stream_1;
    stream_2 = mesh->skin_stream_2;
  }
  g.Call(sub_82572568, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::OpenChunk
  CommandHeader* header = SceneCommand(g, kSetVertexDeclaration, 1);
  frame->header = AddressOf(header);
  u32_be* declaration = SceneSlot<u32_be>(g);
  *declaration = game::VertexDeclaration(g, int32_t(uint32_t(mesh->stream_type_0)),
                                        int32_t(uint32_t(mesh->stream_type_1)),
                                        int32_t(uint32_t(mesh->stream_type_2)));

  const bool two_streams =
      mesh->stream_type_1 == 9 || mesh->stream_type_1 == 10 || mesh->stream_type_2 == 6;
  SceneCommand(g, kSetStreamSource, two_streams ? 2 : 3);
  StreamWords* source = SceneSlot<StreamWords>(g);
  source->stream = 0;
  source->buffer = AddressOf(&mesh->streams[0]);
  source->offset = 0;
  source->stride = mesh->stream_stride_0;
  source = SceneSlot<StreamWords>(g);
  source->stream = 1;
  source->buffer = stream_1;
  source->offset = 0;
  source->stride = mesh->stream_stride_1;
  if (!two_streams) {
    source = SceneSlot<StreamWords>(g);
    source->stream = 2;
    source->buffer = stream_2;
    source->offset = 0;
    source->stride = mesh->stream_stride_2;
  }
  if (mesh->skinned == 1) {
    const VertexWeights* weights = mesh->weights;
    if (weights->index_buffer != 0) {
      SceneCommand(g, kSetIndices, 1);
      IndicesWords* indices = SceneSlot<IndicesWords>(g);
      indices->unknown_8 = 1;
      indices->buffer = weights->index_buffer;
    }
  }
  SceneCommand(g, kSetSamplerState, 4);
  for (uint32_t sampler = 0; sampler < 4; sampler++) {
    SamplerWords(g, sampler, 28, mesh->sampler_28_value);
  }
  Material* material = game::ModelMaterial(model, shape, 0, int32_t(group));
  SceneCommand(g, kSetSamplerState, 12);
  for (uint32_t sampler = 0; sampler < 4; sampler++) {
    SamplerWords(g, sampler, 16, 4);
    SamplerWords(g, sampler, 20, 4);
    SamplerWords(g, sampler, 36, 3);
  }

  if (material_count() > 0) {
    frame->c64.y = 1.0f;
    frame->c64.z = 0.0f;
    frame->c64.w = 0.0f;
    for (uint32_t index = 0;; index++) {
      for (f32_be& z : frame->texture_z) {
        z = 1.0f;
      }
      frame->blend = material->blend;
      if (set_shaders != 0) {
        if (material->shared_pair == 0) {
          SceneCommand(g, kSetVertexShader, 1);
          *SceneSlot<u32_be>(g) =
              guest::Element<game::ShaderPairEntry>(model->shader_pairs, material->shader_pair)->vertex;
          SceneCommand(g, kSetPixelShader, 1);
          *SceneSlot<u32_be>(g) =
              guest::Element<game::ShaderPairEntry>(model->shader_pairs, material->shader_pair)->pixel;
        } else {
          SceneCommand(g, kSetVertexShader, 1);
          u32_be* slot = SceneSlot<u32_be>(g);
          *slot = game::SharedVertexShader(material->shared_pair + pair_offset);
          SceneCommand(g, kSetPixelShader, 1);
          slot = SceneSlot<u32_be>(g);
          *slot = game::SharedPixelShader(material->shared_pair + pair_offset);
        }
      }

      // Vertex c9-c13: each texture's color (x, y from the model's float4
      // track value, its z kept for pixel c67) or (0, 0, 0, 1), then the
      // material's own color.
      CommandHeader* constants_command = SceneCommand(g, kSetPixelConstants, 5);
      constants_command->argument = kVertexConstantsC9;
      Vector* constants = At<Vector>(g.Call(sub_82572638, uint32_t(*At<u32_be>(kCommandList)), 5u));  // CommandList::AllocateSlots
      for (int32_t t = 0; t < material->texture_count; t++) {
        const int32_t id = material->texture_colors[t];
        if (id != -1) {
          const uint32_t values = model->tracks[0].values;
          constants[t].x = guest::Element<Vector>(values, uint32_t(id))->x;
          constants[t].y = guest::Element<Vector>(values, uint32_t(id))->y;
          *guest::Element<f32_be>(AddressOf(frame->texture_z), uint32_t(t)) =
              guest::Element<Vector>(values, uint32_t(id))->z;
        } else {
          constants[t] = Vector{0.0f, 0.0f, 0.0f, 1.0f};
          *guest::Element<f32_be>(AddressOf(frame->texture_z), uint32_t(t)) =
              material->texture_color_z[t];
        }
      }
      constants[4] = material->vertex_color;
      if (material->animation != -1) {
        g.Call(sub_82586878, &model->tracks[3], &constants[4],
               int32_t(material->animation));  // TrackSet::ApplyMaterial
      }
      g.CallVirtual(model, 1, shape, index, &constants[4], &frame->blend);  // The model's draw hook

      // Pixel c63-c67.
      CommandHeader* pixel_command = SceneCommand(g, kSetPixelConstants, 5);
      pixel_command->argument = kPixelConstantsC63;
      Vector* pixel = At<Vector>(g.Call(sub_82572638, uint32_t(*At<u32_be>(kCommandList)), 5u));  // CommandList::AllocateSlots
      const Vector texture_z{frame->texture_z[0], frame->texture_z[1], frame->texture_z[2],
                             frame->texture_z[3]};
      pixel[0] = material->color_64;
      frame->c64.x = material->color_80.x;
      pixel[1] = frame->c64;
      pixel[2] = material->color_16;
      pixel[2].w = material->color_80.z;
      pixel[3] = material->color_32;
      pixel[4] = texture_z;

      SceneCommand(g, kSetBlend, 1);
      StateWords* blend = SceneSlot<StateWords>(g);
      blend->state = 0;
      blend->value = *guest::Element<u32_be>(kBlendTable, uint32_t(int32_t(frame->blend)));
      if (group == 0) {
        SceneCommand(g, kSetRenderState, 2);
        StateWords* state = SceneSlot<StateWords>(g);
        state->state = 96;
        state->value = material->has_alpha != 0 ? 1u : 0u;
        state = SceneSlot<StateWords>(g);
        state->state = kRenderState100;
        state->value = material->has_alpha != 0 ? 64u : uint32_t(material->render_state_100);
      }
      SceneCommand(g, kSetRenderState, 1);
      StateWords* cull = SceneSlot<StateWords>(g);
      cull->state = kRenderStateCull;
      cull->value = material->cull_mode;

      // Its textures, and their sampler states.
      if (model->texture_provider != nullptr && material->texture_count != 0) {
        SceneCommand(g, kSetTextures, uint32_t(int32_t(material->texture_count)));
        for (int32_t t = 0; t < material->texture_count; t++) {
          uint32_t texture = 0;
          bool look_up = true;
          uint32_t xpr = material->texture_xprs[t], id = material->texture_ids[t];
          if (scenery->animations != 0 && material->texture_xprs[t] == 0) {
            const AnimationFrame* current = CurrentFrame(At<AnimationSet>(scenery->animations), id);
            look_up = current != nullptr;
            if (current != nullptr) {
              xpr = current->unknown_0;
              id = current->texture;
            }
          }
          if (look_up) {
            texture = g.CallVirtual(model->texture_provider.get(), 5, xpr, id);  // TextureProvider::Texture
          }
          SetTexture(g, uint32_t(t), texture);
        }
        SceneCommand(g, kSetSamplerState, uint32_t(int32_t(material->texture_count)) * 3);
        for (int32_t t = 0; t < material->texture_count; t++) {
          SamplerWords(g, uint32_t(t), 0, material->sampler_state_0[t]);
          SamplerWords(g, uint32_t(t), 4, material->sampler_state_4[t]);
          SamplerWords(g, uint32_t(t), 24, scenery->sampler_24_value);
        }
      } else {
        SceneCommand(g, kSetTextures, 4);
        for (uint32_t t = 0; t < 4; t++) {
          SetTexture(g, t, 0);
        }
      }
      // The environment textures: flag 0x2 from the provider's slot 6,
      // flag 0x4 from its slot 5, both on stage 6.
      if (material->flags & 0x2) {
        const uint32_t texture = g.CallVirtual(model->texture_provider.get(), 6,
                                               uint32_t(material->environment_xpr),
                                               uint32_t(material->environment_texture));
        SceneCommand(g, kSetTextures, 1);
        SetTexture(g, kEnvironmentStage, texture);
      }
      if (material->flags & 0x4) {
        const uint32_t texture = g.CallVirtual(model->texture_provider.get(), 5,
                                               uint32_t(material->environment_xpr),
                                               uint32_t(material->environment_texture));
        SceneCommand(g, kSetTextures, 1);
        SetTexture(g, kEnvironmentStage, texture);
      }

      // The optional constants: c86 (the range's two ends and 1 / its
      // span) and c76.
      if (vertex_range != 0) {
        SceneCommand(g, kSetPixelConstants, 1)->argument = kVertexConstantC86;
        Vector* slot = SceneSlot<Vector>(g);
        const float low = At<f32_be>(vertex_range)[0];
        const float high = At<f32_be>(vertex_range)[1];
        const float span = high - low;
        slot->x = low;
        slot->w = 0.0f;
        slot->y = high;
        slot->z = 1.0f / span;
      }
      if (pixel_constant != 0) {
        SceneCommand(g, kSetPixelConstants, 1)->argument = kPixelConstantC76;
        *SceneSlot<Vector>(g) = *At<Vector>(pixel_constant);
      }

      // The draw.
      SceneCommand(g, kSetMaterialIndices, 1);
      *SceneSlot<u32_be>(g) = AddressOf(&material->index_buffer);
      SceneCommand(g, kDrawIndexed, 1);
      DrawWords* draw = SceneSlot<DrawWords>(g);
      draw->unknown_4 = 0;
      draw->unknown_6 = 0;
      draw->word_0 = material->draw_word_0;
      draw->word_8 = material->draw_word_8;

      // The optional constants put back.
      if (vertex_range != 0) {
        SceneCommand(g, kSetPixelConstants, 1)->argument = kVertexConstantC86;
        *SceneSlot<Vector>(g) = *At<Vector>(kDefaultVertexRange);
      }
      if (pixel_constant != 0) {
        SceneCommand(g, kSetPixelConstants, 1)->argument = kPixelConstantC76;
        *SceneSlot<Vector>(g) = *At<Vector>(kDefaultPixelConstant);
      }

      material++;
      if (!(int32_t(index + 1) < material_count())) {
        break;
      }
    }
  }

  SceneCommand(g, kSetSamplerState, 8);
  for (uint32_t sampler = 0; sampler < 4; sampler++) {
    SamplerWords(g, sampler, 16, 1);
    SamplerWords(g, sampler, 20, 1);
  }
  SceneCommand(g, kEndCommands, 0);
  g.Call(sub_825725E8, uint32_t(*At<u32_be>(kCommandList)));  // CommandList::CloseChunk
  return frame->header;
}

// --- Small getters and setters ---

// AnimationSet::CurrentFrame: animation `index`'s current frame, or nullptr
// past its last. Read this way, an animation's first word is its frame
// count and its second the current frame; the frames (8 bytes each) follow
// its table of `key_count` words.
AnimationFrame* CurrentFrame(AnimationSet* set, uint32_t index) {
  Animation* animation = set->Entry(index);
  const int32_t frames = animation->key_count;
  const int32_t frame = animation->key;
  if (!(frame < frames)) {
    return nullptr;
  }
  const uint32_t first = AddressOf(animation->KeyFrame(frames));  // Past the table.
  return guest::Element<AnimationFrame>(first, uint32_t(frame));
}

// TextureProvider::Set: slot `slot` of the XPR list set. False where the
// slot is past the list's size or room (the original grows the list).
bool TextureListSet(XPRList* list, uint32_t slot, uint32_t texture) {
  const game::PointerVector& items = list->items;
  if (items.first == 0 || slot >= items.Capacity() || slot >= items.Size()) {
    return false;
  }
  *guest::Element<u32_be>(items.first, slot) = texture;
  return true;
}

}  // namespace

// --- The functions other files of the group call (course_scenery.h) ---

namespace game {

// ModelList::Load: the container's models (its count is its first word),
// each a scenery model loaded from its entry and put at the list's end.
void ModelListLoad(Guest& g, ModelList* list, DataFile* container) {
  const int32_t count = int32_t(container->count);
  list->models.Reset();
  for (int32_t i = 0; i < count; i++) {
    SceneryModel* model =
        At<SceneryModel>(g.Call(sub_8258C000, uint32_t(sizeof(SceneryModel))));  // operator new
    if (model != nullptr) {
      ModelConstruct(g, &model->model);
      model->model.vtable = SceneryModel::kVtable;
    }
    const uint32_t entry = g.Call(sub_82292170, container, i, 0u);  // DataFile::GetEntry
    ModelLoad(g, &model->model, entry, 0);
    list->models.Insert(AddressOf(model), uint32_t(int32_t(list->models.count)));
  }
  list->count = count;
}

// ModelList::LoadPoints: the point list's models loaded, then its point
// buffers set and its draw state cleared.
void PointListLoad(Guest& g, PointList* list, DataFile* container) {
  ModelListLoad(g, &list->list, container);
  for (int i = 0; i < PointList::kPointBufferCount; i++) {
    list->point_buffers[i] = AddressOf(guest::Element<PointBuffer>(kPointBuffers, i));
  }
  list->animations = 0;
  list->texture_provider = 0;
  list->first_group = 0;
  list->point_buffer_count = PointList::kPointBufferCount;
}

// ModelList::Release: each model released (Model::Release) and deleted
// (its vtable's slot 0, the deleting destructor). The list keeps its
// entries.
void ModelListRelease(Guest& g, ModelList* list) {
  for (int32_t i = 0; i < list->count; i++) {
    SceneryModel* model = At<SceneryModel>(*list->models.ItemAtPosition(uint32_t(i)));
    ModelRelease(g, &model->model);
    if (model != nullptr) {
      g.CallVirtual(model, 0, 1u);  // The deleting destructor
    }
  }
}

// ModelList::IsBEntryOn: whether entry `entry` of the course set's +2124
// list is drawn: its bit in the race data's set list flags.
bool ModelListIsBEntryOn(uint32_t entry) {
  static constexpr uint32_t kBits[12] = {0x20, 0x40, 0x80, 0x1,   0x2,   0x4,
                                         0x8,  0x10, 0x100, 0x200, 0x400, 0x800};
  const uint32_t flags = HeadOf(TheRace())->set_list_flags;
  return entry < 12 && (flags & kBits[entry]) != 0;
}

// ModelList::IsCEntryOn: whether entry `entry` of the course set's +2652
// list is drawn. Entry 24 in play mode 3 (and mode 4 at a stage of type
// 0); else entries 0 to 23 are each a course layout's, and 16 to 23 only
// with the camera flags 0x20 or 0x40.
bool ModelListIsCEntryOn(Guest& g, uint32_t entry) {
  constexpr uint32_t kStageEntry = 24;
  RaceData* race = TheRace();
  const uint32_t play_mode = race->play_mode;
  if (play_mode == 5) {
    return false;
  }
  if (play_mode == 3) {
    return entry == kStageEntry;
  }
  if (play_mode == 4 && g.Call(sub_820FFD38, race->unknown_444832, -1) == 0) {  // Stages::Type
    return entry == kStageEntry;
  }
  const int32_t layout = CourseLayout(g);
  if (layout == -1) {
    return false;
  }
  race = TheRace();
  if (race->camera_flags & kTvCameras) {
    return false;
  }
  static constexpr int32_t kEntryLayouts[24] = {4, 5, 6, 7,  8,  9,  10, 11, 14, 15, 12, 13,
                                                0, 1, 3, 2, 16, 17, 18, 19, 20, 21, 22, 23};
  const bool on = entry < 24 && layout == kEntryLayouts[entry];
  if (entry - 16 < 8 && (race->camera_flags & kSetListExtras) == 0) {
    return false;
  }
  return on;
}

// PointList::IsEntryOn: whether entry `entry` of the course set's point
// list is drawn: in race modes 4 and 7 entry n for course n (0 to 7), in
// mode 3 entries 8 to 19 each for one course.
bool PointListIsEntryOn(int32_t entry) {
  const RaceData* race = TheRace();
  if (race == nullptr || (race->camera_flags & kTvCameras) || race->play_mode == 5) {
    return false;
  }
  const int32_t course = HeadOf(race)->course;
  const int32_t mode = race->unknown_827892;
  if (mode == 4 || mode == 7) {
    return entry >= 0 && entry <= 7 && course == entry;
  }
  if (mode == 3) {
    if (entry >= 8 && entry <= 15) {
      return course == entry + 3;
    }
    switch (entry) {
      case 16: return course == 21;
      case 17: return course == 22;
      case 18: return course == 19;
      case 19: return course == 20;
      default: return false;
    }
  }
  return false;
}

// Model::DrawShapeDepth: shape `shape` of `model` drawn in a depth pass
// (probably the shadows'): nothing for a model without meshes (+28) or a
// shape out of view; else its object constants (Recorder::RecordObject)
// and, when the group has materials, an entry of them and the shape's
// depth record (Model::RecordShapeDepth) handed to the render queue.
void ModelDrawShapeDepth(Guest& g, Model* model, Matrix* view_projection, Matrix* world,
                               uint32_t shape, uint8_t group, uint32_t mask, uint32_t with_alpha) {
  g.ctx.fpscr.disableFlushMode();
  if (model->header == 0) {
    return;
  }
  if (!IsShapeVisible(g, model, view_projection, world, shape)) {
    return;
  }
  uint32_t object;
  {
    guest::ReturnAddress from(g, 0x8257C6B4);
    object = g.Call(sub_8257ACD0, model, view_projection, world);  // Recorder::RecordObject (wrapped)
  }
  auto list = [] { return uint32_t(*At<u32_be>(kCommandList)); };
  g.Call(sub_82572568, list());  // CommandList::OpenChunk
  QueueEntry* entry = At<QueueEntry>(g.Call(sub_82572638, list(), 2u));  // CommandList::AllocateSlots
  g.Call(sub_825725E8, list());  // CommandList::CloseChunk

  const uint32_t group_index = group != 0 ? 1 : 0;
  if (model->MeshAt(shape)->materials->group_counts[group_index] == 0) {
    return;
  }
  entry->constants.chunk = object;
  entry->constants.kind = kQueueConstants;
  entry->draw.kind = kQueueDraw;
  entry->draw.chunk = RecordShapeDepth(g, model, shape, group_index, mask, with_alpha);
  g.ctx.f1.f64 = -At<ObjectCommand>(object)->world_view_projection.position.z;
  g.CallVirtual(At<u32_be>(*At<u32_be>(kRenderQueue)), 4, entry,
                uint32_t(*At<u32_be>(kDepthShapeKey)));  // RenderQueue::Add
}

// PointList::Draw: entry `entry`'s points drawn for the camera pass: model
// 0 at the points of groups `first_group` to 2, model 1 at group 3's. The
// frame stays in guest memory: the matrix functions are recompiled.
void PointListDraw(Guest& g, PointList* list, uint32_t entry, uint32_t camera_pass) {
  g.ctx.fpscr.disableFlushMode();
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(PointDrawFrame)));
  PointDrawFrame* frame = At<PointDrawFrame>(stack.Local(0));
  frame->world = kIdentityMatrix;
  frame->translation = Vector{0.0f, 0.0f, 0.0f, 1.0f};
  g.Call(sub_8258CB90, &frame->view_projection, &At<game::CameraPass>(camera_pass)->view_projection,
         uint32_t(sizeof(game::Matrix)));  // memcpy
  const uint32_t points = *guest::Element<u32_be>(AddressOf(list->point_buffers), entry);

  SceneryModel* model = PointModel(list, 0);
  const int32_t first_group = int32_t(list->first_group);
  if (first_group < kLastModelGroup) {
    frame->vectors_144[0].position.w = 1.0f;
    int32_t offset = int32_t(uint32_t(first_group) * kPointGroupBytes);
    do {
      for (int i = 0; i < 4; i++, offset += kPointBytes) {
        DrawAtPoint(g, frame, model, At<Vector>(points + uint32_t(offset)),
                    &frame->vectors_144[0].position, &frame->vectors_144[1].range);
      }
    } while (offset < kLastModelGroup * kPointGroupBytes);
  }

  frame->vectors_144[1].position.w = 1.0f;
  model = PointModel(list, 1);
  for (int32_t offset = kLastModelGroup * kPointGroupBytes;
       offset < (kLastModelGroup + 1) * kPointGroupBytes; offset += kPointBytes) {
    DrawAtPoint(g, frame, model, At<Vector>(points + uint32_t(offset)),
                &frame->vectors_144[1].position, &frame->vectors_144[0].range);
  }
}

void SceneryModelDrawShape(Guest& g, SceneryModel* model, Matrix* view_projection,
                                 Matrix* world, uint32_t shape, uint32_t depth_pass,
                                 const Vector* pixel_constant, const DrawRange* range,
                                 uint32_t pair_offset) {
  const ShapeDraw draw{ShapeRecorder::kScenery, kSceneryDrawShapeReturn, AddressOf(pixel_constant),
                       AddressOf(range), pair_offset};
  DrawShape(g, draw, &model->model, view_projection, world, shape, depth_pass);
}

void ModelDrawShape(Guest& g, Model* model, Matrix* view_projection, Matrix* world,
                          uint32_t shape, uint32_t depth_pass, const Vector* pixel_constant,
                          const DrawRange* range) {
  const ShapeDraw draw{ShapeRecorder::kModel, kModelDrawShapeReturn, AddressOf(pixel_constant),
                       AddressOf(range), 0};
  DrawShape(g, draw, model, view_projection, world, shape, depth_pass);
}

// SceneryModel::ResetDrawState.
void SceneryModelResetDrawState(SceneryModel* model) {
  model->animations = 0;
  model->sampler_24_value = 1;
  model->unknown_440 = 0;
}

// PassDescription::SetBillboardPass.
void SetBillboardPass(SceneryDrawPass* pass, uint32_t word_0, uint32_t word_4) {
  pass->billboard[0] = word_0;
  pass->billboard[1] = word_4;
}

}  // namespace game

#define SBX_SCENERY_LISTS_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();                     \
  if (!On()) {                             \
    __imp__##name(ctx, base);              \
    return;                                \
  }                                        \
  Guest g(ctx, base);

// SceneryModel::ResetDrawState: r3 the model. Returns it.
extern "C" REX_FUNC(sub_82145338) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82145338)
  game::SceneryModelResetDrawState(At<SceneryModel>(ctx.r3.u32));
}

// ModelList::Load: r3 the list, r4 the container.
extern "C" REX_FUNC(sub_82146CD0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82146CD0)
  game::ModelListLoad(g, At<ModelList>(ctx.r3.u32), At<game::DataFile>(ctx.r4.u32));
}

// ModelList::LoadPoints: r3 the point list, r4 the container.
extern "C" REX_FUNC(sub_82146DD0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82146DD0)
  game::PointListLoad(g, At<PointList>(ctx.r3.u32), At<game::DataFile>(ctx.r4.u32));
}

// ModelList::Release: r3 the list.
extern "C" REX_FUNC(sub_82146848) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82146848)
  game::ModelListRelease(g, At<ModelList>(ctx.r3.u32));
}

// Race::CourseLayout: no arguments.
extern "C" REX_FUNC(sub_8228C7F0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8228C7F0)
  ctx.r3.s64 = CourseLayout(g);
}

// ModelList::IsBEntryOn: r3 the list (not read), r4 the entry.
extern "C" REX_FUNC(sub_821460E0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_821460E0)
  ctx.r3.u64 = game::ModelListIsBEntryOn(ctx.r4.u32);
}

// ModelList::IsCEntryOn: r3 the list (not read), r4 the entry.
extern "C" REX_FUNC(sub_821461E8) {
  SBX_SCENERY_LISTS_FALLBACK(sub_821461E8)
  const uint32_t entry = ctx.r4.u32;
  ctx.r3.u64 = game::ModelListIsCEntryOn(g, entry);
}

// PointList::IsEntryOn: r3 the list (not read), r4 the entry.
extern "C" REX_FUNC(sub_821463A8) {
  SBX_SCENERY_LISTS_FALLBACK(sub_821463A8)
  ctx.r3.u64 = game::PointListIsEntryOn(ctx.r4.s32);
}

// AnimationSet::CurrentFrame: r3 the set, r4 the animation. The frame (0
// for none).
extern "C" REX_FUNC(sub_82145410) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82145410)
  ctx.r3.u64 = AddressOf(CurrentFrame(At<AnimationSet>(ctx.r3.u32), ctx.r4.u32));
}

// TextureProvider::Set (the texture list's vtable slot 4): r3 the list, r4
// the slot, r5 the texture. A slot past the list's size runs the original,
// which grows it.
extern "C" REX_FUNC(sub_8257A018) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257A018)
  if (!TextureListSet(At<XPRList>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32)) {
    __imp__sub_8257A018(ctx, base);
  }
}

// VertexDeclarations::ForDepth: r3 the stream type. The depth pass's
// declaration for it.
extern "C" REX_FUNC(sub_8256F4F0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8256F4F0)
  ctx.r3.u64 = DepthDeclaration(ctx.r3.u32);
}

// PassDescription::Set (only SetBillboardPass's tail call): r3 the pass,
// r4 and r6 its two billboard words (r5 unused).
extern "C" REX_FUNC(sub_8209BE60) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8209BE60)
  game::SetBillboardPass(At<SceneryDrawPass>(ctx.r3.u32), ctx.r4.u32, ctx.r6.u32);
}

// PassDescription::SetBillboardPass: r3 the pass, r4 and r6 its two words.
// Returns r3.
extern "C" REX_FUNC(sub_82146EF8) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82146EF8)
  game::SetBillboardPass(At<SceneryDrawPass>(ctx.r3.u32), ctx.r4.u32, ctx.r6.u32);
}

// SceneLights::RecordProjectors: r3 the record; the eye and the projectors
// copied in. Returns it.
extern "C" REX_FUNC(sub_8256F6D8) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8256F6D8)
  RecordProjectors(At<ProjectorRecord>(ctx.r3.u32));
}

// Model::IsShapeVisible: r3 the model, r4 the view-projection, r5 the
// world matrix, r6 the shape.
extern "C" REX_FUNC(sub_8257AAE0) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257AAE0)
  ctx.r3.u64 = IsShapeVisible(g, At<Model>(ctx.r3.u32), At<game::Matrix>(ctx.r4.u32),
                              At<game::Matrix>(ctx.r5.u32), ctx.r6.u32);
}

// Model::DrawShapeDepth: r3 the model, r4 the view-projection, r5 the
// world matrix, r6 the shape, r7 the group (a byte), r8 a material mask, r9
// a flag (both for Model::RecordShapeDepth).
extern "C" REX_FUNC(sub_8257C660) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257C660)
  game::ModelDrawShapeDepth(g, At<Model>(ctx.r3.u32), At<game::Matrix>(ctx.r4.u32),
                            At<game::Matrix>(ctx.r5.u32), ctx.r6.u32, uint8_t(ctx.r7.u32), ctx.r8.u32,
                            ctx.r9.u32);
}

// PointList::Draw: r3 the point list, r4 the entry, r5 the camera pass.
extern "C" REX_FUNC(sub_821468E8) {
  SBX_SCENERY_LISTS_FALLBACK(sub_821468E8)
  game::PointListDraw(g, At<PointList>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32);
}

// Model::RecordShapeDepth: r3 the model, r4 the shape, r5 the group, r7 a
// material mask, r8 the alpha flag (a byte). Returns the chunk's first
// slot, or 0.
extern "C" REX_FUNC(sub_8257BA38) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257BA38)
  ctx.r3.u64 = RecordShapeDepth(g, At<Model>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32, ctx.r7.u32,
                                ctx.r8.u32);
}

// SceneryModel::DrawShape: r3 the model, r4 the view-projection, r5 the
// world matrix, r6 the shape, r7 whether to draw the depth pass too, r8 an
// optional pixel c76, r9 an optional range for vertex c86, r10 an offset
// added to the shared shader pair.
extern "C" REX_FUNC(sub_82146618) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82146618)
  game::SceneryModelDrawShape(g, At<SceneryModel>(ctx.r3.u32), At<game::Matrix>(ctx.r4.u32),
                              At<game::Matrix>(ctx.r5.u32), ctx.r6.u32, ctx.r7.u32,
                              At<Vector>(ctx.r8.u32), At<game::DrawRange>(ctx.r9.u32), ctx.r10.u32);
}

// Model::DrawShape: r3 the model, r4 the view-projection, r5 the world
// matrix, r6 the shape, r7 the depth pass too, r8 and r9 to
// Model::RecordShape.
extern "C" REX_FUNC(sub_8257C430) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257C430)
  game::ModelDrawShape(g, At<Model>(ctx.r3.u32), At<game::Matrix>(ctx.r4.u32),
                       At<game::Matrix>(ctx.r5.u32), ctx.r6.u32, ctx.r7.u32, At<Vector>(ctx.r8.u32),
                       At<game::DrawRange>(ctx.r9.u32));
}

// SceneLights::RecordDirections: r3 the constants (35 vectors).
extern "C" REX_FUNC(sub_82571500) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82571500)
  RecordDirections(g, At<DirectionConstants>(ctx.r3.u32));
}

// SceneryModel::RecordShape: r3 the model, r4 the shape, r5 the group, r6
// whether to set the shaders, r7 an optional pixel c76, r8 an optional
// range for vertex c86, r9 an offset to the shared shader pair. Returns the
// chunk's first slot, or 0.
extern "C" REX_FUNC(sub_82145460) {
  SBX_SCENERY_LISTS_FALLBACK(sub_82145460)
  ctx.r3.u64 = SceneryRecordShape(g, At<SceneryModel>(ctx.r3.u32), ctx.r4.u32, ctx.r5.u32,
                                  ctx.r6.u32, ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

// Recorder::RecordSceneLights: r3 the model, r4 the world matrix (neither
// read). Returns the command's first slot.
extern "C" REX_FUNC(sub_8257AD90) {
  SBX_SCENERY_LISTS_FALLBACK(sub_8257AD90)
  ctx.r3.u64 = RecordSceneLights(g);
}

// Recorder::RecordObject (sub_8257ACD0): r3 the model (not read), r4 the
// view-projection, r5 the world matrix. Returns the command's first slot.
// Called from subtick's wrapper (subtick/car_transforms.cpp) in place of
// the recompiled function; the wrapper reads the caller (lr) before.
void hooks::NativeSceneryLists_RecordObject(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8257ACD0(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ctx.r3.u64 = RecordObject(g, ctx.r4.u32, ctx.r5.u32);
}
