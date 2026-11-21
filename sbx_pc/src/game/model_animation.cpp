// The models' animation as our own C++ (--native_model_animation, on by
// default): the track sets (their set-ups from the part file's sections,
// their releases, and the per-frame sampling and stepping Model::Animate
// does), the skeleton's construction, frees, node values and channel
// animation, the instances' cursors, and the primitive drawer's
// constructor, Init (its shader compiles answered by SBX's stored shaders,
// src/hooks/shader_compiles.cpp), shaders and draws. Model::Animate's C++ is
// here for SBX's own wrapper of it to call. The curves' key interpolation
// stays recompiled.

#include "game/model_animation.h"

#include "base/settings.h"
#include "game/model.h"

SBX_SETTING_BOOL(native_model_animation, true, "Overrides", "Scene",
                 "Our own C++ for the models' animation (the track sets' set-ups, sampling "
                 "and stepping, the skeletons' set-up, frees and channel animation) instead "
                 "of the recompiled functions");

namespace {

using game::AnimationChannel;
using game::AnimationKey;
using game::TrackSet;
using game::SkeletonCursorClear;
using game::TrackSetRelease;
using game::TrackSetReleaseArray;
using game::NodeState;
using game::PrimitiveDrawer;
using game::Skeleton;
using game::SkeletonCursor;
using game::SkeletonNodeEntry;
using game::TrackValue16;
using game::TrackValue208;
using game::TrackValue32;
using game::ValueArrayHeader;
using game::Vector;
using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

// A 12-byte record (sub_82586FC0's; a physical pad's +168 and +180).
struct PadRecord {
  guest::f32_be unknown_0;
  u32_be unknown_4[2];
};
GUEST_SIZE(PadRecord, 12);

// The largest track counts whose values operator new[] is asked for; past
// them it's asked for 0xFFFFFFFF bytes (and refuses).
constexpr uint32_t kMaxFloat4Values = 0x0FFFFFFF;
constexpr uint32_t kMaxFloatValues = 0x3FFFFFFF;
constexpr uint32_t kMax32Values = 0x07FFFFFF;
constexpr uint32_t kMax208Values = 0x013B13B1;
constexpr uint32_t kRefusedSize = 0xFFFFFFFF;

// The primitive drawer's shaders: two vertex and five pixel shaders, each
// made from HLSL source at run time.
struct PrimitiveShader {
  u32_be source;
  u32_be length;
  u32_be shader;
};
GUEST_SIZE(PrimitiveShader, 12);
constexpr uint32_t kPrimitiveVertexShaders = 0x8297338C;
constexpr uint32_t kPrimitivePixelShaders = 0x829733A4;
constexpr uint32_t kPrimitiveVertexBytes = 24;  // Position, color, texture coordinate.
constexpr uint32_t kPrimitiveCopyReturn = 0x82586EE4;  // Where its memcpy returns to.

bool On() { return SBX_GET(native_model_animation); }

// --- The heap ---

uint32_t NewArray(Guest& g, uint32_t bytes) { return g.Call(sub_8258C0C0, bytes); }  // operator new[]
void DeleteArray(Guest& g, uint32_t block) { g.Call(sub_8258C940, block); }         // operator delete[]

// operator new[]'s size for `count` values of `bytes` each after a 16-byte
// count header: refused past `max`, or when the header doesn't fit.
uint32_t CountedArraySize(uint32_t count, uint32_t bytes, uint32_t max) {
  if (count > max) {
    return kRefusedSize;
  }
  const uint32_t values = count * bytes;
  return values <= 0xFFFFFFEFu ? values + uint32_t(sizeof(ValueArrayHeader)) : kRefusedSize;
}

// A counted array of `count` values of `bytes` each: the header's count set,
// the values' address (0 when operator new[] gave nothing).
uint32_t NewCountedArray(Guest& g, uint32_t count, uint32_t bytes, uint32_t max) {
  const uint32_t block = NewArray(g, CountedArraySize(count, bytes, max));
  if (block == 0) {
    return 0;
  }
  At<ValueArrayHeader>(block)->count = count;
  return block + uint32_t(sizeof(ValueArrayHeader));
}

// Frees a counted array by its values' address.
void DeleteCountedArray(Guest& g, uint32_t values) {
  DeleteArray(g, values - uint32_t(sizeof(ValueArrayHeader)));
}

// --- Track sets ---

void TrackSetClear(TrackSet* set) {
  set->section = nullptr;
  set->track_count = nullptr;
  set->tracks = nullptr;
  set->start_time = 0.0f;
}

// Binds a section (0: none).
void TrackSetView(TrackSet* set, uint32_t section) {
  if (section == 0) {
    TrackSetClear(set);
    return;
  }
  set->section = At<game::AnimationSection>(section);
  set->track_count = &set->section->track_count;
  set->tracks = set->section->tracks;
  set->start_time = set->section->start_time;
}

// A track's start or end time; the section's defaults for track -1; 0.0
// with no section.
float TrackSetStartTime(TrackSet* set, int32_t track) {
  if (!set->HasSection()) {
    return 0.0f;
  }
  return track == -1 ? float(set->section->start_time) : float(set->Track(track).start_time);
}

float TrackSetEndTime(TrackSet* set, int32_t track) {
  if (!set->HasSection()) {
    return 0.0f;
  }
  return track == -1 ? float(set->section->end_time) : float(set->Track(track).end_time);
}

// A channel's curve sampled at `time` (Curve::Sample, recompiled).
float SampleCurve(Guest& g, TrackSet* set, AnimationChannel* channel, float time) {
  g.ctx.f1.f64 = double(time);
  g.Call(sub_82587678, set, channel->keys, uint32_t(channel->key_count));  // Curve::Sample
  return float(g.ctx.f1.f64);
}

// The +304 set's value for a track: (0, 0, 1), then each channel sampled at
// the value's time into the float it names.
void TrackSetSampleFloat4(Guest& g, TrackSet* set, uint32_t index) {
  if (!set->HasSection()) {
    return;
  }
  game::AnimationTrack& track = set->Track(index);
  AnimationChannel* channel = set->Channels(index);
  auto value = [&] { return Element<TrackValue16>(set->values, index); };
  value()->x = 0.0f;
  value()->y = 0.0f;
  value()->z = 1.0f;
  for (int32_t i = 0; i < int32_t(track.channel_count); i++) {
    const float sampled = SampleCurve(g, set, channel, value()->time);
    value()->Float(channel->component) = sampled;
    channel = channel->Next();
  }
}

// The +364 set's: the mask of the floats written cleared, then each channel
// marks its float and is sampled into it.
void TrackSetSample32(Guest& g, TrackSet* set, uint32_t index) {
  if (!set->HasSection()) {
    return;
  }
  game::AnimationTrack& track = set->Track(index);
  AnimationChannel* channel = set->Channels(index);
  TrackValue32* value = Element<TrackValue32>(set->values, index);
  value->written = 0;
  for (int32_t i = 0; i < int32_t(track.channel_count); i++) {
    const uint32_t shift = channel->component & 63;
    value->written |= shift < 32 ? 1u << shift : 0u;
    const float time = Element<TrackValue32>(set->values, index)->time;
    value->Float(channel->component) = SampleCurve(g, set, channel, time);
    channel = channel->Next();
  }
}

// A value's time stepped on and looped between its track's start and end
// times. Whether it looped.
bool AdvanceTime(TrackSet* set, uint32_t index, guest::f32_be& time, double step) {
  time = float(step + double(float(time)));
  if (float(time) > TrackSetEndTime(set, index)) {
    time = TrackSetStartTime(set, index);
    return true;
  }
  if (float(time) < TrackSetStartTime(set, index)) {
    time = TrackSetEndTime(set, index);
    return true;
  }
  return false;
}

bool TrackSetAdvance(TrackSet* set, uint32_t index, double step) {
  return AdvanceTime(set, index, Element<TrackValue16>(set->values, index)->time, step);
}

bool TrackSetAdvance32(TrackSet* set, uint32_t index, double step) {
  return AdvanceTime(set, index, Element<TrackValue32>(set->values, index)->time, step);
}

// Model::Animate: each track of the model's float4 set sampled and stepped
// one frame, then each of its 32-byte set's.
void ModelAnimate(Guest& g, game::Model* model) {
  TrackSet* float4 = &model->tracks[0];
  for (int32_t i = 0; i < int32_t(float4->Count()); i++) {
    TrackSetSampleFloat4(g, float4, i);
    if (float4->HasSection()) {
      TrackSetAdvance(float4, i, 1.0);
    }
  }
  TrackSet* set32 = &model->tracks[3];
  for (int32_t i = 0; i < int32_t(set32->Count()); i++) {
    TrackSetSample32(g, set32, i);
    if (set32->HasSection()) {
      TrackSetAdvance32(set32, i, 1.0);
    }
  }
}

// --- Skeletons ---

// Binds the node animation section (0: none).
void SkeletonCursorView(SkeletonCursor* cursor, uint32_t section) {
  if (section == 0) {
    SkeletonCursorClear(cursor);
    return;
  }
  cursor->section = At<game::AnimationSection>(section);
  cursor->entry_count = At<i32_be>(section + offsetof(game::AnimationSection, track_count));
  cursor->entries = At<SkeletonNodeEntry>(section + offsetof(game::AnimationSection, tracks));
  cursor->start_time = cursor->section->start_time;
  cursor->unknown_16 = 0;
}

void SkeletonBaseConstruct(Guest& g, Skeleton* skeleton) {
  skeleton->vtable = Skeleton::kBaseVtable;
  skeleton->matrix.right.w = 1.0f;
  skeleton->matrix.up.w = 1.0f;
  skeleton->matrix.forward.w = 1.0f;
  skeleton->matrix.position.w = 1.0f;
  g.Call(sub_82587918, &skeleton->node_file);  // Clears the view's first word.
  skeleton->matrix = game::Matrix{{1.0f, 0.0f, 0.0f, 0.0f},
                                  {0.0f, 1.0f, 0.0f, 0.0f},
                                  {0.0f, 0.0f, 1.0f, 0.0f},
                                  {0.0f, 0.0f, 0.0f, 1.0f}};
  skeleton->cursors = nullptr;
  skeleton->own_matrices = 0;
  skeleton->node_states = nullptr;
  skeleton->bone_states = nullptr;
  skeleton->bone_matrices = nullptr;
  skeleton->matrices = 0;
  skeleton->unknown_152 = 0;
  skeleton->root_index = 0;
  skeleton->instance_count = 0;
}

void SkeletonBaseDestruct(Guest& g, Skeleton* skeleton) {
  skeleton->vtable = Skeleton::kBaseVtable;
  SkeletonBaseFree(g, skeleton);
}

void SkeletonFreeNodeValues(Guest& g, Skeleton* skeleton) {
  if (skeleton->node_values.address != 0) {
    DeleteArray(g, skeleton->node_values.address);
  }
  skeleton->node_values = nullptr;
}

// A node channel's keys sampled at `time`, starting from the pair the
// cursor word holds and moving it on (wrapping to the first pair) to the
// pair around the time. Before the first key or past the last, their value.
float CursorCurveSample(Guest& g, uint32_t keys, i32_be* cursor, int32_t count, float time) {
  auto key = [&](int32_t index) { return Element<AnimationKey>(keys, index); };
  if (count <= 1) {
    return key(0)->value;
  }
  const AnimationKey* last = key(count - 1);
  if (!(time < last->time)) {
    return last->value;
  }
  if (!(time > key(0)->time)) {
    return key(0)->value;
  }
  if (*cursor <= 0 || *cursor >= count) {
    *cursor = 1;
  }
  AnimationKey* previous = key(*cursor - 1);
  AnimationKey* next = key(*cursor);
  while (previous->time > time || !(time < next->time)) {
    previous = key(*cursor);
    next = key(*cursor + 1);
    *cursor = *cursor + 1;
    if (*cursor >= count) {
      previous = key(0);
      *cursor = 1;
      next = key(1);
    }
  }
  g.ctx.f1.f64 = double(time);
  g.Call(sub_8258BE78, previous, next);  // Key::Interpolate
  return float(g.ctx.f1.f64);
}

// --- The primitive drawer ---

// Its vertex declaration's elements (position float3, color, texture
// coordinate float2), and the compiler's names.
constexpr uint32_t kPrimitiveElements = 0x8265B04C;
constexpr uint32_t kDeclarationError = 0x8206EFCC;  // "!! CREATE VERTEX DECLARATION ERROR !!"
constexpr uint32_t kEntryPoint = 0x82001BC4;        // "main"
constexpr uint32_t kVertexProfile = 0x82001BCC;     // "vs_3_0"
constexpr uint32_t kPixelProfile = 0x82001BBC;      // "ps_3_0"
constexpr int kPrimitiveVertexShaderCount = 2;
constexpr int kPrimitivePixelShaderCount = 5;

// PrimitiveDrawer::Init's frame (160 bytes): the compiler reads where its
// errors and constant table go from +84 and +92 (its ninth and tenth
// arguments), and writes the buffers at +96 and +100.
constexpr uint32_t kPrimitiveInitFrame = 160;
struct PrimitiveInitFrame {
  uint8_t linkage[84];
  u32_be errors_out;     // Where the error buffer goes.
  uint8_t unknown_88[4];
  u32_be constants_out;  // 0: no constant table.
  u32_be buffers[2];     // The shader's and the errors' buffers (by shader kind).
};
GUEST_OFFSET(PrimitiveInitFrame, errors_out, 84);
GUEST_OFFSET(PrimitiveInitFrame, constants_out, 92);
GUEST_OFFSET(PrimitiveInitFrame, buffers, 96);

// A D3DX buffer's calls (its vtable slots 3 and 2).
uint32_t BufferPointer(Guest& g, uint32_t buffer) { return g.CallVirtual(At<u32_be>(buffer), 3); }
void ReleaseBuffer(Guest& g, uint32_t buffer) { g.CallVirtual(At<u32_be>(buffer), 2); }

// One shader compiled from its HLSL (D3DXCompileShader; answered from the
// stored shaders) and made with `create`; its errors printed when the
// compile fails. The shader's buffer goes to `shader_out`, the errors' to
// `errors_out` (the vertex and pixel loops use the two frame words the other
// way round).
void CompilePrimitiveShader(Guest& g, PrimitiveInitFrame* frame, PrimitiveShader* entry,
                            uint32_t profile, u32_be* shader_out, u32_be* errors_out,
                            Guest::Function* create) {
  frame->constants_out = 0;
  frame->errors_out = AddressOf(errors_out);
  const int32_t result =
      int32_t(g.Call(sub_822B3678, uint32_t(entry->source), uint32_t(entry->length), 0u, 0u,
                     kEntryPoint, profile, 0u, shader_out));  // D3DXCompileShader
  if (result < 0 && *errors_out != 0) {
    g.Call(sub_82293CB8, BufferPointer(g, *errors_out));  // OutputDebugStringA
    ReleaseBuffer(g, *errors_out);
  }
  if (*shader_out == 0) {
    return;  // Never written (see Init): the original would call through it.
  }
  const uint32_t code = BufferPointer(g, *shader_out);
  entry->shader = g.Call(create, code);
  ReleaseBuffer(g, *shader_out);
}

// PrimitiveDrawer::Init (vtable slot 0, once at boot): its vertex
// declaration made, then its two vertex and five pixel shaders.
void PrimitiveDrawerInit(Guest& g, PrimitiveDrawer* drawer) {
  u32_be* declaration = At<u32_be>(drawer->declaration);
  *declaration = g.Call(sub_822A5F28, kPrimitiveElements);  // CreateVertexDeclaration
  if (*declaration == 0) {
    g.Call(sub_82293CB8, kDeclarationError);  // OutputDebugStringA
  }
  guest::StackFrame stack(g.ctx, kPrimitiveInitFrame);
  PrimitiveInitFrame* frame = At<PrimitiveInitFrame>(stack.Local(0));
  // The out words are cleared first: the compiler only writes them, and a
  // failed compile that left one unwritten would have handed the original
  // whatever was on its stack.
  frame->buffers[0] = 0;
  frame->buffers[1] = 0;
  for (int i = 0; i < kPrimitiveVertexShaderCount; i++) {
    CompilePrimitiveShader(g, frame, Element<PrimitiveShader>(kPrimitiveVertexShaders, i),
                           kVertexProfile, &frame->buffers[1], &frame->buffers[0],
                           sub_822A7388);  // VertexShader::Create
  }
  for (int i = 0; i < kPrimitivePixelShaderCount; i++) {
    CompilePrimitiveShader(g, frame, Element<PrimitiveShader>(kPrimitivePixelShaders, i),
                           kPixelProfile, &frame->buffers[0], &frame->buffers[1],
                           sub_822A7250);  // PixelShader::Create
  }
}

// Its vertex shader (0 or 1) and pixel shader (0 to 4) set on the device;
// another choice reads the entry before the table's first, as the game does.
void PrimitiveDrawerSetShaders(Guest& g, uint32_t device, uint32_t vertex, uint32_t pixel) {
  const uint32_t vertex_index = vertex == 0 ? 0 : vertex == 1 ? 1 : uint32_t(-1);
  const uint32_t pixel_index = pixel <= 4 ? pixel : uint32_t(-1);
  g.Call(sub_822A5F00, device,
         Element<PrimitiveShader>(kPrimitiveVertexShaders, vertex_index)->shader);  // SetVertexShader
  g.Call(sub_822A5BB8, device,
         Element<PrimitiveShader>(kPrimitivePixelShaders, pixel_index)->shader);  // SetPixelShader
}

// The vertices drawn inline: its declaration set, room for them asked of
// the device, and copied in.
void PrimitiveDrawerDraw(Guest& g, PrimitiveDrawer* drawer, uint32_t device, uint32_t type,
                         uint32_t vertices, uint32_t count) {
  g.Call(sub_822A5F08, device, *At<u32_be>(drawer->declaration));  // SetVertexDeclaration
  const uint32_t to = g.Call(sub_825A31A0, device, type, count, kPrimitiveVertexBytes);  // BeginVertices
  if (to == 0) {
    return;
  }
  {
    guest::ReturnAddress from(g, kPrimitiveCopyReturn);
    g.Call(sub_8258CB90, to, vertices, count * kPrimitiveVertexBytes);  // memcpy
  }
  g.Call(sub_825A3AE0, device);  // EndVertices
}

}  // namespace

// --- The functions other files of the group call (model_animation.h) ---

namespace game {

void TrackSetConstruct(TrackSet* set) {
  TrackSetClear(set);
  set->values = 0;
}

// The values' frees: with a count header (the 16-, 32- and 208-byte ones)
// or without (the floats).
void TrackSetReleaseArray(Guest& g, TrackSet* set) {
  if (set->values != 0) {
    DeleteCountedArray(g, set->values);
  }
  set->values = 0;
}

void TrackSetRelease(Guest& g, TrackSet* set) {
  if (set->values != 0) {
    DeleteArray(g, set->values);
  }
  set->values = 0;
}

// The set-ups: the old values freed, the section bound, and one value a
// track made, each one's time the section's start time.
void TrackSetSetupFloat4(Guest& g, TrackSet* set, uint32_t section) {
  TrackSetReleaseArray(g, set);
  TrackSetView(set, section);
  const uint32_t count = set->Count();
  if (count == 0) {
    return;
  }
  const uint32_t values = NewCountedArray(g, count, sizeof(TrackValue16), kMaxFloat4Values);
  if (values != 0) {
    for (uint32_t i = 0; i < count; i++) {
      Element<TrackValue16>(values, i)->time = 1.0f;
    }
  }
  set->values = values;
  for (uint32_t i = 0; i < count && set->values != 0; i++) {
    Element<TrackValue16>(set->values, i)->time = TrackSetStartTime(set, -1);
  }
}

void TrackSetSetupFloat(Guest& g, TrackSet* set, uint32_t section) {
  TrackSetRelease(g, set);
  TrackSetView(set, section);
  const uint32_t count = set->Count();
  if (count == 0) {
    return;
  }
  set->values = NewArray(g, count <= kMaxFloatValues ? count * 4 : kRefusedSize);
  for (uint32_t i = 0; i < count && set->values != 0; i++) {
    *Element<guest::f32_be>(set->values, i) = TrackSetStartTime(set, -1);
  }
}

void TrackSetSetup32(Guest& g, TrackSet* set, uint32_t section) {
  TrackSetReleaseArray(g, set);
  TrackSetView(set, section);
  const uint32_t count = set->Count();
  if (count == 0) {
    return;
  }
  const uint32_t values = NewCountedArray(g, count, sizeof(TrackValue32), kMax32Values);
  if (values != 0) {
    for (uint32_t i = 0; i < count; i++) {
      Element<TrackValue32>(values, i)->floats[3] = 1.0f;
    }
  }
  set->values = values;
  for (uint32_t i = 0; i < count && set->values != 0; i++) {
    Element<TrackValue32>(set->values, i)->time = TrackSetStartTime(set, -1);
  }
}

// The 208-byte values: each row's last float 1.0, then for each track its
// time from the set's start time and its own set-up (sub_82587020; both
// recompiled: never seen with tracks).
void TrackSetSetup208(Guest& g, TrackSet* set, uint32_t section) {
  TrackSetReleaseArray(g, set);
  TrackSetView(set, section);
  const uint32_t count = set->Count();
  if (count == 0) {
    return;
  }
  const uint32_t values = NewCountedArray(g, count, sizeof(TrackValue208), kMax208Values);
  if (values != 0) {
    for (uint32_t i = 0; i < count; i++) {
      for (Vector& row : Element<TrackValue208>(values, i)->rows) {
        row.w = 1.0f;
      }
    }
  }
  set->values = values;
  for (int32_t i = 0; i < int32_t(count) && set->values != 0; i++) {
    g.Call(sub_8258BDC8, set, -1);  // TrackSet208::StartTime
    Element<TrackValue208>(set->values, i)->time = float(g.ctx.f1.f64);
    g.Call(sub_82587020, set, i);  // TrackSet208::SetupTrack
  }
}

// Skeleton::Skeleton: the base's construction, no node values yet.
Skeleton* SkeletonConstruct(Guest& g, Skeleton* skeleton) {
  SkeletonBaseConstruct(g, skeleton);
  skeleton->node_values = nullptr;
  skeleton->vtable = Skeleton::kVtable;
  return skeleton;
}

void SkeletonCursorClear(SkeletonCursor* cursor) {
  cursor->start_time = 0.0f;
  cursor->section = nullptr;
  cursor->entry_count = nullptr;
  cursor->entries = nullptr;
  cursor->unknown_16 = 0;
}

// What SkeletonBase::SetUp made: the cursors, the node states, the bones'
// blocks and its own matrices; each freed when set, then cleared.
void SkeletonBaseFree(Guest& g, Skeleton* skeleton) {
  if (skeleton->cursors.address != 0) {
    DeleteArray(g, skeleton->cursors.address);
  }
  if (skeleton->node_states.address != 0) {
    DeleteCountedArray(g, skeleton->node_states.address);
  }
  if (skeleton->bone_matrices.address != 0) {
    DeleteCountedArray(g, skeleton->bone_matrices.address);
  }
  if (skeleton->own_matrices != 0) {
    DeleteCountedArray(g, skeleton->own_matrices);
  }
  skeleton->cursors = nullptr;
  skeleton->node_states = nullptr;
  skeleton->bone_matrices = nullptr;
  skeleton->own_matrices = 0;
}

void SkeletonDestruct(Guest& g, Skeleton* skeleton) {
  skeleton->vtable = Skeleton::kVtable;
  SkeletonFreeNodeValues(g, skeleton);
  SkeletonBaseDestruct(g, skeleton);
}

uint32_t SkeletonNodeCount(Skeleton* skeleton) {
  if (skeleton->node_values.address == 0) {
    return 0;
  }
  return uint32_t(int32_t(skeleton->node_file.file->node_count));
}

// Instance 0's cursor bound to the node animation section, and each entry's
// node value set from it: x and z its time, y its +16.
void SkeletonLoadNodeValues(Skeleton* skeleton, uint32_t section) {
  if (skeleton->node_values.address == 0) {
    return;
  }
  SkeletonCursorView(skeleton->cursors.get(), section);
  if (!skeleton->cursors->HasSection()) {
    return;
  }
  SkeletonNodeEntry* entry = skeleton->cursors->entries.get();
  for (int32_t i = 0; i < *skeleton->cursors->entry_count; i++, entry++) {
    Vector& value = *skeleton->Value(entry->node);
    const float time = entry->time;
    value.x = time;
    value.y = entry->unknown_16;
    value.z = time;
  }
}

// Each entry of instance 0's cursor: its node's channels sampled at the
// node's time (its value's z) into the node's state.
void SkeletonApplyAnimation(Guest& g, Skeleton* skeleton) {
  SkeletonCursor* cursor = skeleton->cursors.get();
  if (!cursor->HasSection()) {
    return;
  }
  SkeletonNodeEntry* entry = cursor->entries.get();
  for (int32_t i = 0; i < *skeleton->cursors->entry_count; i++, entry++) {
    cursor = skeleton->cursors.get();
    const uint32_t channels_offset = Element<SkeletonNodeEntry>(cursor->entries.address, i)->channels_offset;
    const float time = skeleton->Value(entry->node)->z;
    NodeState& state = *skeleton->State(entry->node);
    AnimationChannel* channel = At<AnimationChannel>(cursor->section.address + channels_offset);
    for (int32_t c = 0; c < entry->channel_count; c++) {
      const uint32_t component = channel->component;
      state.Float(component) = CursorCurveSample(g, AddressOf(channel->keys), state.Cursor(component),
                                                 int32_t(channel->key_count), time);
      channel = channel->Next();
    }
  }
}

}  // namespace game

#define SBX_ANIMATION_FALLBACK(name)   \
  REX_FUNC_PROLOGUE();                 \
  if (!On()) {                         \
    __imp__##name(ctx, base);          \
    return;                            \
  }                                    \
  Guest g(ctx, base);

// --- Track sets ---

// TrackSet::TrackSet: r3 the set. Returns it.
extern "C" REX_FUNC(sub_825866D8) {
  SBX_ANIMATION_FALLBACK(sub_825866D8)
  game::TrackSetConstruct(At<TrackSet>(ctx.r3.u32));
}

// TrackSet::Clear: r3 the set. Returns it.
extern "C" REX_FUNC(sub_82587638) {
  SBX_ANIMATION_FALLBACK(sub_82587638)
  TrackSetClear(At<TrackSet>(ctx.r3.u32));
}

// TrackSet::View: r3 the set, r4 a section or 0. Returns the set.
extern "C" REX_FUNC(sub_825876E0) {
  SBX_ANIMATION_FALLBACK(sub_825876E0)
  TrackSetView(At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

// TrackSet::Count: r3 the set. Its track count, 0 with no section.
extern "C" REX_FUNC(sub_82587658) {
  SBX_ANIMATION_FALLBACK(sub_82587658)
  ctx.r3.u64 = At<TrackSet>(ctx.r3.u32)->Count();
}

// TrackSet::StartTime: r3 the set, r4 a track or -1. f1 its start time.
extern "C" REX_FUNC(sub_8258BE30) {
  SBX_ANIMATION_FALLBACK(sub_8258BE30)
  ctx.f1.f64 = double(TrackSetStartTime(At<TrackSet>(ctx.r3.u32), ctx.r4.s32));
}

// TrackSet::ReleaseArray and Release: r3 the set; its values freed.
extern "C" REX_FUNC(sub_82587120) {
  SBX_ANIMATION_FALLBACK(sub_82587120)
  game::TrackSetReleaseArray(g, At<TrackSet>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_825872B8) {
  SBX_ANIMATION_FALLBACK(sub_825872B8)
  game::TrackSetRelease(g, At<TrackSet>(ctx.r3.u32));
}

// TrackSet::SetupFloat4, SetupFloat, Setup32 and Setup208: r3 the set, r4
// the part file's section.
extern "C" REX_FUNC(sub_82587538) {
  SBX_ANIMATION_FALLBACK(sub_82587538)
  game::TrackSetSetupFloat4(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_82587318) {
  SBX_ANIMATION_FALLBACK(sub_82587318)
  game::TrackSetSetupFloat(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_82586900) {
  SBX_ANIMATION_FALLBACK(sub_82586900)
  game::TrackSetSetup32(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_82587180) {
  SBX_ANIMATION_FALLBACK(sub_82587180)
  game::TrackSetSetup208(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

// TrackSet::SampleFloat4 and Sample32: r3 the set, r4 a track.
extern "C" REX_FUNC(sub_825873B8) {
  SBX_ANIMATION_FALLBACK(sub_825873B8)
  TrackSetSampleFloat4(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_82586710) {
  SBX_ANIMATION_FALLBACK(sub_82586710)
  TrackSetSample32(g, At<TrackSet>(ctx.r3.u32), ctx.r4.u32);
}

// TrackSet::Advance and Advance32: r3 the set, r4 a track, f1 the step. 1
// when the time looped.
extern "C" REX_FUNC(sub_82587498) {
  SBX_ANIMATION_FALLBACK(sub_82587498)
  TrackSet* set = At<TrackSet>(ctx.r3.u32);
  ctx.r3.u64 = set->HasSection() && TrackSetAdvance(set, ctx.r4.u32, ctx.f1.f64) ? 1 : 0;
}

extern "C" REX_FUNC(sub_825867D8) {
  SBX_ANIMATION_FALLBACK(sub_825867D8)
  TrackSet* set = At<TrackSet>(ctx.r3.u32);
  ctx.r3.u64 = set->HasSection() && TrackSetAdvance32(set, ctx.r4.u32, ctx.f1.f64) ? 1 : 0;
}

// Model::Animate (sub_8257A460): r3 the model. SBX's own wrapper keeps the
// address (subtick/per_frame_logic.cpp, for the render-only passes' times),
// so it's no override here: the wrapper calls this in place of the
// recompiled function.
namespace hooks {
void NativeModelAnimation_Animate(PPCContext& ctx, uint8_t* base);
}

void hooks::NativeModelAnimation_Animate(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8257A460(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ModelAnimate(g, At<game::Model>(ctx.r3.u32));
}

// --- Skeletons ---

// SkeletonBase::SkeletonBase: r3 the skeleton. Returns it.
extern "C" REX_FUNC(sub_8257F918) {
  SBX_ANIMATION_FALLBACK(sub_8257F918)
  Skeleton* skeleton = At<Skeleton>(ctx.r3.u32);
  SkeletonBaseConstruct(g, skeleton);
  ctx.r3.u64 = AddressOf(skeleton);
}

// Skeleton::Skeleton: r3 the skeleton. Returns it.
extern "C" REX_FUNC(sub_8257EEF0) {
  SBX_ANIMATION_FALLBACK(sub_8257EEF0)
  ctx.r3.u64 = AddressOf(game::SkeletonConstruct(g, At<Skeleton>(ctx.r3.u32)));
}

// Skeleton::~Skeleton and SkeletonBase::~SkeletonBase: r3 the skeleton.
extern "C" REX_FUNC(sub_8257F180) {
  SBX_ANIMATION_FALLBACK(sub_8257F180)
  game::SkeletonDestruct(g, At<Skeleton>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_82580088) {
  SBX_ANIMATION_FALLBACK(sub_82580088)
  SkeletonBaseDestruct(g, At<Skeleton>(ctx.r3.u32));
}

// SkeletonBase::Free: r3 the skeleton; what SetUp made freed.
extern "C" REX_FUNC(sub_8257FA00) {
  SBX_ANIMATION_FALLBACK(sub_8257FA00)
  game::SkeletonBaseFree(g, At<Skeleton>(ctx.r3.u32));
}

// Skeleton::FreeNodeValues: r3 the skeleton.
extern "C" REX_FUNC(sub_8257EF38) {
  SBX_ANIMATION_FALLBACK(sub_8257EF38)
  SkeletonFreeNodeValues(g, At<Skeleton>(ctx.r3.u32));
}

// Skeleton::LoadNodeValues: r3 the skeleton, r4 the part file's node
// animation section.
extern "C" REX_FUNC(sub_8257EF98) {
  SBX_ANIMATION_FALLBACK(sub_8257EF98)
  game::SkeletonLoadNodeValues(At<Skeleton>(ctx.r3.u32), ctx.r4.u32);
}

// SkeletonCursor::Clear: r3 the cursor. Returns it.
extern "C" REX_FUNC(sub_825877D8) {
  SBX_ANIMATION_FALLBACK(sub_825877D8)
  game::SkeletonCursorClear(At<SkeletonCursor>(ctx.r3.u32));
}

// SkeletonCursor::View: r3 the cursor, r4 a section or 0. Returns the cursor.
extern "C" REX_FUNC(sub_82587800) {
  SBX_ANIMATION_FALLBACK(sub_82587800)
  SkeletonCursorView(At<SkeletonCursor>(ctx.r3.u32), ctx.r4.u32);
}

// Skeleton::NodeCount: r3 the skeleton. 0 without node values.
extern "C" REX_FUNC(sub_8257F038) {
  SBX_ANIMATION_FALLBACK(sub_8257F038)
  ctx.r3.u64 = game::SkeletonNodeCount(At<Skeleton>(ctx.r3.u32));
}

// Skeleton::UpdateMatrices: r3 the skeleton; with node values, each node's
// matrix made from its parent's (Skeleton::UpdateNodeMatrices).
extern "C" REX_FUNC(sub_8257F058) {
  SBX_ANIMATION_FALLBACK(sub_8257F058)
  Skeleton* skeleton = At<Skeleton>(ctx.r3.u32);
  if (skeleton->node_values.address != 0) {
    game::SkeletonUpdateNodeMatrices(g, skeleton);
  }
}

// Skeleton::ApplyAnimation: r3 the skeleton.
extern "C" REX_FUNC(sub_8257F070) {
  SBX_ANIMATION_FALLBACK(sub_8257F070)
  game::SkeletonApplyAnimation(g, At<Skeleton>(ctx.r3.u32));
}

// CursorCurve::Sample: r4 the keys, r5 the channel's cursor word, r6 the key
// count, f1 the time. f1 the value.
extern "C" REX_FUNC(sub_82587858) {
  SBX_ANIMATION_FALLBACK(sub_82587858)
  const float time = float(ctx.f1.f64);
  const float value = CursorCurveSample(g, ctx.r4.u32, At<i32_be>(ctx.r5.u32), ctx.r6.s32, time);
  ctx.f1.f64 = double(value);
}

// --- Beside them in the library ---

// PrimitiveDrawer::PrimitiveDrawer: r3 the drawer; +4 a 4-byte block for
// its vertex declaration. Returns it.
extern "C" REX_FUNC(sub_82586A60) {
  SBX_ANIMATION_FALLBACK(sub_82586A60)
  PrimitiveDrawer* drawer = At<PrimitiveDrawer>(ctx.r3.u32);
  drawer->vtable = PrimitiveDrawer::kVtable;
  drawer->declaration = g.Call(sub_8258C000, 4u);  // operator new
  ctx.r3.u64 = AddressOf(drawer);
}

// PrimitiveDrawer::Init (vtable slot 0): r3 the drawer.
extern "C" REX_FUNC(sub_82586B20) {
  SBX_ANIMATION_FALLBACK(sub_82586B20)
  PrimitiveDrawerInit(g, At<PrimitiveDrawer>(ctx.r3.u32));
}

// PrimitiveDrawer::SetShaders (vtable slot 2): r3 the drawer, r4 the
// device, r5 the vertex shader, r6 the pixel shader.
extern "C" REX_FUNC(sub_82586D98) {
  SBX_ANIMATION_FALLBACK(sub_82586D98)
  PrimitiveDrawerSetShaders(g, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
}

// PrimitiveDrawer::Draw (vtable slot 3): r3 the drawer, r4 the device, r5
// the primitive type, r6 the vertices, r7 their count.
extern "C" REX_FUNC(sub_82586E88) {
  SBX_ANIMATION_FALLBACK(sub_82586E88)
  PrimitiveDrawer* drawer = At<PrimitiveDrawer>(ctx.r3.u32);
  const uint32_t device = ctx.r4.u32;
  const uint32_t type = ctx.r5.u32;
  const uint32_t vertices = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  PrimitiveDrawerDraw(g, drawer, device, type, vertices, count);
}

// RecordClear: r3 a 12-byte record. Returns it.
extern "C" REX_FUNC(sub_82586FC0) {
  SBX_ANIMATION_FALLBACK(sub_82586FC0)
  PadRecord* record = At<PadRecord>(ctx.r3.u32);
  record->unknown_0 = 0.0f;
  record->unknown_4[0] = 0;
  record->unknown_4[1] = 0;
}
