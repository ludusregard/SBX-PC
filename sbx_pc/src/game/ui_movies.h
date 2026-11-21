// A UI movie: its movie file and the file's tables, its animations, the
// tracks and keys that move its nodes, its animation queue and looping
// list, its named animations, and the table of loaded movies. The scene's
// nodes and texts are in ui_movie_nodes.h, the draw context in
// ui_movie_draw.h.

#pragma once

#include "game/fixed_list.h"
#include "game/guest_types.h"
#include "game/ui_movie_draw.h"
#include "game/ui_movie_nodes.h"

#include <bit>

namespace game {

using guest::f32_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The file's 8-byte table entries: a pointer once the file is fixed up (an
// offset before).
struct MovieEntry {
  u32_be value;
  uint8_t unknown_4[4];
};
GUEST_SIZE(MovieEntry, 8);

// A movie file's chunk (the file's header is one too): a tag and the
// offset to the next. The nodes (MovieNode) are its scene's chunks.
struct MovieChunk {
  u32_be tag;
  u32_be next;
};
GUEST_SIZE(MovieChunk, 8);

// A movie file ("X2D", 0, "005", 0 at +8): its scene at +[+4], its table
// of 8-byte entries at +32, the animations a run of the table's entries.
struct MovieFile {
  uint8_t unknown_0[4];
  u32_be scene;  // Offset from the file's start.
  uint8_t magic[8];
  u32_be first_animation;  // In the table.
  u32_be animation_count;
  u32_be entry_count;
  uint8_t unknown_28[4];
  MovieEntry entries[1];

  MovieEntry* EntryAt(uint32_t index) {
    return guest::Element<MovieEntry>(guest::AddressOf(entries), index);
  }
  // MovieFile::Animation (sub_825865D0): the file's animation `index`.
  uint32_t Animation(uint32_t index) { return EntryAt(first_animation + index)->value; }
};
GUEST_OFFSET(MovieFile, scene, 4);
GUEST_OFFSET(MovieFile, magic, 8);
GUEST_OFFSET(MovieFile, first_animation, 16);
GUEST_OFFSET(MovieFile, entry_count, 24);
GUEST_OFFSET(MovieFile, entries, 32);

// An XMD file (the movie's archive: its X2D file, then its texture set):
// "XMD", 0, "001", 0, then a count and that many offsets from its start.
struct XmdFile {
  uint8_t magic[8];
  u32_be flags;
  u32_be count;
  u32_be offsets[1];  // count of them.

  uint32_t OffsetAt(uint32_t index) {
    return *guest::Element<u32_be>(guest::AddressOf(offsets), index);
  }
};
GUEST_OFFSET(XmdFile, count, 12);
GUEST_OFFSET(XmdFile, offsets, 16);

// A track's key (32 bytes in its key list; the curves read the first 20):
// how it blends to the next (0 a step, 1 a line, 2 and 3 curves), its time,
// its value (a float's or an integer's bits) and a curve's two values.
struct MovieTrackKey {
  u32_be blend;
  f32_be time;
  u32_be value;
  f32_be curve_12;
  f32_be curve_16;

  float FloatValue() const { return std::bit_cast<float>(uint32_t(value)); }
  int32_t IntValue() const { return int32_t(uint32_t(value)); }
};
GUEST_SIZE(MovieTrackKey, 20);
GUEST_OFFSET(MovieTrackKey, time, 4);

// A key list: its count, then 32-byte keys from +16.
struct MovieKeyList {
  uint8_t unknown_0[12];
  u32_be count;

  // Track::Key (sub_8258BCD8).
  MovieTrackKey* Key(uint32_t index) {
    return guest::At<MovieTrackKey>(guest::AddressOf(this) + 32 * index + 16);
  }
};
GUEST_OFFSET(MovieKeyList, count, 12);

// A track: the node property it moves (UiMovie::TrackProperty's id), its
// key list's index, the time its keys count from, and the index in the
// node's vertices for a vertex's property.
struct MovieTrack {
  u32_be property;
  u32_be key_list;
  f32_be start;
  u32_be index;
};
GUEST_SIZE(MovieTrack, 16);

// A node track list: the tracks of the nodes with its id, 16 bytes each
// after its header.
struct MovieNodeTrackList {
  uint8_t unknown_0[8];
  u32_be id;
  u32_be track_count;

  // Animation::NodeTrack (sub_8258BCC8).
  MovieTrack* Track(uint32_t index) {
    return guest::At<MovieTrack>(guest::AddressOf(this) + 16 * (index + 1));
  }
};
GUEST_OFFSET(MovieNodeTrackList, track_count, 12);

// A sub-animation: its animation (an index into the file's animations until
// fixed up) from `start` to `end` of the parent's time, mapped onto its own
// `from` to `to`, on the node (node id 0) or on the node's children with
// its node id.
struct MovieSubAnimation {
  uint8_t unknown_0[8];
  u32_be node_id;
  uint8_t unknown_12[4];
  u32_be animation;
  uint8_t unknown_20[4];
  f32_be start;
  f32_be end;
  f32_be from;
  f32_be to;
};
GUEST_OFFSET(MovieSubAnimation, node_id, 8);
GUEST_OFFSET(MovieSubAnimation, animation, 16);
GUEST_OFFSET(MovieSubAnimation, start, 24);
GUEST_OFFSET(MovieSubAnimation, to, 36);

// An animation: its length, and its node track lists, key lists and
// sub-animations as runs of its 8-byte entries (after +64, offsets from the
// animation until fixed up).
struct MovieAnimation {
  uint8_t unknown_0[12];
  f32_be length;
  uint8_t unknown_16[16];
  u32_be first_node_track_list;
  u32_be node_track_list_count;
  u32_be first_key_list;
  uint8_t unknown_44[4];
  u32_be first_sub_animation;
  u32_be sub_animation_count;
  u32_be entry_count;
  uint8_t unknown_60[4];
  MovieEntry entries[1];

  MovieEntry* EntryAt(uint32_t index) {
    return guest::Element<MovieEntry>(guest::AddressOf(entries), index);
  }
  // Animation::NodeTrackList, KeyList and SubAnimation (sub_8258BCE8,
  // sub_8258BD00, sub_8258BD18).
  MovieNodeTrackList* NodeTrackList(uint32_t index) {
    return guest::At<MovieNodeTrackList>(EntryAt(first_node_track_list + index)->value);
  }
  MovieKeyList* KeyList(uint32_t index) {
    return guest::At<MovieKeyList>(EntryAt(first_key_list + index)->value);
  }
  MovieSubAnimation* SubAnimation(uint32_t index) {
    return guest::At<MovieSubAnimation>(EntryAt(first_sub_animation + index)->value);
  }
};
GUEST_OFFSET(MovieAnimation, length, 12);
GUEST_OFFSET(MovieAnimation, first_node_track_list, 32);
GUEST_OFFSET(MovieAnimation, first_key_list, 40);
GUEST_OFFSET(MovieAnimation, first_sub_animation, 48);
GUEST_OFFSET(MovieAnimation, entry_count, 56);
GUEST_OFFSET(MovieAnimation, entries, 64);

// A node property's descriptor (UiMovie::TrackProperty's table, 64 of
// them): where the value is and how a track writes it.
struct MovieTrackProperty {
  enum Type : uint32_t { kFloat = 0, kSigned = 1, kUnsigned = 2 };  // 3 and up: none.

  u32_be offset;  // In the node (or in its vertex, when in_array).
  u32_be type;
  uint8_t set;       // Set, not added to.
  uint8_t in_array;  // A vertex's: in the node's vertex `index`.
  uint8_t unknown_10[2];
};
GUEST_SIZE(MovieTrackProperty, 12);
GUEST_OFFSET(MovieTrackProperty, set, 8);

// A running animation: which one (by its index in a scene and the scene's
// index, -1 for the live scene, or by its named animation's index, -1 when
// started by scene and index), the node it moves, its time and how it runs.
struct AnimationSlot {
  i32_be index;        // Its animation's index in its scene.
  i32_be scene_index;  // A sub-scene's index, or -1.
  i32_be named_index;  // A named animation's index, or -1.
  ptr<MovieGroup> target;  // The node (a scene) it moves.
  ptr<MovieAnimation> animation;
  f32_be time;
  u32_be with_next;      // The next in the queue runs with it.
  u32_be backwards;
  u32_be remove_at_end;  // Taken off the queue when it ends.
  u32_be hold;           // Its time is held.
};
GUEST_SIZE(AnimationSlot, 40);
GUEST_OFFSET(AnimationSlot, target, 12);
GUEST_OFFSET(AnimationSlot, time, 20);

static_assert(sizeof(AnimationList) == 1420);

// One of the movie's 1,024 named animations (36 bytes; made by
// sub_8211D3D0, destroyed by sub_820913F0): its name, "scene.animation"
// (a std::string), the scene it moves and the animation.
struct NamedAnimation {
  uint8_t name[28];
  u32_be scene;
  ptr<MovieAnimation> animation;
};
GUEST_SIZE(NamedAnimation, 36);

// A UI movie (40,976 bytes, made by UiMovie::UiMovie).
struct UiMovie {
  static constexpr int kNamedAnimations = 1024;

  u32_be file_copies[3];   // The movie file's three copies (VirtualAlloc).
  u32_be texture_set;      // An XPR (24 bytes), or 0.
  ptr<MovieGroup> live;    // The scene drawn and animated (in copy 0).
  ptr<MovieGroup> saved;   // The live pose before the update (in copy 1).
  ptr<MovieGroup> file;    // The pose as the file has it (in copy 2).
  MovieWords textures;     // MovieTexture pointers.
  MovieTextList texts;     // The texts queued by the draw.
  uint8_t unknown_52[12];
  MovieDrawContext draw_context;
  NamedAnimation named[kNamedAnimations];
  u32_be named_count;
  AnimationList queue;
  AnimationList looping;
  MovieTextMap text_map;
  u32_be loaded;  // Its file has loaded (UiMovie::OnLoaded).
  u32_be id;      // Its key in the movie table.
  AnimationSlot last_started;  // A copy of the last animation started.
  uint8_t unknown_40960[16];
};
GUEST_OFFSET(UiMovie, live, 16);
GUEST_OFFSET(UiMovie, textures, 28);
GUEST_OFFSET(UiMovie, texts, 40);
GUEST_OFFSET(UiMovie, draw_context, 64);
GUEST_OFFSET(UiMovie, named, 176);
GUEST_OFFSET(UiMovie, named_count, 37040);
GUEST_OFFSET(UiMovie, queue, 37044);
GUEST_OFFSET(UiMovie, looping, 38464);
GUEST_OFFSET(UiMovie, text_map, 39884);
GUEST_OFFSET(UiMovie, loaded, 40912);
GUEST_OFFSET(UiMovie, last_started, 40920);
GUEST_SIZE(UiMovie, 40976);

// A movie by its address, 0 included: the originals read a missing movie's
// words at their offsets from address 0.
inline UiMovie* MovieAt(uint32_t address) {
  return reinterpret_cast<UiMovie*>(guest::Host(address));
}

// The table of loaded movies (writable data): a MoviePairs of {id, movie}.
constexpr uint32_t kUiMovieTable = 0x826B59AC;

// ---- ui_movies.cpp's functions the other movie files call.

// IsXmd and IsMovieFile (sub_82586358, sub_82586538): the XMD archive's
// magic at its start, the movie file's at +8.
bool IsXmd(const uint8_t* file);
bool IsMovieFile(const uint8_t* file);
// UiMovie::UiMovie and DestroyNamedAnimations (sub_8211E380,
// sub_8211D378).
void ConstructMovie(guest::Guest& g, UiMovie* movie);
void DestroyNamedAnimations(guest::Guest& g, NamedAnimation* named);
// A text's first line and whether it scrolls, by its id (sub_8211B988,
// sub_8211BB88); 0 when the movie hasn't the text.
uint32_t TextFirstLine(UiMovie* movie, uint32_t id);
uint32_t TextScrolls(UiMovie* movie, uint32_t id);

}  // namespace game
