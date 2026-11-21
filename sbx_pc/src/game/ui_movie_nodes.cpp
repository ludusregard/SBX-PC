// The UI movies' nodes, textures, texts and lists as our own C++ (under
// --native_ui_movies, defined in ui_movies.cpp): finding a node and
// showing, hiding, coloring or texturing it; resetting a node's pose; the
// layers a load gives the nodes; the text items and quads; the movie's
// texture and pair arrays and its text map; the animation lists' links;
// unloading a movie; and the animations' times.
//
// Nothing is kept between calls.

#include "game/ui_movie_nodes.h"

#include "base/settings.h"
#include "game/strings.h"
#include "game/ui_movie_draw.h"
#include "game/ui_movies.h"
#include "hooks/native_calls.h"

#include <algorithm>
#include <cstdint>

SBX_SETTING_DECLARE(bool, native_ui_movies);

namespace game {
namespace {

using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;

bool On() { return SBX_GET(native_ui_movies); }

// Guest addresses (writable data, read and written in place).
constexpr uint32_t kRenderApp = 0x826AA880;   // Holds the render manager.
constexpr uint32_t kNoMovie = 0x826B59BC;     // UiMovies::Find's answer for a key not there.
constexpr uint32_t kNoText = 0x826B59B8;      // TextMap::Find's answer for an id not there.
constexpr uint32_t kScreenFlag = 0x826B5990;  // Cleared first by UiMovies::Reset.
// The words UiMovies::Reset clears after the table, in its order.
constexpr uint32_t kScreenWords[] = {0x826B5984, 0x826B598C, 0x826B5988, 0x826B59A0,
                                     0x826B5994, 0x826B5998, 0x826B599C};

// Read-only image data.
constexpr uint32_t kColors = 0x820083A0;     // SetNodeColor's colors: 12 bytes each.
constexpr uint32_t kEmptyText = 0x820083C4;  // An empty string.
constexpr float kNoTime = -1.0f;             // 0x82001BD8: an animation that isn't running.

// The render manager's slots UiMovies::Reset calls.
constexpr uint32_t kRenderManagerSlot = 8;
constexpr uint32_t kCommandListSlot = 1;
constexpr uint32_t kRendererSlot = 2;

// The deleting destructor's flags: an array, and freed.
constexpr uint32_t kDeleteArray = 2;
constexpr uint32_t kDeleteFree = 1;

constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kMemRelease = 0x8000;
constexpr uint32_t kPageReadWrite = 4;

constexpr int kLayerBuckets = 64;
constexpr uint32_t kBucketNodes = 64;

// The movie's texture `index` (its textures' array, by a 32-bit index).
u32_be* Texture(UiMovie* movie, uint32_t index) {
  return Element<u32_be>(movie->textures.data, index);
}

// A node in `scene` by its index, or in its sub-scene `sub_scene` when
// that isn't -1.
MovieNode* FindIn(MovieGroup* scene, int32_t sub_scene, int32_t index) {
  if (sub_scene != -1) {
    scene = reinterpret_cast<MovieGroup*>(Child(scene, uint32_t(sub_scene)));
  }
  return Child(scene, uint32_t(index));
}

// UiMovie::FindNode (sub_8211A748): a node of the live scene.
MovieNode* FindNode(UiMovie* movie, int32_t sub_scene, int32_t index) {
  return FindIn(movie->live, sub_scene, index);
}

// The movie's texture a picture's texture entry names.
u32_be* PictureTexture(UiMovie* movie, MoviePicture* picture) {
  return Texture(movie, picture->TextureEntryAt(picture->texture_entry)->index);
}

Matrix Identity() {
  Matrix m;
  m.right = {1.0f, 0.0f, 0.0f, 0.0f};
  m.up = {0.0f, 1.0f, 0.0f, 0.0f};
  m.forward = {0.0f, 0.0f, 1.0f, 0.0f};
  m.position = {0.0f, 0.0f, 0.0f, 1.0f};
  return m;
}

// ---- Nodes

// UiNode::CopyFields: the node `to` given `from`'s fields, by `to`'s tag:
// the pose, then a picture's, a primitive's (with each of `from`'s
// vertices) or a text's own.
void CopyNodeFields(MovieNode* to, MovieNode* from) {
  const uint32_t tag = to->tag;
  switch (tag) {
    case kTagGroup:
    case kTagPicture:
    case kTagPrimitive:
    case kTagScene:
    case kTagTransform:
    case kTagText:
      to->SetPose(*from);
      break;
  }
  if (tag == kTagPicture) {
    MoviePicture* out = reinterpret_cast<MoviePicture*>(to);
    const MoviePicture* in = reinterpret_cast<MoviePicture*>(from);
    out->width = in->width;
    out->height = in->height;
    out->texture_entry = in->texture_entry;
    std::copy(std::begin(in->unknown_112), std::end(in->unknown_112), out->unknown_112);
    out->source_left = in->source_left;
    out->source_top = in->source_top;
    out->source_right = in->source_right;
    out->source_bottom = in->source_bottom;
    out->flip_x = in->flip_x;
    out->flip_y = in->flip_y;
  } else if (tag == kTagPrimitive) {
    MoviePrimitive* out = reinterpret_cast<MoviePrimitive*>(to);
    MoviePrimitive* in = reinterpret_cast<MoviePrimitive*>(from);
    out->squared = in->squared;
    out->texture_entry = in->texture_entry;
    std::copy(std::begin(in->unknown_108), std::end(in->unknown_108), out->unknown_108);
    for (uint32_t i = 0; i < in->vertex_count; i++) {
      *out->VertexAt(i) = *in->VertexAt(i);
    }
  } else if (tag == kTagText) {
    MovieTextNode* out = reinterpret_cast<MovieTextNode*>(to);
    const MovieTextNode* in = reinterpret_cast<MovieTextNode*>(from);
    out->box_width = in->box_width;
    out->box_height = in->box_height;
    out->letter_spacing = in->letter_spacing;
    out->line_spacing = in->line_spacing;
    out->horizontal_align = in->horizontal_align;
    out->vertical_align = in->vertical_align;
    out->unknown_132 = in->unknown_132;
    out->unknown_136 = in->unknown_136;
  }
}

// UiMovie::ResetNodeFields: a node's fields put back from the file's
// scene in the live and saved ones (not its subtree).
void ResetNodeFields(UiMovie* movie, int32_t sub_scene, int32_t index) {
  MovieNode* from = FindIn(movie->file, sub_scene, index);
  MovieNode* live = FindIn(movie->live, sub_scene, index);
  MovieNode* saved = FindIn(movie->saved, sub_scene, index);
  CopyNodeFields(live, from);
  CopyNodeFields(saved, from);
}

// UiMovie::ResetNode: a node's pose put back from the file's scene in the
// live and saved ones, then its subtree (RestorePose).
void ResetNode(UiMovie* movie, int32_t sub_scene, int32_t index) {
  MovieGroup* file_scene = movie->file;
  MovieGroup* live_scene = movie->live;
  MovieGroup* saved_scene = movie->saved;
  if (sub_scene != -1) {
    file_scene = reinterpret_cast<MovieGroup*>(Child(file_scene, uint32_t(sub_scene)));
    live_scene = reinterpret_cast<MovieGroup*>(Child(live_scene, uint32_t(sub_scene)));
    saved_scene = reinterpret_cast<MovieGroup*>(Child(saved_scene, uint32_t(sub_scene)));
  }
  MovieNode* from = Child(file_scene, uint32_t(index));
  MovieNode* live = Child(live_scene, uint32_t(index));
  MovieNode* saved = Child(saved_scene, uint32_t(index));
  live->SetPose(*from);
  saved->SetPose(*from);
  RestorePose(live_scene, file_scene, reinterpret_cast<MovieGroup*>(live),
              reinterpret_cast<MovieGroup*>(from));
  RestorePose(saved_scene, file_scene, reinterpret_cast<MovieGroup*>(saved),
              reinterpret_cast<MovieGroup*>(from));
}

// UiMovie::SetColor: the node's color set to entry `color` of the colors'
// table.
void SetColor(MovieNode* node, uint32_t color) {
  const u32_be* rgb = Element<u32_be>(kColors, 3 * color);
  node->red = int32_t(uint32_t(rgb[0]));
  node->green = int32_t(uint32_t(rgb[1]));
  node->blue = int32_t(uint32_t(rgb[2]));
}

// UiMovie::SetLayer: the drawing layer for a node with layer number n:
// 124 + 2n for the first 12, 580 + 2n up to 665 after.
void SetLayer(MovieNode* node, uint32_t number) {
  uint32_t layer;
  if (number <= 11) {
    layer = std::min((number + 62) * 2, 147u);
  } else if (number <= 43) {
    layer = std::min((number + 290) * 2, 665u);
  } else {
    layer = 665;
  }
  node->layer = int32_t(layer);
}

// UiMovie::SetNodeTexture: a primitive's or a picture's texture entry
// (other nodes are left as they are).
void SetTextureEntry(MovieNode* node, uint32_t entry) {
  if (node->tag == kTagPrimitive) {
    reinterpret_cast<MoviePrimitive*>(node)->texture_entry = entry;
  } else if (node->tag == kTagPicture) {
    reinterpret_cast<MoviePicture*>(node)->texture_entry = entry;
  }
}

// ---- The arrays

// The array resized to `capacity` (when that's new): a new block
// (operator new[]; -1 bytes when the size overflows), the words that fit
// copied, the old block freed (operator delete[]).
void ReserveWords(Guest& g, MovieWords* words, uint32_t capacity) {
  if (capacity == words->capacity) {
    return;
  }
  const uint32_t bytes = capacity > 0x3FFFFFFFu ? 0xFFFFFFFFu : capacity * 4;
  const uint32_t block = g.Call(sub_8258C0C0, bytes);  // operator new[]
  for (uint32_t i = 0; i < std::min(uint32_t(words->capacity), capacity); i++) {
    *Element<u32_be>(block, i) = uint32_t(*Element<u32_be>(words->data, i));
  }
  g.Call(sub_8258C940, words->data);  // operator delete[]
  words->data = block;
  words->capacity = capacity;
}

// The movie table's pairs: 64 cleared (operator new[]; none when it fails).
void MakePairs(Guest& g, MoviePairs* pairs) {
  pairs->capacity = 64;
  const uint32_t block = g.Call(sub_8258C0C0, 512u);  // operator new[]
  if (block != 0) {
    for (uint32_t i = 0; i < 64; i++) {
      MoviePairs::Pair* pair = Element<MoviePairs::Pair>(block, i);
      pair->key = 0;
      pair->value = 0;
    }
  }
  pairs->data.address = block;
}

// The pairs resized to `capacity` (when that's new), as ReserveWords, the
// new block cleared first.
void ReservePairs(Guest& g, MoviePairs* pairs, uint32_t capacity) {
  if (capacity == pairs->capacity) {
    return;
  }
  const uint32_t bytes = capacity > 0x1FFFFFFFu ? 0xFFFFFFFFu : capacity * 8;
  const uint32_t block = g.Call(sub_8258C0C0, bytes);  // operator new[]
  if (block != 0) {
    for (uint32_t i = 0; i < capacity; i++) {
      MoviePairs::Pair* pair = Element<MoviePairs::Pair>(block, i);
      pair->key = 0;
      pair->value = 0;
    }
  }
  for (uint32_t i = 0; i < std::min(uint32_t(pairs->capacity), capacity); i++) {
    MoviePairs::Pair* to = Element<MoviePairs::Pair>(block, i);
    const MoviePairs::Pair* from = Element<MoviePairs::Pair>(pairs->data.address, i);
    to->key = int32_t(from->key);
    to->value = uint32_t(from->value);
  }
  g.Call(sub_8258C940, pairs->data);  // operator delete[]
  pairs->data.address = block;
  pairs->capacity = capacity;
}

// UiMovies::Reset (a screen's start): the UI movies given the render
// manager's renderer and command list (UiMovie::SetRenderer), every movie
// table entry marked free (key -1, no movie; nothing is freed), the screen's
// words cleared.
void ResetMovies(Guest& g) {
  *At<u32_be>(kScreenFlag) = 0;
  const uint32_t command_list = g.CallVirtual(
      At<u32_be>(g.CallVirtual(At<u32_be>(*At<u32_be>(kRenderApp)), kRenderManagerSlot)),
      kCommandListSlot);
  const uint32_t renderer = g.CallVirtual(
      At<u32_be>(g.CallVirtual(At<u32_be>(*At<u32_be>(kRenderApp)), kRenderManagerSlot)),
      kRendererSlot);
  SetMovieRenderer(renderer, command_list);
  MoviePairs* table = At<MoviePairs>(kUiMovieTable);
  for (uint32_t i = 0; i < table->count; i++) {
    MoviePairs::Pair* pair = Element<MoviePairs::Pair>(table->data.address, i);
    pair->key = -1;
    pair->value = 0;
  }
  for (uint32_t address : kScreenWords) {
    *At<u32_be>(address) = 0;
  }
}

// ---- Animation lists

// The slot of the first animation in the looping list, then the queue,
// that `matches`; nullptr when neither has one.
template <typename Match>
AnimationSlot* FindSlot(UiMovie* movie, Match matches) {
  for (AnimationList* list : {&movie->looping, &movie->queue}) {
    const uint32_t count = uint32_t(int32_t(list->count));
    for (uint32_t i = 0; i < count; i++) {
      AnimationSlot* slot = list->ItemAtPosition(i);
      if (matches(*slot)) {
        return slot;
      }
    }
  }
  return nullptr;
}

float AnimationTime(AnimationSlot* slot) { return slot != nullptr ? float(slot->time) : kNoTime; }

}  // namespace

// ---- Text items and quads

// TextItem::TextItem: no text, an identity matrix, its depth, color and
// clip zero (the float 0.0's word).
void MakeTextItem(MovieTextItem* item) {
  item->matrix = Identity();
  item->depth = 0.0f;
  item->node = nullptr;
  item->red = 0.0f;
  item->green = 0.0f;
  item->blue = 0.0f;
  item->alpha = 0.0f;
}

// TextItem::~TextItem: no text, an identity matrix, its depth zero.
void ClearTextItem(MovieTextItem* item) {
  item->depth = 0.0f;
  item->node = nullptr;
  item->matrix = Identity();
}

// TextItem::operator=: all but +4 to +15 and +116 on.
void CopyTextItem(MovieTextItem* to, const MovieTextItem* from) {
  to->node = from->node;
  to->matrix = from->matrix;
  to->depth = from->depth;
  to->red = from->red;
  to->green = from->green;
  to->blue = from->blue;
  to->alpha = from->alpha;
  to->clip = from->clip;
}

// Quad::Quad: an identity matrix, blend mode 0, +72 = 2.
void MakeQuad(MovieQuad* quad) {
  quad->matrix = Identity();
  quad->unknown_64 = 0;
  quad->blend = 0;
  quad->unknown_72 = 2;
}

// TextItem's deleting destructor: with kDeleteArray an array made by
// operator new[] (its count in the block's first word, the items 16 bytes
// on), each destroyed last first; else one item. Freed with kDeleteFree.
// Returns the block (or the item).
uint32_t DeleteTextItems(Guest& g, MovieTextItem* items, uint32_t flags) {
  if ((flags & kDeleteArray) != 0) {
    const uint32_t block = AddressOf(items) - 16;
    const int32_t count = int32_t(uint32_t(*At<u32_be>(block)));
    for (int32_t i = count - 1; i >= 0; i--) {
      ClearTextItem(Element<MovieTextItem>(AddressOf(items), uint32_t(i)));
    }
    if ((flags & kDeleteFree) != 0) {
      g.Call(sub_8258C940, block);  // operator delete[]
    }
    return block;
  }
  ClearTextItem(items);
  if ((flags & kDeleteFree) != 0) {
    g.Call(sub_8258C868, items);  // operator delete
  }
  return AddressOf(items);
}

// TextList::TextList: 512 text items made in one block (operator new[];
// none when it fails). The count is set by the movie's constructor.
void MakeTextList(Guest& g, MovieTextList* list) {
  constexpr uint32_t kBlock = 16 + MovieTextList::kItems * sizeof(MovieTextItem);
  list->capacity = MovieTextList::kItems;
  const uint32_t block = g.Call(sub_8258C0C0, kBlock);  // operator new[]
  uint32_t items = 0;
  if (block != 0) {
    items = block + 16;
    *At<u32_be>(block) = MovieTextList::kItems;
    for (uint32_t i = 0; i < MovieTextList::kItems; i++) {
      MakeTextItem(Element<MovieTextItem>(items, i));
    }
  }
  list->items.address = items;
}

// ---- The arrays

// The movie's textures array: 512 words (operator new[]), none used.
void MakeWords(Guest& g, MovieWords* words) {
  words->capacity = 512;
  words->data = g.Call(sub_8258C0C0, 2048u);  // operator new[]
  words->count = 0;
}

// A word appended, the array doubled first when full.
void AppendWord(Guest& g, MovieWords* words, uint32_t word) {
  if (words->count >= words->capacity) {
    ReserveWords(g, words, words->capacity * 2);
  }
  const uint32_t index = words->count;
  words->count = index + 1;
  *Element<u32_be>(words->data, index) = word;
}

// A pair appended, the array doubled first when full.
void AppendPair(Guest& g, MoviePairs* pairs, MoviePairs::Pair pair) {
  if (pairs->count >= pairs->capacity) {
    ReservePairs(g, pairs, pairs->capacity * 2);
  }
  const uint32_t index = pairs->count;
  pairs->count = index + 1;
  *Element<MoviePairs::Pair>(pairs->data.address, index) = pair;
}

// UiMovies::Find: the value's address for a key, or kNoMovie (a word of 0).
uint32_t FindPair(MoviePairs* pairs, int32_t key) {
  const uint32_t count = pairs->count;
  for (uint32_t i = 0; i < count; i++) {
    MoviePairs::Pair* pair = Element<MoviePairs::Pair>(pairs->data.address, i);
    if (pair->key == key) {
      return AddressOf(&pair->value);
    }
  }
  return kNoMovie;
}

// TextMap::Find: the record slot for a text id, or kNoText (a word of 0)
// when the map has none.
uint32_t FindTextSlot(MovieTextMap* map, int32_t id) {
  const uint32_t count = map->count;
  for (uint32_t i = 0; i < count; i++) {
    if (map->pairs[i].id == id) {
      return AddressOf(&map->pairs[i].record);
    }
  }
  return kNoText;
}

// TextMap::Set: the record for a text id, replaced when the id is there,
// else appended. There's no check against the map's 128 pairs: the count
// is stored before the pair, so a 129th lands on it.
void SetText(MovieTextMap* map, int32_t id, MovieTextRecord* record) {
  const uint32_t count = map->count;
  for (uint32_t i = 0; i < map->count; i++) {
    if (map->pairs[i].id == id) {
      map->pairs[i].record = record;
      return;
    }
  }
  map->count = count + 1;
  MovieTextMap::Pair* pair = Element<MovieTextMap::Pair>(AddressOf(map->pairs), count);
  pair->id = id;
  pair->record = record;
}

// A text's record by its id; nullptr when it has none (an id not in the
// map reads kNoText's word as its record, as the game does).
MovieTextRecord* FindText(UiMovie* movie, uint32_t id) {
  return At<MovieTextRecord>(*At<u32_be>(FindTextSlot(&movie->text_map, int32_t(id))));
}

// ---- The movie's texts and textures

// UiMovie::FreeTexts: each text's buffer (VirtualFree) and record
// (operator delete) freed; the map emptied (its ids stay).
void FreeTexts(Guest& g, UiMovie* movie) {
  MovieTextMap& map = movie->text_map;
  for (uint32_t i = 0; i < map.count; i++) {
    g.Call(sub_82292D70, map.pairs[i].record->buffer, 0u, kMemRelease);  // VirtualFree
    g.Call(sub_8258C868, map.pairs[i].record);                           // operator delete
  }
  map.count = 0;
}

// UiMovie::GetText: the record of the text whose id is at `id`, the text
// given the next id first when it has none. A text with no record gets a
// new one (operator new) with a cleared 2 KB buffer (VirtualAlloc); the
// caller stores it in the map.
MovieTextRecord* GetText(Guest& g, UiMovie* movie, u32_be* id) {
  if (*id == 0) {
    *id = movie->text_map.count + 1;
  }
  MovieTextRecord* record = FindText(movie, *id);
  if (record != nullptr) {
    return record;
  }
  record = At<MovieTextRecord>(g.Call(sub_8258C000, uint32_t(sizeof(MovieTextRecord))));
  const uint32_t buffer = g.Call(sub_82292DD8, 0u, MovieTextRecord::kBufferSize, kMemCommit,
                                 kPageReadWrite);  // VirtualAlloc
  record->buffer = buffer;
  g.Call(sub_8258C950, buffer, 0u, MovieTextRecord::kBufferSize);  // memset
  record->scroll_speed = 0.0f;
  record->scrolling = 0;
  record->scroll_frames = 0;
  record->scroll_delay = 0;
  record->page = 0;
  record->first_line = 0;
  return record;
}

// UiMovie::TextString: a text's buffer, or an empty string when it has no
// record.
uint32_t TextString(UiMovie* movie, uint32_t id) {
  const MovieTextRecord* record = FindText(movie, id);
  return record != nullptr ? uint32_t(record->buffer) : kEmptyText;
}

// UiMovie::TextPage: the page a text shows, or 0 when it has no record.
uint32_t TextPage(UiMovie* movie, uint32_t id) {
  const MovieTextRecord* record = FindText(movie, id);
  return record != nullptr ? uint32_t(record->page) : 0;
}

// UiMovie::ClearTextIds: every text after the live scene's chunk (up to
// the end chunk) loses its text's id.
void ClearTextIds(UiMovie* movie) {
  MovieNode* chunk = &movie->live->node;
  while (chunk->tag != kTagEnd) {
    chunk = At<MovieNode>(AddressOf(chunk) + chunk->size);
    if (chunk->tag == kTagText) {
      reinterpret_cast<MovieTextNode*>(chunk)->text_id = 0;
    }
  }
}

// UiMovie::InitAnimationLists: the movie's file copies, texture set,
// scenes and loaded flag cleared, its queue and looping animations emptied.
void InitAnimationLists(UiMovie* movie) {
  for (u32_be& copy : movie->file_copies) {
    copy = 0;
  }
  movie->texture_set = 0;
  movie->live = nullptr;
  movie->saved = nullptr;
  movie->file = nullptr;
  movie->loaded = 0;
  movie->queue.Reset();
  movie->looping.Reset();
}

// UiMovie::Unload: the file's three copies freed (VirtualFree) and the
// scenes cleared; the textures' entries cleared and freed (operator
// delete), last first; the queued text items taken off; the texture set
// released (XPR::Release); the movie marked not loaded and its texts freed.
void Unload(Guest& g, UiMovie* movie) {
  for (u32_be& copy : movie->file_copies) {
    g.Call(sub_82292D70, copy, 0u, kMemRelease);  // VirtualFree
  }
  movie->live = nullptr;
  movie->saved = nullptr;
  movie->file = nullptr;
  MovieWords& textures = movie->textures;
  while (textures.count != 0) {
    const uint32_t index = textures.count - 1;
    textures.count = index;
    MovieTexture* entry = At<MovieTexture>(*Element<u32_be>(textures.data, index));
    if (entry != nullptr) {
      entry->texture = 0;
      entry->width = 0;
      entry->height = 0;
      g.Call(sub_8258C868, entry);  // operator delete
    }
  }
  // The game copies each queued item out and destroys the copy, which
  // changes only its stack.
  movie->texts.count = 0;
  if (movie->texture_set != 0) {
    ReleaseXpr(g, &movie->texture_set);
  }
  movie->texture_set = 0;
  movie->loaded = 0;
  FreeTexts(g, movie);
}

// UiMovie::AssignLayers: the group's children sorted by their layer offset
// (at most 63), last child first, then in bucket order: groups and
// sub-scenes recursed into (a sub-scene as its own scene), pictures,
// primitives and texts given their layer: the bucket's number when
// `by_bucket` is set, else `layer`.
//
// A group with a negative offset or more than 64 children in a bucket has
// the recompiled function sort it: its buckets on its stack are written
// outside them there (as in the game).
void AssignLayers(Guest& g, UiMovie* movie, MovieGroup* scene, MovieGroup* group, uint32_t layer,
                  bool by_bucket) {
  struct Bucket {
    MovieNode* nodes[kBucketNodes];
    uint32_t count = 0;
  };
  Bucket buckets[kLayerBuckets];
  for (int32_t i = group->first_child + group->child_count - 1; i >= group->first_child; i--) {
    MovieNode* child = Child(scene, uint32_t(i));
    const int32_t offset = std::min(int32_t(child->layer), kLayerBuckets - 1);
    if (offset < 0 || buckets[offset].count == kBucketNodes) {
      g.Call(__imp__sub_8211BEE0, movie, scene, group, layer, uint32_t(by_bucket));
      return;
    }
    Bucket& bucket = buckets[offset];
    bucket.nodes[bucket.count++] = child;
  }
  for (int k = 0; k < kLayerBuckets; k++) {
    for (uint32_t j = 0; j < buckets[k].count; j++) {
      MovieNode* node = buckets[k].nodes[j];
      if (by_bucket) {
        layer = uint32_t(k);
      }
      switch (uint32_t(node->tag)) {
        case kTagGroup:
          AssignLayers(g, movie, scene, reinterpret_cast<MovieGroup*>(node), layer, false);
          break;
        case kTagScene:
          AssignLayers(g, movie, reinterpret_cast<MovieGroup*>(node),
                       reinterpret_cast<MovieGroup*>(node), layer, false);
          break;
        case kTagPicture:
        case kTagPrimitive:
        case kTagText:
          SetLayer(node, layer);
          break;
      }
    }
  }
}

// ---- Animation lists

// AnimationList::RemoveAt: the link at `position` taken out and put first
// on the free links.
void RemoveAt(AnimationList* list, uint32_t position) {
  i16_be* link = position == 0 ? &list->head : &list->Walk(position - 1)->next;
  const int16_t taken = *link;
  AnimationList::Node* node = list->NodeAt(taken);
  *link = int16_t(node->next);
  node->next = int16_t(list->free_head);
  list->free_head = taken;
  list->count = int16_t(list->count - 1);
}

// AnimationList::Get: the slot at `position` copied to `to`.
void GetSlot(AnimationSlot* to, AnimationList* list, uint32_t position) {
  *to = *list->ItemAtPosition(position);
}

// AnimationList::Insert: a free link put in at `position` (its low 16
// bits) and the slot copied to it.
AnimationSlot* InsertSlot(AnimationList* list, const AnimationSlot* slot, uint32_t position) {
  list->Link(int16_t(position));
  AnimationSlot* to = list->ItemAtPosition(position);
  *to = *slot;
  return to;
}

}  // namespace game

using namespace game;

#define SBX_UI_MOVIE_NODES_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                    \
  if (!On()) {                            \
    __imp__##name(ctx, base);             \
    return;                               \
  }                                       \
  Guest g(ctx, base);

// The adapters: r3 the movie (or the object named), then the arguments as
// the original takes them. A node is named by r4, a sub-scene's index in
// the live scene or -1, and r5, its index there.

// UiNode::CopyFields: r4 the node given r5's fields (r3, the movie, isn't
// read).
extern "C" REX_FUNC(sub_8211A078) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A078)
  CopyNodeFields(At<MovieNode>(ctx.r4.u32), At<MovieNode>(ctx.r5.u32));
}

// UiMovie::ResetNodeFields: r3 the movie, r4 and r5 the node.
extern "C" REX_FUNC(sub_8211A300) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A300)
  ResetNodeFields(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
}

// UiMovie::ResetNode: r3 the movie, r4 and r5 the node.
extern "C" REX_FUNC(sub_8211A3B0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A3B0)
  ResetNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
}

// UiMovie::FindNode: r3 the movie, r4 and r5 the node. Returns it.
extern "C" REX_FUNC(sub_8211A748) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A748)
  ctx.r3.u64 = AddressOf(FindNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32));
}

// UiMovie::SetNodeColor: r3 the movie, r4 and r5 the node, r6 the color.
// Returns the node.
extern "C" REX_FUNC(sub_8211A788) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A788)
  const uint32_t color = ctx.r6.u32;
  MovieNode* node = FindNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
  SetColor(node, color);
  ctx.r3.u64 = AddressOf(node);
}

// UiMovie::SetColor: r4 the node, r5 the color (r3 isn't read or set: a
// caller reads it on).
extern "C" REX_FUNC(sub_8211A800) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A800)
  SetColor(At<MovieNode>(ctx.r4.u32), ctx.r5.u32);
}

// UiMovie::SetLayer: r4 the node, r5 its layer number.
extern "C" REX_FUNC(sub_8211A830) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A830)
  SetLayer(At<MovieNode>(ctx.r4.u32), ctx.r5.u32);
}

// UiMovie::ClearTextIds: r3 the movie.
extern "C" REX_FUNC(sub_8211A890) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A890)
  ClearTextIds(At<UiMovie>(ctx.r3.u32));
}

// UiMovie::SetNodeTexture: r3 the movie, r4 and r5 the node, r6 the
// texture entry. Returns the node.
extern "C" REX_FUNC(sub_8211A8D8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A8D8)
  const uint32_t entry = ctx.r6.u32;
  MovieNode* node = FindNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
  SetTextureEntry(node, entry);
  ctx.r3.u64 = AddressOf(node);
}

// MovieWords::Reserve: r3 the array, r4 the capacity.
extern "C" REX_FUNC(sub_8211A958) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A958)
  ReserveWords(g, At<MovieWords>(ctx.r3.u32), ctx.r4.u32);
}

// TextItem::TextItem: r3 the item, returned.
extern "C" REX_FUNC(sub_8211A9E8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211A9E8)
  MakeTextItem(At<MovieTextItem>(ctx.r3.u32));
}

// TextItem::~TextItem: r3 the item.
extern "C" REX_FUNC(sub_8211AA98) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AA98)
  ClearTextItem(At<MovieTextItem>(ctx.r3.u32));
}

// Quad::Quad: r3 the quad, returned.
extern "C" REX_FUNC(sub_8211AB18) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AB18)
  MakeQuad(At<MovieQuad>(ctx.r3.u32));
}

// UiMovie::InitAnimationLists: r3 the movie (left in r3: a caller reads
// it).
extern "C" REX_FUNC(sub_8211ABB8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211ABB8)
  InitAnimationLists(At<UiMovie>(ctx.r3.u32));
}

// UiMovie::SetPictureTexture: r3 the movie, r4 and r5 a picture, r6 a
// texture: the movie's texture its entry names replaced. Returns the old
// one.
extern "C" REX_FUNC(sub_8211AC78) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AC78)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  const uint32_t texture = ctx.r6.u32;
  MoviePicture* picture = reinterpret_cast<MoviePicture*>(FindNode(movie, ctx.r4.s32, ctx.r5.s32));
  const uint32_t old = *PictureTexture(movie, picture);
  *PictureTexture(movie, picture) = texture;
  ctx.r3.u64 = old;
}

// UiMovie::GetPictureTexture: r3 the movie, r4 and r5 a picture. Returns
// its texture.
extern "C" REX_FUNC(sub_8211ACF8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211ACF8)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  MoviePicture* picture = reinterpret_cast<MoviePicture*>(FindNode(movie, ctx.r4.s32, ctx.r5.s32));
  ctx.r3.u64 = uint32_t(*PictureTexture(movie, picture));
}

// UiMovie::SwapTexture: r3 the movie, r4 a texture's index, r5 the new
// texture. Returns the old one.
extern "C" REX_FUNC(sub_8211AD60) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AD60)
  u32_be* slot = Texture(At<UiMovie>(ctx.r3.u32), ctx.r4.u32);
  const uint32_t old = *slot;
  *slot = ctx.r5.u32;
  ctx.r3.u64 = old;
}

// UiMovie::ResetPose: r3 the movie: the live and the saved scenes put back
// to the file's pose.
extern "C" REX_FUNC(sub_8211AD78) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AD78)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  RestorePose(movie->live, movie->file, nullptr, nullptr);
  RestorePose(movie->saved, movie->file, nullptr, nullptr);
}

// UiMovie::ShowNode and HideNode: r3 the movie, r4 and r5 the node.
// Returns it.
extern "C" REX_FUNC(sub_8211ADD0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211ADD0)
  MovieNode* node = FindNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
  node->visible = 1;
  ctx.r3.u64 = AddressOf(node);
}

extern "C" REX_FUNC(sub_8211AE18) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AE18)
  MovieNode* node = FindNode(At<UiMovie>(ctx.r3.u32), ctx.r4.s32, ctx.r5.s32);
  node->visible = 0;
  ctx.r3.u64 = AddressOf(node);
}

// UiMovie::FreeTexts: r3 the movie.
extern "C" REX_FUNC(sub_8211AE60) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AE60)
  FreeTexts(g, At<UiMovie>(ctx.r3.u32));
}

// UiMovies::Reset: no arguments.
extern "C" REX_FUNC(sub_8211AED0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AED0)
  ResetMovies(g);
}

// MovieWords::MovieWords: r3 the array, returned.
extern "C" REX_FUNC(sub_8211AFE8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211AFE8)
  const uint32_t words = ctx.r3.u32;
  MakeWords(g, At<MovieWords>(words));
  ctx.r3.u64 = words;
}

// MovieWords::Append: r3 the array, r4 the word's address.
extern "C" REX_FUNC(sub_8211B038) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B038)
  AppendWord(g, At<MovieWords>(ctx.r3.u32), *At<u32_be>(ctx.r4.u32));
}

// TextItem::operator=: r3 the item copied to, returned; r4 the item.
extern "C" REX_FUNC(sub_8211B0A0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B0A0)
  CopyTextItem(At<MovieTextItem>(ctx.r3.u32), At<MovieTextItem>(ctx.r4.u32));
}

// TextItem's deleting destructor: r3 the items, r4 the flags. Returns the
// block.
extern "C" REX_FUNC(sub_8211B160) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B160)
  ctx.r3.u64 = DeleteTextItems(g, At<MovieTextItem>(ctx.r3.u32), ctx.r4.u32);
}

// TextList::TextList: r3 the list, returned.
extern "C" REX_FUNC(sub_8211B1E8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B1E8)
  const uint32_t list = ctx.r3.u32;
  MakeTextList(g, At<MovieTextList>(list));
  ctx.r3.u64 = list;
}

// AnimationList::RemoveAt: r3 the list, r4 the position.
extern "C" REX_FUNC(sub_8211B250) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B250)
  RemoveAt(At<AnimationList>(ctx.r3.u32), ctx.r4.u32);
}

// MoviePairs::MoviePairs: r3 the array, returned.
extern "C" REX_FUNC(sub_8211B2B8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B2B8)
  const uint32_t pairs = ctx.r3.u32;
  MakePairs(g, At<MoviePairs>(pairs));
  ctx.r3.u64 = pairs;
}

// MoviePairs::Reserve: r3 the array, r4 the capacity.
extern "C" REX_FUNC(sub_8211B330) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B330)
  ReservePairs(g, At<MoviePairs>(ctx.r3.u32), ctx.r4.u32);
}

// AnimationList::LinkFree: r3 the list, r4 the position (its low 16 bits).
extern "C" REX_FUNC(sub_8211B400) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B400)
  At<AnimationList>(ctx.r3.u32)->Link(int16_t(ctx.r4.u32));
}

// AnimationList::Get: r3 the slot copied to, returned; r4 the list, r5 the
// position.
extern "C" REX_FUNC(sub_8211B468) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B468)
  GetSlot(At<AnimationSlot>(ctx.r3.u32), At<AnimationList>(ctx.r4.u32), ctx.r5.u32);
}

// TextMap::Set: r3 the map, r4 the id's address, r5 the record's (r3 left
// as it came: callers read it on).
extern "C" REX_FUNC(sub_8211B540) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B540)
  SetText(At<MovieTextMap>(ctx.r3.u32), *At<i32_be>(ctx.r4.u32),
          At<MovieTextRecord>(*At<u32_be>(ctx.r5.u32)));
}

// TextMap::Find: r3 the text map, r4 the id's address. Returns the
// record's slot.
extern "C" REX_FUNC(sub_8211B4E0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B4E0)
  ctx.r3.u64 = FindTextSlot(At<MovieTextMap>(ctx.r3.u32), *At<i32_be>(ctx.r4.u32));
}

// UiMovies::Find: r3 the pairs, r4 the key's address. Returns the value's
// address.
extern "C" REX_FUNC(sub_8211B5C0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B5C0)
  ctx.r3.u64 = FindPair(At<MoviePairs>(ctx.r3.u32), *At<i32_be>(ctx.r4.u32));
}

// MoviePairs::Append: r3 the array, r4 the pair's address.
extern "C" REX_FUNC(sub_8211B628) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B628)
  AppendPair(g, At<MoviePairs>(ctx.r3.u32), *At<MoviePairs::Pair>(ctx.r4.u32));
}

// AnimationList::Insert: r3 the list, r4 a slot, r5 the position. Returns
// the list's slot.
extern "C" REX_FUNC(sub_8211B6A0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B6A0)
  ctx.r3.u64 = AddressOf(
      InsertSlot(At<AnimationList>(ctx.r3.u32), At<AnimationSlot>(ctx.r4.u32), ctx.r5.u32));
}

// UiMovie::Unload: r3 the movie.
extern "C" REX_FUNC(sub_8211B710) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B710)
  Unload(g, At<UiMovie>(ctx.r3.u32));
}

// UiMovie::GetText: r3 the movie, r4 the address of a text's id. Returns
// its record.
extern "C" REX_FUNC(sub_8211B830) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B830)
  ctx.r3.u64 = AddressOf(GetText(g, At<UiMovie>(ctx.r3.u32), At<u32_be>(ctx.r4.u32)));
}

// UiMovie::TextString: r3 the movie, r4 a text id. Returns its buffer, or
// an empty string when it has no record.
extern "C" REX_FUNC(sub_8211B8F0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B8F0)
  ctx.r3.u64 = TextString(At<UiMovie>(ctx.r3.u32), ctx.r4.u32);
}

// UiMovie::TextPage: r3 the movie, r4 a text id. Returns the page shown,
// or 0 when it has no record.
extern "C" REX_FUNC(sub_8211B940) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211B940)
  ctx.r3.u64 = TextPage(At<UiMovie>(ctx.r3.u32), ctx.r4.u32);
}

// UiMovie::AnimationTime: r3 the movie, r4 a sub-scene's index or -1, r5
// an animation's index in it. f1 the time of its slot, in the looping list
// or the queue, or -1.0.
extern "C" REX_FUNC(sub_8211BBD0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211BBD0)
  const int32_t scene = ctx.r4.s32, animation = ctx.r5.s32;
  AnimationSlot* slot = FindSlot(At<UiMovie>(ctx.r3.u32), [&](const AnimationSlot& s) {
    return s.scene_index == scene && s.index == animation;
  });
  ctx.f1.f64 = double(AnimationTime(slot));
}

// UiMovie::NamedAnimationTime: r3 the movie, r4 a named animation's index.
// f1 its time, or -1.0.
extern "C" REX_FUNC(sub_8211BCE8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211BCE8)
  const int32_t named = ctx.r4.s32;
  AnimationSlot* slot = FindSlot(At<UiMovie>(ctx.r3.u32),
                                 [&](const AnimationSlot& s) { return s.named_index == named; });
  ctx.f1.f64 = double(AnimationTime(slot));
}

// UiMovie::IsNamedActive: r3 the movie, r4 a named animation's index.
// Returns 1 when it's looping or queued.
extern "C" REX_FUNC(sub_8211BDE8) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211BDE8)
  const int32_t named = ctx.r4.s32;
  AnimationSlot* slot = FindSlot(At<UiMovie>(ctx.r3.u32),
                                 [&](const AnimationSlot& s) { return s.named_index == named; });
  ctx.r3.u64 = slot != nullptr ? 1 : 0;
}

// UiMovie::AssignLayers: r3 the movie, r4 the scene, r5 a group (or the
// scene), r6 a layer, r7 nonzero to number by bucket.
extern "C" REX_FUNC(sub_8211BEE0) {
  SBX_UI_MOVIE_NODES_FALLBACK(sub_8211BEE0)
  AssignLayers(g, At<UiMovie>(ctx.r3.u32), At<MovieGroup>(ctx.r4.u32), At<MovieGroup>(ctx.r5.u32),
               ctx.r6.u32, ctx.r7.u32 != 0);
}
