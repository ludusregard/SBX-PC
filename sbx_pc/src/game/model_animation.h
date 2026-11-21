// The models' animation data: the track sets a model holds for its part
// file's animation sections (the sections, their tracks, channels and keys,
// and the values the sets animate), and the skeleton built over a node file
// (its node records, the per-node states and matrices, and the instances'
// cursors). All of it sits in guest memory, read by recompiled code too.

#pragma once

#include "game/guest_types.h"
#include "game/math.h"

#include <cstdint>

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// --- Animation sections (part file data) ---

// A key of a channel's curve.
struct AnimationKey {
  f32_be time;
  u32_be flags;  // How it's interpolated to the next key.
  f32_be value;
  f32_be in_tangent;
  f32_be out_tangent;
};
GUEST_SIZE(AnimationKey, 20);

// A channel: which float of the value it writes, and its keys. The next
// channel follows its last key.
struct AnimationChannel {
  u32_be key_count;
  u32_be component;  // The index of the float it writes in the value.
  AnimationKey keys[1];

  AnimationKey* Key(uint32_t index) { return guest::Element<AnimationKey>(guest::AddressOf(keys), index); }
  AnimationChannel* Next() { return reinterpret_cast<AnimationChannel*>(Key(key_count)); }
};
GUEST_OFFSET(AnimationChannel, keys, 8);

// A section's track: its channels (an offset from the section) and the
// times its value loops between.
struct AnimationTrack {
  uint8_t unknown_0[64];
  u32_be channel_count;
  u32_be channels_offset;
  f32_be start_time;
  f32_be end_time;
};
GUEST_SIZE(AnimationTrack, 80);

// An animation section: the default times (for track -1), then the tracks.
struct AnimationSection {
  u32_be unknown_0;
  f32_be start_time;
  f32_be end_time;
  u32_be unknown_12;
  u32_be track_count;
  AnimationTrack tracks[1];
};
GUEST_OFFSET(AnimationSection, start_time, 4);
GUEST_OFFSET(AnimationSection, track_count, 16);
GUEST_OFFSET(AnimationSection, tracks, 20);

// --- Track sets (a model's +304 to +384) ---

// A track set (a model's five, Model::tracks): the section it views and one
// value a track.
struct TrackSet {
  ptr<AnimationSection> section;  // 0: none.
  ptr<u32_be> track_count;        // The section's track count.
  ptr<AnimationTrack> tracks;
  f32_be start_time;  // The section's default start time.
  u32_be values;      // The values' address (one a track).

  bool HasSection() const { return section.address != 0; }
  uint32_t Count() const { return HasSection() ? uint32_t(*track_count.get()) : 0; }
  AnimationTrack& Track(uint32_t index) { return *guest::Element<AnimationTrack>(tracks.address, index); }
  AnimationChannel* Channels(uint32_t index) {
    return guest::At<AnimationChannel>(section.address + Track(index).channels_offset);
  }
};
GUEST_SIZE(TrackSet, 20);
GUEST_OFFSET(TrackSet, start_time, 12);
GUEST_OFFSET(TrackSet, values, 16);

// The values the set-ups allocate. Channels write their value's floats by
// index (the component), so each has a float view of itself.

// The +304 set's (16 bytes, after a count header): x, y, z and the time.
struct TrackValue16 {
  f32_be x, y, z;
  f32_be time;

  f32_be& Float(uint32_t component) {
    return *guest::Element<f32_be>(guest::AddressOf(this), component);
  }
};
GUEST_SIZE(TrackValue16, 16);

// The +364 set's (32 bytes, after a count header): four floats, the time,
// and a mask of the components the channels wrote.
struct TrackValue32 {
  f32_be floats[4];
  f32_be time;
  u32_be written;
  uint8_t unknown_24[8];

  f32_be& Float(uint32_t component) {
    return *guest::Element<f32_be>(guest::AddressOf(this), component);
  }
};
GUEST_SIZE(TrackValue32, 32);
GUEST_OFFSET(TrackValue32, time, 16);
GUEST_OFFSET(TrackValue32, written, 20);

// The +384 set's (208 bytes, after a count header): eleven 16-byte rows
// whose last floats start at 1.0, and a time.
struct TrackValue208 {
  Vector rows[11];
  uint8_t unknown_176[20];
  f32_be time;
  uint8_t unknown_200[8];
};
GUEST_SIZE(TrackValue208, 208);
GUEST_OFFSET(TrackValue208, time, 196);

// The header operator new[] puts before the 16-, 32- and 208-byte values.
struct ValueArrayHeader {
  u32_be count;
  uint8_t unknown_4[12];
};
GUEST_SIZE(ValueArrayHeader, 16);

// --- The skeleton ---

// A node file: its counts, then its sections' offsets from the file.
struct NodeFile {
  u32_be unknown_0;
  i32_be node_count;
  i32_be bone_count;
  u32_be first_bone;  // The first bone's node.
  u32_be offsets[9];  // +16 to +48.
};
GUEST_SIZE(NodeFile, 52);

// A node's record in the file.
struct NodeRecord {
  static constexpr uint32_t kRestExtra = 0x40;  // ResetPose copies `extra`.
  static constexpr uint32_t kBone = 0x80;
  static constexpr int32_t kLastPlainKind = 4;
  static constexpr int32_t kKind8 = 8;    // Its matrix made by sub_82580E48.
  static constexpr int32_t kKind16 = 16;  // Its matrix made by sub_82581540.

  uint8_t unknown_0[64];
  Vector extra;
  uint8_t unknown_80[12];
  i32_be translation_source;  // The node whose translation it takes; -1 none.
  uint8_t unknown_96[32];
  i32_be parent;  // -1 for none.
  i32_be kind;    // 0 to 4 plain nodes, 8 and 16 special; bits 0x40, 0x80.
  uint8_t unknown_136[24];
};
GUEST_SIZE(NodeRecord, 160);
GUEST_OFFSET(NodeRecord, extra, 64);
GUEST_OFFSET(NodeRecord, translation_source, 92);
GUEST_OFFSET(NodeRecord, parent, 128);
GUEST_OFFSET(NodeRecord, kind, 132);

// A node file's view (NodeFile::View): its sections as addresses.
struct NodeFileView {
  ptr<NodeFile> file;
  ptr<NodeRecord> nodes;
  u32_be translations;  // A Vector a node.
  u32_be angles;        // A Vector a node.
  u32_be scales;        // A Vector a node.
  u32_be unknown_20;
  u32_be matrices;  // A Matrix a node.
  u32_be unknown_28;
  u32_be unknown_32;
  u32_be unknown_36;  // unknown_32 + 16.
  u32_be unknown_40;
  u32_be node_minus_bone_count;
};
GUEST_SIZE(NodeFileView, 48);

// A node's state: its pose, and a key cursor for each channel that
// animates it.
struct NodeState {
  Vector translation;
  Vector angles;
  Vector scale;
  Vector extra;
  i32_be channel_cursors[16];  // One for each float of the state above.
  uint8_t unknown_128[4];
  u32_be unknown_132;
  uint8_t unknown_136[8];

  // The key cursor of the channel writing float `component`.
  i32_be* Cursor(uint32_t component) {
    return guest::Element<i32_be>(guest::AddressOf(channel_cursors), component);
  }
  f32_be& Float(uint32_t component) {
    return *guest::Element<f32_be>(guest::AddressOf(this), component);
  }
};
GUEST_SIZE(NodeState, 144);
GUEST_OFFSET(NodeState, channel_cursors, 64);
GUEST_OFFSET(NodeState, unknown_132, 132);

// A bone's matrix block (64 bytes, SetUp's +144).
struct BoneMatrix {
  f32_be unknown_0[3];
  f32_be one_12;
  f32_be unknown_16[3];
  f32_be one_28;
  f32_be unknown_32[3];
  f32_be one_44;
  f32_be unknown_48;
  u32_be unknown_52;  // 1 after SetUp.
  uint8_t unknown_56[8];
};
GUEST_SIZE(BoneMatrix, 64);

// An entry of the node animation section: a node's channels and times.
struct SkeletonNodeEntry {
  u32_be node;
  i32_be channel_count;
  u32_be channels_offset;  // From the section.
  f32_be time;
  f32_be unknown_16;
};
GUEST_SIZE(SkeletonNodeEntry, 20);

// An instance's cursor over the node animation section (a track set's
// layout with +16 = 0).
struct SkeletonCursor {
  ptr<AnimationSection> section;
  ptr<i32_be> entry_count;
  ptr<SkeletonNodeEntry> entries;
  f32_be start_time;
  u32_be unknown_16;

  bool HasSection() const { return section.address != 0; }
};
GUEST_SIZE(SkeletonCursor, 20);

// A skeleton (a model's +112): the base class SkeletonBase and the skeleton
// are one object.
struct Skeleton {
  static constexpr uint32_t kVtable = 0x82069C30;
  static constexpr uint32_t kBaseVtable = 0x82069C40;

  u32_be vtable;
  uint8_t unknown_4[12];
  Matrix matrix;  // The root's matrix is multiplied by it.
  NodeFileView node_file;
  ptr<SkeletonCursor> cursors;  // One an instance.
  u32_be own_matrices;          // A Matrix a node (count header), when none were given.
  ptr<NodeState> node_states;   // One a node (count header).
  ptr<NodeState> bone_states;   // The first bone's state.
  ptr<BoneMatrix> bone_matrices;  // One a bone (count header).
  u32_be matrices;                // The nodes' matrices: given, or own_matrices.
  u32_be unknown_152;
  u32_be root_index;
  i32_be instance_count;
  uint8_t unknown_164[12];
  ptr<Vector> node_values;  // A Vector a node; z the node's animation time.

  Matrix* MatrixAt(uint32_t index) { return guest::Element<Matrix>(matrices, index); }
  NodeRecord* Node(uint32_t index) { return guest::Element<NodeRecord>(node_file.nodes.address, index); }
  NodeState* State(uint32_t index) { return guest::Element<NodeState>(node_states.address, index); }
  Vector* Value(uint32_t index) { return guest::Element<Vector>(node_values.address, index); }
};
GUEST_SIZE(Skeleton, 180);
GUEST_OFFSET(Skeleton, matrix, 16);
GUEST_OFFSET(Skeleton, node_file, 80);
GUEST_OFFSET(Skeleton, cursors, 128);
GUEST_OFFSET(Skeleton, matrices, 148);
GUEST_OFFSET(Skeleton, instance_count, 160);
GUEST_OFFSET(Skeleton, node_values, 176);

// The primitive drawer (vtable 0x8206EFB4): +4 its vertex declaration's
// block.
struct PrimitiveDrawer {
  static constexpr uint32_t kVtable = 0x8206EFB4;

  u32_be vtable;
  u32_be declaration;
};
GUEST_SIZE(PrimitiveDrawer, 8);

// --- The functions other files of the group call (model_animation.cpp) ---

// The track sets: construction, the values' frees (with a count header or
// without), and the set-ups from a part file's section (the old values
// freed, the section bound, one value a track).
void TrackSetConstruct(TrackSet* set);                         // sub_825866D8
void TrackSetReleaseArray(guest::Guest& g, TrackSet* set);     // sub_82587120
void TrackSetRelease(guest::Guest& g, TrackSet* set);          // sub_825872B8
void TrackSetSetupFloat4(guest::Guest& g, TrackSet* set, uint32_t section);  // sub_82587538
void TrackSetSetupFloat(guest::Guest& g, TrackSet* set, uint32_t section);   // sub_82587318
void TrackSetSetup32(guest::Guest& g, TrackSet* set, uint32_t section);      // sub_82586900
void TrackSetSetup208(guest::Guest& g, TrackSet* set, uint32_t section);     // sub_82587180

// The skeleton: construction and destruction, SkeletonBase::Free, its node
// values (their count and their load from the node animation section) and
// the node channels applied; an instance's cursor cleared.
Skeleton* SkeletonConstruct(guest::Guest& g, Skeleton* skeleton);  // sub_8257EEF0
void SkeletonDestruct(guest::Guest& g, Skeleton* skeleton);        // sub_8257F180
void SkeletonBaseFree(guest::Guest& g, Skeleton* skeleton);        // sub_8257FA00
uint32_t SkeletonNodeCount(Skeleton* skeleton);                    // sub_8257F038
void SkeletonLoadNodeValues(Skeleton* skeleton, uint32_t section);  // sub_8257EF98
void SkeletonApplyAnimation(guest::Guest& g, Skeleton* skeleton);   // sub_8257F070
void SkeletonCursorClear(SkeletonCursor* cursor);                   // sub_825877D8

}  // namespace game
