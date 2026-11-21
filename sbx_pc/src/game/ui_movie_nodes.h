// A UI movie's scene: its nodes (groups and scenes, pictures, primitives,
// texts, transforms) as chunks of the movie file, the texts' records and
// the movie's text map, the queued text items, the quads pictures and
// primitives are drawn as, and the small growing arrays the movies keep
// (their textures, the table of loaded movies). The movie itself, its file
// tables and animations are in ui_movies.h.

#pragma once

#include "game/fixed_list.h"
#include "game/guest_types.h"
#include "game/math.h"

namespace game {

using guest::f32_be;
using guest::i16_be;
using guest::i32_be;
using guest::ptr;
using guest::u32_be;

// The tags of the scene's nodes (and the file's chunks), as big-endian
// words: three characters and a 0.
constexpr uint32_t kTagGroup = 0x47525000;      // "GRP"
constexpr uint32_t kTagScene = 0x53434500;      // "SCE"
constexpr uint32_t kTagPicture = 0x50494300;    // "PIC"
constexpr uint32_t kTagPrimitive = 0x50524D00;  // "PRM"
constexpr uint32_t kTagText = 0x54585400;       // "TXT"
constexpr uint32_t kTagTransform = 0x54464D00;  // "TFM"
constexpr uint32_t kTagEnd = 0x54524D00;        // "TRM", a chunk list's end
constexpr uint32_t kTagXmd = 0x584D4400;        // "XMD"

// A clip rectangle in screen pixels, all 0 for none. The game sorts x0/x1
// and y0/y1 when it draws, so either order is allowed.
struct MovieClip {
  i32_be x0;
  i32_be y0;
  i32_be x1;
  i32_be y1;
};
GUEST_SIZE(MovieClip, 16);

// The same rectangle as the quads carry it, in 16-bit words.
struct MovieClip16 {
  i16_be x0;
  i16_be y0;
  i16_be x1;
  i16_be y1;
};
GUEST_SIZE(MovieClip16, 8);

// A color as four 32-bit integers, 0 to 255 (a quad's vertices).
struct MovieColorWords {
  u32_be r, g, b, a;
};
GUEST_SIZE(MovieColorWords, 16);

// The part every node starts with: its chunk's tag and size, the id
// animations name it by, and its pose (+12 to +83, what a pose reset puts
// back from the file's scene).
struct MovieNode {
  u32_be tag;
  u32_be size;  // From this chunk to the next.
  u32_be id;    // What animations name it by.
  f32_be x;     // Position.
  f32_be y;
  i32_be pivot_x;  // What it turns and scales about.
  i32_be pivot_y;
  f32_be scale_x;
  f32_be scale_y;
  f32_be rotation;  // Degrees.
  MovieClip clip;
  i32_be red, green, blue;  // 0 to 255
  f32_be alpha;             // 0 to 1
  i32_be layer;  // The file's layer offset until the load gives the drawing layer.
  u32_be size_flags;  // A text's font size (MovieTextNode::SizeFlags); not known for others.
  u32_be visible;
  uint8_t unknown_84[16];

  // The pose (+12 to +83) copied from another node, bits as they are.
  void SetPose(const MovieNode& from) {
    x = from.x;
    y = from.y;
    pivot_x = from.pivot_x;
    pivot_y = from.pivot_y;
    scale_x = from.scale_x;
    scale_y = from.scale_y;
    rotation = from.rotation;
    clip = from.clip;
    red = from.red;
    green = from.green;
    blue = from.blue;
    alpha = from.alpha;
    layer = from.layer;
    size_flags = from.size_flags;
    visible = from.visible;
  }
};
GUEST_SIZE(MovieNode, 100);
GUEST_OFFSET(MovieNode, id, 8);
GUEST_OFFSET(MovieNode, x, 12);
GUEST_OFFSET(MovieNode, scale_x, 28);
GUEST_OFFSET(MovieNode, clip, 40);
GUEST_OFFSET(MovieNode, red, 56);
GUEST_OFFSET(MovieNode, alpha, 68);
GUEST_OFFSET(MovieNode, layer, 72);
GUEST_OFFSET(MovieNode, size_flags, 76);
GUEST_OFFSET(MovieNode, visible, 80);

// A group or a scene: its children are a range of a scene's child list (a
// group's of the scene it's in, a scene's of its own). A scene's 8-byte
// entries after +128 hold its children and its animations.
struct MovieGroup {
  struct Entry {
    u32_be value;  // A pointer once the file is fixed up (an offset before).
    uint8_t unknown_4[4];
  };

  MovieNode node;
  i32_be first_child;  // In the scene's child list.
  i32_be child_count;
  u32_be children;  // A scene's: its child list's first entry.
  u32_be scene_child_count;
  u32_be animations;  // A scene's: its first animation entry.
  u32_be animation_count;
  u32_be entry_count;
  Entry entries[1];  // entry_count of them.

  Entry* EntryAt(uint32_t index) {
    return guest::Element<Entry>(guest::AddressOf(entries), index);
  }
  // Scene::Animation (sub_82582D68): a scene's animation `index`.
  uint32_t Animation(uint32_t index) { return EntryAt(animations + index)->value; }
};
GUEST_OFFSET(MovieGroup, first_child, 100);
GUEST_OFFSET(MovieGroup, children, 108);
GUEST_OFFSET(MovieGroup, animations, 116);
GUEST_OFFSET(MovieGroup, entry_count, 124);
GUEST_OFFSET(MovieGroup, entries, 128);

// A picture: a textured rectangle from (0, 0) to its size. +108 is which of
// its texture entries it shows; the entry's first word indexes the movie's
// textures.
struct MoviePicture {
  struct TextureEntry {
    u32_be index;  // In the movie's textures.
    uint8_t unknown_4[12];
  };

  MovieNode node;
  f32_be width;
  f32_be height;
  u32_be texture_entry;
  uint8_t unknown_112[4];
  i32_be source_left;  // The texture's pixels it shows.
  i32_be source_top;
  i32_be source_right;
  i32_be source_bottom;
  u32_be flip_x;
  u32_be flip_y;
  u32_be textured;
  TextureEntry texture_entries[1];

  // Picture::TextureEntry (sub_82582D40).
  TextureEntry* TextureEntryAt(uint32_t index) {
    return guest::Element<TextureEntry>(guest::AddressOf(texture_entries), index);
  }
};
GUEST_OFFSET(MoviePicture, width, 100);
GUEST_OFFSET(MoviePicture, texture_entry, 108);
GUEST_OFFSET(MoviePicture, source_left, 116);
GUEST_OFFSET(MoviePicture, flip_x, 132);
GUEST_OFFSET(MoviePicture, textured, 140);
GUEST_OFFSET(MoviePicture, texture_entries, 144);

// One of a primitive's vertices.
struct MovieVertex {
  f32_be x, y;
  i32_be red, green, blue;  // 0 to 255
  f32_be alpha;             // 0 to 1
  i32_be u, v;              // In the texture's pixels.
};
GUEST_SIZE(MovieVertex, 32);

// A primitive: a list of vertices, with a texture or not. Its texture
// entries come first after +128, then its vertices.
struct MoviePrimitive {
  using TextureEntry = MoviePicture::TextureEntry;

  MovieNode node;
  u32_be squared;  // Its colors squared; also the quad's blend mode.
  u32_be texture_entry;
  uint8_t unknown_108[4];
  u32_be texture_count;  // 0 for none; its vertices come after them.
  u32_be vertex_count;
  uint8_t unknown_120[8];
  TextureEntry texture_entries[1];

  // Primitive::TextureEntry (sub_82582D10).
  TextureEntry* TextureEntryAt(uint32_t index) {
    return guest::Element<TextureEntry>(guest::AddressOf(texture_entries), index);
  }
  // Primitive::Vertex (sub_82582D20).
  MovieVertex* VertexAt(uint32_t index) {
    return guest::Element<MovieVertex>(guest::AddressOf(TextureEntryAt(texture_count)), index);
  }
};
GUEST_OFFSET(MoviePrimitive, squared, 100);
GUEST_OFFSET(MoviePrimitive, texture_entry, 104);
GUEST_OFFSET(MoviePrimitive, texture_count, 112);
GUEST_OFFSET(MoviePrimitive, vertex_count, 116);
GUEST_OFFSET(MoviePrimitive, texture_entries, 128);

// A text node: the box its text is laid out in and how, and its text's id
// in the movie's text map (0 until it has one).
struct MovieTextNode {
  // The node's size flags (MovieNode::size_flags): the first set picks the
  // font's page.
  enum SizeFlags : uint32_t {
    kPage0 = 1,
    kPage1 = 2,
    kPage2 = 4,
    kPage3 = 8,
    kSmall = 16,  // Page 2 with glyphs at 0.65.
  };

  MovieNode node;
  f32_be box_width;  // The box the text is aligned in.
  f32_be box_height;
  u32_be text_id;
  uint8_t unknown_112[4];
  f32_be letter_spacing;
  f32_be line_spacing;
  u32_be horizontal_align;
  u32_be vertical_align;
  u32_be unknown_132;
  u32_be unknown_136;
};
GUEST_SIZE(MovieTextNode, 140);
GUEST_OFFSET(MovieTextNode, box_width, 100);
GUEST_OFFSET(MovieTextNode, text_id, 108);
GUEST_OFFSET(MovieTextNode, letter_spacing, 116);
GUEST_OFFSET(MovieTextNode, vertical_align, 128);

// A text's record (28 bytes, made by UiMovie::GetText), kept in the
// movie's text map by the text's id: its 2,048-byte buffer and how it
// scrolls and pages.
struct MovieTextRecord {
  static constexpr uint32_t kBufferSize = 2048;

  u32_be buffer;
  u32_be scrolling;      // It scrolls (the garage's ticker).
  u32_be scroll_frames;  // Frames counted since it started (TickerText::Step).
  i32_be scroll_delay;   // Frames to wait before it scrolls.
  f32_be scroll_speed;
  u32_be page;        // The page shown (Strings::CopyPage).
  u32_be first_line;  // The page's lines skipped before the one shown first.
};
GUEST_SIZE(MovieTextRecord, 28);

// The movie's text map: 128 {id, record} pairs and their count.
struct MovieTextMap {
  struct Pair {
    i32_be id;
    ptr<MovieTextRecord> record;
  };
  static constexpr int kPairs = 128;

  Pair pairs[kPairs];
  u32_be count;
};
GUEST_SIZE(MovieTextMap, 1028);

// A queued text (128 bytes, made by TextItem::TextItem): a text node with
// the context it was drawn in, drawn after the movie's nodes
// (UiMovie::DrawQueuedText).
struct MovieTextItem {
  ptr<MovieNode> node;
  uint8_t unknown_4[12];
  Matrix matrix;
  f32_be depth;
  f32_be red, green, blue, alpha;
  MovieClip clip;
  uint8_t unknown_116[12];
};
GUEST_SIZE(MovieTextItem, 128);
GUEST_OFFSET(MovieTextItem, matrix, 16);
GUEST_OFFSET(MovieTextItem, depth, 80);
GUEST_OFFSET(MovieTextItem, red, 84);
GUEST_OFFSET(MovieTextItem, clip, 100);

// The movie's text list: 512 text items in one block (operator new[]: the
// count at the block's start, the items 16 bytes on), and how many are
// queued.
struct MovieTextList {
  static constexpr uint32_t kItems = 512;

  ptr<MovieTextItem> items;
  u32_be capacity;
  u32_be count;
};
GUEST_SIZE(MovieTextList, 12);

// A quad or primitive as the recorders take it (made by Quad::Quad on the
// drawing function's stack). Its vertices are three arrays of 16 bytes a
// vertex.
struct MovieQuad {
  Matrix matrix;  // Identity; not read by the recorders.
  u32_be unknown_64;
  u32_be blend;  // 0 to 4; 5 and up set no blend state.
  u32_be unknown_72;
  uint8_t unknown_76[4];
  u32_be vertex_count;
  u32_be texture;  // The texture's handle, or 0 for none.
  ptr<Vector> positions;
  ptr<MovieColorWords> colors;
  ptr<Vector> texcoords;  // u and v; z and w not used.
  u32_be clipped;
  MovieClip16 clip;
  u32_be scaled;  // The context's: picks the recorders' sampler state.
};
GUEST_OFFSET(MovieQuad, blend, 68);
GUEST_OFFSET(MovieQuad, vertex_count, 80);
GUEST_OFFSET(MovieQuad, positions, 88);
GUEST_OFFSET(MovieQuad, clipped, 100);
GUEST_OFFSET(MovieQuad, clip, 104);
GUEST_OFFSET(MovieQuad, scaled, 112);

// The game's growing array of words (the movie's textures: MovieTexture
// pointers).
struct MovieWords {
  u32_be data;
  u32_be capacity;
  u32_be count;
};
GUEST_SIZE(MovieWords, 12);

// One of the movie's textures (12 bytes, held by address in its
// textures): the texture and its size.
struct MovieTexture {
  u32_be texture;  // What the recorders hand the renderer.
  u32_be width;
  u32_be height;
};
GUEST_SIZE(MovieTexture, 12);

// The game's growing array of 8-byte {key, value} pairs (the table of
// loaded movies, kUiMovieTable).
struct MoviePairs {
  struct Pair {
    i32_be key;
    u32_be value;
  };

  ptr<Pair> data;
  u32_be capacity;
  u32_be count;
};
GUEST_SIZE(MoviePairs, 12);

// ---- ui_movie_nodes.cpp's functions the other movie files call.

struct UiMovie;
struct AnimationSlot;
using AnimationList = FixedList<32, AnimationSlot>;

// TextItem::TextItem, ~TextItem and operator= (sub_8211A9E8, sub_8211AA98,
// sub_8211B0A0), and Quad::Quad (sub_8211AB18).
void MakeTextItem(MovieTextItem* item);
void ClearTextItem(MovieTextItem* item);
void CopyTextItem(MovieTextItem* to, const MovieTextItem* from);
void MakeQuad(MovieQuad* quad);
// TextItem's deleting destructor (sub_8211B160). Returns the block.
uint32_t DeleteTextItems(guest::Guest& g, MovieTextItem* items, uint32_t flags);
// TextList::TextList (sub_8211B1E8).
void MakeTextList(guest::Guest& g, MovieTextList* list);

// MovieWords::MovieWords and Append (sub_8211AFE8, sub_8211B038).
void MakeWords(guest::Guest& g, MovieWords* words);
void AppendWord(guest::Guest& g, MovieWords* words, uint32_t word);
// UiMovies::Find (sub_8211B5C0): the value's address for a key, or the
// game's word for none (0). MoviePairs::Append (sub_8211B628).
uint32_t FindPair(MoviePairs* pairs, int32_t key);
void AppendPair(guest::Guest& g, MoviePairs* pairs, MoviePairs::Pair pair);

// TextMap::Find and Set (sub_8211B4E0, sub_8211B540), and a text's record
// by its id (nullptr for none).
uint32_t FindTextSlot(MovieTextMap* map, int32_t id);
void SetText(MovieTextMap* map, int32_t id, MovieTextRecord* record);
MovieTextRecord* FindText(UiMovie* movie, uint32_t id);
// UiMovie::GetText, TextString and TextPage (sub_8211B830, sub_8211B8F0,
// sub_8211B940).
MovieTextRecord* GetText(guest::Guest& g, UiMovie* movie, u32_be* id);
uint32_t TextString(UiMovie* movie, uint32_t id);
uint32_t TextPage(UiMovie* movie, uint32_t id);
// UiMovie::FreeTexts, ClearTextIds, InitAnimationLists and Unload
// (sub_8211AE60, sub_8211A890, sub_8211ABB8, sub_8211B710).
void FreeTexts(guest::Guest& g, UiMovie* movie);
void ClearTextIds(UiMovie* movie);
void InitAnimationLists(UiMovie* movie);
void Unload(guest::Guest& g, UiMovie* movie);
// UiMovie::AssignLayers (sub_8211BEE0).
void AssignLayers(guest::Guest& g, UiMovie* movie, MovieGroup* scene, MovieGroup* group,
                  uint32_t layer, bool by_bucket);

// AnimationList::RemoveAt, Get and Insert (sub_8211B250, sub_8211B468,
// sub_8211B6A0). Insert returns the list's slot.
void RemoveAt(AnimationList* list, uint32_t position);
void GetSlot(AnimationSlot* to, AnimationList* list, uint32_t position);
AnimationSlot* InsertSlot(AnimationList* list, const AnimationSlot* slot, uint32_t position);

}  // namespace game
