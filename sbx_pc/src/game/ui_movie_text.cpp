// The UI movies' texts, animation starts and stops, and the movie table as
// our own C++ (under --native_ui_movies, defined in ui_movies.cpp): the
// text setters (formatted strings, 8-bit strings, numbers, scrolling,
// pages, the racers' names), the animations' starts, the moves
// between the queue and the looping list, the finishes, the draw, and the
// movie table's open, load, load callback, delete and destructor.
//
// Also the queued text draw, UiMovie::Play, LoopNamed, FinishNamed, Run and
// the HUD's rear-view mirror. Stay recompiled: the named animations' set-up
// (sub_8211D3D0, sub_8211E538: std::string temporaries in their frames,
// made over stack bytes they never write). UiMovie::Update and the ticker
// text's step and offset are called through Guest::Call (subtick wraps
// them). Nothing is kept between calls.

#include "game/ui_movie_text.h"

#include "base/settings.h"
#include "game/ui_movie_draw.h"
#include "game/ui_movie_nodes.h"
#include "game/ui_movies.h"
#include "hooks/native_calls.h"

#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>

SBX_SETTING_DECLARE(bool, native_ui_movies);

namespace game {
namespace {

using guest::AddressOf;
using guest::At;
using guest::Element;
using guest::Guest;
using guest::StackFrame;

bool On() { return SBX_GET(native_ui_movies); }

// The read-only float the starts and finishes load (0x82000A24).
constexpr float kZero = 0.0f;
// A scrolling text's speed (0x820021B8) and the frames it waits first.
constexpr float kScrollSpeed = 4.0f;
constexpr uint32_t kScrollDelay = 45;
// The game's string ids for a number: 8 for 0, 9 for any other.
constexpr uint32_t kStringZero = 8;
constexpr uint32_t kStringNumber = 9;
// UiMovie::OnLoaded (sub_8211E6C0), the movie file's load callback.
constexpr uint32_t kOnLoaded = 0x8211E6C0;
// VirtualAlloc's MEM_COMMIT and PAGE_READWRITE; VirtualFree's MEM_RELEASE.
constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kPageReadWrite = 4;
constexpr uint32_t kMemRelease = 0x8000;

uint32_t StringTableAddress() { return *At<u32_be>(kStringTable); }
uint32_t FontAddress() { return *At<u32_be>(kFont); }
MoviePairs* MovieTable() { return At<MoviePairs>(kUiMovieTable); }

// --- Nodes and texts ---

// The node the setters name: child `child` of the live scene, or of the
// live scene's child `scene` when that isn't -1.
MovieTextNode* FindNode(UiMovie* movie, int32_t scene, uint32_t child) {
  MovieGroup* parent = movie->live;
  if (scene != -1) {
    parent = reinterpret_cast<MovieGroup*>(Child(parent, uint32_t(scene)));
  }
  return reinterpret_cast<MovieTextNode*>(Child(parent, child));
}

// The node's text record (UiMovie::GetText: made, and the node given an
// id, when it has none), its buffer cleared (memset).
MovieTextRecord* ClearedText(Guest& g, UiMovie* movie, MovieTextNode* node) {
  MovieTextRecord* text = GetText(g, movie, &node->text_id);
  g.Call(sub_8258C950, text->buffer, 0u, MovieTextRecord::kBufferSize);  // memset
  return text;
}

// The node's record kept in the movie's text map by its id (read again:
// GetText may have just given it one).
void MapText(UiMovie* movie, const MovieTextNode* node, MovieTextRecord* text) {
  SetText(&movie->text_map, int32_t(uint32_t(node->text_id)), text);
}

// UiMovie::SetNodeText (sub_8211C9B0) and SetText's work: the text
// cleared and string `string_id` formatted into it with the va_list
// `args` (Strings::FormatV), shown from its first page.
void FormatText(Guest& g, UiMovie* movie, MovieTextNode* node, uint32_t string_id,
                uint32_t args) {
  MovieTextRecord* text = ClearedText(g, movie, node);
  text->page = 0;
  StringsFormatV(g, StringTableAddress(), text->buffer, string_id & 0xFFFF, args);
  MapText(movie, node, text);
}

// UiMovie::SetNodeTextV (sub_8211CDA0): the same with no page reset.
void FormatTextV(Guest& g, UiMovie* movie, MovieTextNode* node, uint32_t string_id,
                 uint32_t args) {
  MovieTextRecord* text = ClearedText(g, movie, node);
  StringsFormatV(g, StringTableAddress(), text->buffer, string_id & 0xFFFF, args);
  MapText(movie, node, text);
}

// The text cleared and an 8-bit string converted into it by the font
// (sub_82108B90).
void SetCharText(Guest& g, UiMovie* movie, MovieTextNode* node, uint32_t string) {
  MovieTextRecord* text = ClearedText(g, movie, node);
  g.Call(sub_82108B90, FontAddress(), text->buffer, string);  // Font's text from 8-bit characters
  MapText(movie, node, text);
}

// The text cleared and a number formatted into it (Strings::Format):
// string 8 for 0, else string 9 with the value (all of its register, as
// the original hands it on). The argument goes in a va_list on the guest
// stack (Strings::Format reads its arguments by guest address).
void SetNumberText(Guest& g, UiMovie* movie, MovieTextNode* node, uint64_t value) {
  StackFrame frame(g.ctx, 96);
  const uint32_t args = frame.Local(80);
  MovieTextRecord* text = ClearedText(g, movie, node);
  const bool zero = int32_t(uint32_t(value)) == 0;
  *At<guest::be<uint64_t>>(args) = zero ? 0 : value;
  StringsFormat(g, StringTableAddress(), text->buffer, zero ? kStringZero : kStringNumber, args);
  MapText(movie, node, text);
}

// UiMovie::ClearText (sub_8211C640): the text cleared, its scroll restarted.
void ClearText(Guest& g, UiMovie* movie, MovieTextNode* node) {
  MovieTextRecord* text = ClearedText(g, movie, node);
  text->scroll_frames = 0;
  MapText(movie, node, text);
}

// UiMovie::ClearNodeText (sub_8211CE18): the text cleared, nothing else.
void ClearNodeText(Guest& g, UiMovie* movie, MovieTextNode* node) {
  MapText(movie, node, ClearedText(g, movie, node));
}

// sub_8211C6C0: the frames a text waits before it scrolls, and its speed.
void SetScrollSpeed(Guest& g, UiMovie* movie, MovieTextNode* node, uint32_t delay, float speed) {
  MovieTextRecord* text = GetText(g, movie, &node->text_id);
  text->scroll_delay = int32_t(delay);
  text->scroll_speed = speed;
  MapText(movie, node, text);
}

// UiMovie::ScrollText (sub_8211C740): whether the text scrolls, at the
// usual delay and speed.
void ScrollText(Guest& g, UiMovie* movie, MovieTextNode* node, uint32_t scrolling) {
  MovieTextRecord* text = GetText(g, movie, &node->text_id);
  text->scrolling = scrolling;
  text->scroll_delay = int32_t(kScrollDelay);
  text->scroll_speed = kScrollSpeed;
  MapText(movie, node, text);
}

// sub_8211C7C8: the text's scroll restarted.
void RestartScroll(Guest& g, UiMovie* movie, MovieTextNode* node) {
  MovieTextRecord* text = GetText(g, movie, &node->text_id);
  text->scroll_frames = 0;
  MapText(movie, node, text);
}

// Whether there's a page after the one shown (Strings::PageCount counts
// the text's page breaks).
bool HasNextPage(const MovieTextRecord* text) {
  const int32_t pages = int32_t(PageCount(At<u16_be>(text->buffer)));
  return int32_t(text->page + 1) <= pages;
}

// sub_8211CAF8: shows the text's next page. Whether there was one.
bool ShowNextPage(Guest& g, UiMovie* movie, MovieTextNode* node) {
  MovieTextRecord* text = GetText(g, movie, &node->text_id);
  const bool more = HasNextPage(text);
  if (more) {
    text->page += 1;
  }
  MapText(movie, node, text);
  return more;
}

// sub_8211CB88: whether the text has a page after the one shown.
bool HasMorePages(Guest& g, UiMovie* movie, MovieTextNode* node) {
  return HasNextPage(GetText(g, movie, &node->text_id));
}

// A racer's name record (the save data's, the tuning screen's): two parts,
// each a kind, a signed index and a flag.
struct NameRecord {
  uint8_t unknown_0[4];
  u32_be kind;
  guest::i16_be index;
  uint8_t flag;
  uint8_t unknown_11;
  u32_be second_kind;
  guest::i16_be second_index;
  uint8_t second_flag;
};
GUEST_OFFSET(NameRecord, kind, 4);
GUEST_OFFSET(NameRecord, index, 8);
GUEST_OFFSET(NameRecord, flag, 10);
GUEST_OFFSET(NameRecord, second_kind, 12);
GUEST_OFFSET(NameRecord, second_index, 16);
GUEST_OFFSET(NameRecord, second_flag, 18);

// The kinds' string ids: `base` + index when the index is 0 to count - 1,
// else -1 (none).
int32_t NameString(int32_t base, int32_t index, int32_t count) {
  return index >= 0 && index < count ? base + index : -1;
}

constexpr uint32_t kNameKindWhole = 5;        // One string, 6012 + index (0 to 70).
constexpr uint32_t kTwoPartNameString = 533;  // Its two parts' strings.

int32_t FirstNamePart(const NameRecord* name) {
  const int32_t index = name->index;
  switch (uint32_t(name->kind)) {
    case 0:
      return NameString(name->flag != 0 ? 5652 : 5832, index, 90);
    case 1:
      return NameString(6083, index, 4);
    case 4:
      return NameString(5547, index, 105);
    default:
      return -1;
  }
}

int32_t SecondNamePart(const NameRecord* name) {
  const int32_t index = name->second_index;
  switch (int32_t(uint32_t(name->second_kind))) {
    case 2:
      return NameString(name->second_flag != 0 ? 5742 : 5922, index, 90);
    case 3:
      return NameString(6087, index, 6);
    default:
      return -1;
  }
}

// UiMovie::SetNameText (sub_8211CC68): a name record shown in a text node
// as UiMovie::SetNodeText shows a string: a whole name (kind 5) as its
// string; else the two parts' strings in string 533, when both have one.
// The two parts go in a va_list on the guest stack (Strings::FormatV reads
// its arguments by guest address); the whole name's string reads none.
// A node that isn't a text goes to the recompiled SetNodeText (the game's
// assert).
void SetNameText(Guest& g, UiMovie* movie, MovieTextNode* node, const NameRecord* name) {
  StackFrame frame(g.ctx, 112);
  const uint32_t args = frame.Local(80);
  guest::be<uint64_t>* slots = At<guest::be<uint64_t>>(args);
  uint32_t string_id;
  int32_t first = 0, second = 0;
  if (name->kind == kNameKindWhole) {
    const int32_t index = name->index;
    if (index < 0 || index >= 71) {
      return;
    }
    string_id = uint32_t(6012 + index);
  } else {
    first = FirstNamePart(name);
    second = SecondNamePart(name);
    if (first == -1 || second == -1) {
      return;
    }
    string_id = kTwoPartNameString;
    slots[0] = uint64_t(int64_t(first));
    slots[1] = uint64_t(int64_t(second));
  }
  if (node->node.tag != kTagText) {
    g.Call(__imp__sub_8211C9B0, movie, node, string_id, first, second);
    return;
  }
  FormatText(g, movie, node, string_id, args);
}

// --- Animations ---

// UiMovie::ApplyAnimation of a slot at its end: its time the animation's
// length, or 0 when it runs backwards (applied as running forwards).
void ApplyAtEnd(Guest& g, const AnimationSlot* slot) {
  const double time =
      slot->backwards != 0 ? double(kZero) : double(float(slot->animation->length));
  ApplyAnimation(g, slot->target, slot->animation, time, 0);
}

// A list's first slot taken off (its link put first on the free links).
void TakeFirst(AnimationList& list) {
  const int16_t first = list.head;
  AnimationList::Node* node = list.NodeAt(first);
  list.head = node->next;
  node->next = list.free_head;
  list.free_head = first;
  list.count = int16_t(list.count - 1);
}

uint32_t Count(const AnimationList& list) { return uint32_t(int32_t(int16_t(list.count))); }

// A slot queued at the end of the movie's queue and copied as the last
// started (memcpy, through Guest::Call: an SBX hook wraps it, so the slot
// is on the guest stack).
void Queue(Guest& g, UiMovie* movie, AnimationSlot* slot) {
  InsertSlot(&movie->queue, slot, Count(movie->queue));
  g.Call(sub_8258CB90, &movie->last_started, slot, uint32_t(sizeof(AnimationSlot)));  // memcpy
}

// UiMovie::Play (sub_8211DB38): animation `animation` of the live scene's
// child `scene` (the live scene itself for -1) queued as PlayNamed queues a
// named one, unless it's looping. Its time starts at 0, or at its length
// when it runs backwards.
void Play(Guest& g, UiMovie* movie, int32_t scene, int32_t animation, uint32_t with_next,
          uint32_t backwards, uint32_t remove_at_end, uint32_t hold) {
  StackFrame frame(g.ctx, 192);
  MovieGroup* scene_node = movie->live;
  if (scene != -1) {
    scene_node = reinterpret_cast<MovieGroup*>(Child(scene_node, uint32_t(scene)));
  }
  MovieAnimation* played = At<MovieAnimation>(scene_node->Animation(uint32_t(animation)));
  const uint32_t looping = Count(movie->looping);
  for (uint32_t i = 0; i < looping; i++) {
    const AnimationSlot* slot = movie->looping.ItemAtPosition(i);
    if (slot->scene_index == scene && slot->index == animation) {
      return;
    }
  }
  AnimationSlot* slot = At<AnimationSlot>(frame.Local(80));
  slot->index = animation;
  slot->scene_index = scene;
  slot->named_index = -1;
  slot->target = scene_node;
  slot->animation = played;
  slot->time = backwards != 0 ? played->length : f32_be(kZero);
  slot->with_next = with_next;
  slot->backwards = backwards;
  slot->remove_at_end = remove_at_end;
  slot->hold = hold;
  Queue(g, movie, slot);
}

// UiMovie::PlayNamed (sub_8211DC88): named animation `named` queued at the
// end of the queue and copied as the last started, unless it's looping.
// Its time starts at 0, or at its length (the word as it is) when it runs
// backwards. Nothing for a negative index.
void PlayNamed(Guest& g, UiMovie* movie, int32_t named, uint32_t with_next, uint32_t backwards,
               uint32_t remove_at_end, uint32_t hold) {
  StackFrame frame(g.ctx, 160);
  if (named < 0) {
    return;
  }
  const uint32_t looping = Count(movie->looping);
  for (uint32_t i = 0; i < looping; i++) {
    if (movie->looping.ItemAtPosition(i)->named_index == named) {
      return;
    }
  }
  NamedAnimation* entry = Element<NamedAnimation>(AddressOf(movie->named), uint32_t(named));
  AnimationSlot* slot = At<AnimationSlot>(frame.Local(80));
  slot->index = -1;
  slot->scene_index = -1;
  slot->named_index = named;
  slot->target.address = entry->scene;
  slot->animation = entry->animation;
  slot->time = backwards != 0 ? entry->animation->length : f32_be(kZero);
  slot->with_next = with_next;
  slot->backwards = backwards;
  slot->remove_at_end = remove_at_end;
  slot->hold = hold;
  Queue(g, movie, slot);
}

// The first slot in the queue (to the looping list) or the looping list (to
// the queue) that `matches` moved to the other's end: each copied out to
// test it (AnimationList::Get), then taken off and appended.
template <typename Match>
void MoveSlot(UiMovie* movie, bool to_looping, Match matches) {
  AnimationList& from = to_looping ? movie->queue : movie->looping;
  AnimationList& to = to_looping ? movie->looping : movie->queue;
  AnimationSlot copy;
  const uint32_t count = Count(from);
  for (uint32_t i = 0; i < count; i++) {
    GetSlot(&copy, &from, i);
    if (matches(copy)) {
      RemoveAt(&from, i);
      InsertSlot(&to, &copy, Count(to));
      return;
    }
  }
}

// UiMovie::MoveAnimation (sub_8211DDA0): by the scene's and animation's
// indices.
void MoveAnimation(UiMovie* movie, int32_t scene, int32_t animation, bool to_looping) {
  MoveSlot(movie, to_looping, [&](const AnimationSlot& slot) {
    return slot.scene_index == scene && slot.index == animation;
  });
}

// UiMovie::MoveNamedAnimation (sub_8211DEA8): by the named animation's index.
void MoveNamedAnimation(UiMovie* movie, int32_t named, bool to_looping) {
  MoveSlot(movie, to_looping, [&](const AnimationSlot& slot) { return slot.named_index == named; });
}

// UiMovie::LoopLast (sub_8211DFA8): the last started animation looped, by
// name when it was started by name.
void LoopLast(UiMovie* movie) {
  const AnimationSlot& last = movie->last_started;
  if (last.named_index != -1) {
    MoveNamedAnimation(movie, last.named_index, true);
    return;
  }
  MoveAnimation(movie, last.scene_index, last.index, true);
}

// The first queued slot that `matches` applied at its end and taken off:
// the live scene keeps its end pose.
template <typename Match>
void FinishQueued(Guest& g, UiMovie* movie, Match matches) {
  const uint32_t count = Count(movie->queue);
  for (uint32_t i = 0; i < count; i++) {
    AnimationSlot* slot = movie->queue.ItemAtPosition(i);
    if (matches(*slot)) {
      ApplyAtEnd(g, slot);
      RemoveAt(&movie->queue, i);
      return;
    }
  }
}

// UiMovie::Finish (sub_8211DFF8): the animation moved off the looping list
// to the queue, then finished there.
void Finish(Guest& g, UiMovie* movie, int32_t scene, int32_t animation) {
  MoveAnimation(movie, scene, animation, false);
  FinishQueued(g, movie, [&](const AnimationSlot& slot) {
    return slot.scene_index == scene && slot.index == animation;
  });
}

// UiMovie::FinishNamed (sub_8211E0E0): the same by a named animation's index.
void FinishNamed(Guest& g, UiMovie* movie, int32_t named) {
  MoveNamedAnimation(movie, named, false);
  FinishQueued(g, movie, [&](const AnimationSlot& slot) { return slot.named_index == named; });
}

// UiMovie::FinishAll (sub_8211E1B8): every looping animation moved to the
// queue's end, then every queued one applied at its end and taken off,
// first to last. Each is copied out first (memcpy, through Guest::Call: an
// SBX hook wraps it, so the copy is on the guest stack, the original's
// 176-byte frame, +80).
void FinishAll(Guest& g, UiMovie* movie) {
  StackFrame frame(g.ctx, 176);
  AnimationSlot* copy = At<AnimationSlot>(frame.Local(80));
  AnimationList& queue = movie->queue;
  AnimationList& looping = movie->looping;
  for (uint32_t n = Count(looping); n != 0; n--) {
    g.Call(sub_8258CB90, copy, looping.ItemAtPosition(0), uint32_t(sizeof(AnimationSlot)));
    TakeFirst(looping);
    InsertSlot(&queue, copy, Count(queue));
  }
  for (uint32_t n = Count(queue); n != 0; n--) {
    g.Call(sub_8258CB90, copy, queue.ItemAtPosition(0), uint32_t(sizeof(AnimationSlot)));
    ApplyAtEnd(g, copy);
    TakeFirst(queue);
  }
}

// UiMovie::Draw (sub_8211D440): the movie drawn as its update draws it,
// without applying animations: the live pose saved, the scene drawn, its
// texts drawn (not draw-only) and the clip reset after both draw keys.
void Draw(Guest& g, UiMovie* movie) {
  RestorePose(movie->saved, movie->live, nullptr, nullptr);
  DrawMovie(g, &movie->draw_context, movie->live, &movie->textures, &movie->texts, 0xFFFFFFFFu);
  DrawQueuedText(g, movie, &movie->texts, 0);
  ResetClip(g, 676);
  ResetClip(g, 148);
}

// --- The movie table ---

// sub_8211C338: the table's value for a key set, or the pair appended when
// the key isn't there.
void SetMoviePair(Guest& g, MoviePairs* table, int32_t key, uint32_t value) {
  const uint32_t count = table->count;
  for (uint32_t i = 0; i < count; i++) {
    MoviePairs::Pair* pair = Element<MoviePairs::Pair>(table->data.address, i);
    if (pair->key == key) {
      pair->value = value;
      return;
    }
  }
  MoviePairs::Pair pair;
  pair.key = key;
  pair.value = value;
  AppendPair(g, table, pair);
}

// The table's movie for an id (UiMovies::Find), or 0: an id that isn't
// there gives a word of the game's that is 0.
uint32_t FindMovie(uint32_t id) { return *At<u32_be>(FindPair(MovieTable(), int32_t(id))); }

// UiMovies::IsLoaded (sub_8211C068): whether the movie for an id has
// loaded. An id that isn't there reads a movie at 0's flag, as the game.
bool IsLoaded(uint32_t id) { return MovieAt(FindMovie(id))->loaded != 0; }

// UiMovie::~UiMovie (sub_8211E320): its named animations destroyed, its
// draw context's destructor (sub_821611F0), its text items' array freed
// (TextItem's deleting destructor, r4 = 3) and its textures' array
// (operator delete[]).
void DestroyMovie(Guest& g, UiMovie* movie) {
  DestroyNamedAnimations(g, movie->named);
  g.Call(sub_821611F0, &movie->draw_context);  // DrawContext::~DrawContext
  if (movie->texts.items != nullptr) {
    DeleteTextItems(g, movie->texts.items, 3);
  }
  g.Call(sub_8258C940, movie->textures.data);  // operator delete[]
}

// UiMovies::Delete (sub_8211E418): the movie for an id unloaded,
// destroyed and freed (operator delete), and the table's value for the id
// set to 0.
void DeleteMovie(Guest& g, uint32_t id) {
  const uint32_t movie = FindMovie(id);
  if (movie == 0) {
    return;
  }
  Unload(g, At<UiMovie>(movie));
  DestroyMovie(g, At<UiMovie>(movie));
  g.Call(sub_8258C868, movie);  // operator delete
  SetMoviePair(g, MovieTable(), int32_t(id), 0);
}

// UiMovies::DeleteAll (sub_8211E488): each movie in the table deleted by
// its id (the table read again after each: Delete sets values, it doesn't
// remove pairs).
void DeleteAllMovies(Guest& g) {
  MoviePairs* table = MovieTable();
  for (uint32_t i = 0; i < table->count; i++) {
    DeleteMovie(g, uint32_t(int32_t(Element<MoviePairs::Pair>(table->data.address, i)->key)));
  }
}

// UiMovie::Load (sub_8211E958): the movie's file read into a buffer of its
// size (Archive::GetSize, VirtualAlloc) by Archive::Load, with
// UiMovie::OnLoaded as its callback and the movie as the callback's
// context.
void LoadMovie(Guest& g, uint32_t movie, uint32_t asset) {
  const uint32_t size = g.Call(sub_820A0860, asset);  // Archive::GetSize
  const uint32_t buffer =
      g.Call(sub_82292DD8, 0u, size, kMemCommit, kPageReadWrite);  // VirtualAlloc
  g.Call(sub_8209DF80, asset, buffer, kOnLoaded, movie, 0u, 0u);   // Archive::Load
}

// UiMovies::Open (sub_8211E9C8): unless the table has a movie for the id,
// a movie made (operator new, UiMovie::UiMovie; 0 when the allocation
// fails, used anyway), its lists set up, its id kept, its file loaded and
// the movie put in the table. Whether it was opened.
bool OpenMovie(Guest& g, uint32_t id, uint32_t asset) {
  if (FindMovie(id) != 0) {
    return false;
  }
  const uint32_t movie = g.Call(sub_8258C000, uint32_t(sizeof(UiMovie)));  // operator new
  if (movie != 0) {
    ConstructMovie(g, At<UiMovie>(movie));
  }
  InitAnimationLists(MovieAt(movie));
  MovieAt(movie)->id = id;
  LoadMovie(g, movie, asset);
  SetMoviePair(g, MovieTable(), int32_t(id), movie);
  return true;
}

// The texture set's XPR file: its record count at +12.
struct TextureSetFile {
  uint8_t unknown_0[12];
  u32_be record_count;
};
GUEST_OFFSET(TextureSetFile, record_count, 12);

// What Texture::GetLevelDesc writes (D3DSURFACE_DESC): width and height at
// +24 and +28.
struct LevelDescription {
  uint8_t unknown_0[24];
  u32_be width;
  u32_be height;
};
GUEST_OFFSET(LevelDescription, width, 24);

// UiMovie::OnLoaded (sub_8211E6C0), Archive::Load's callback: the loaded
// XMD archive's X2D file (entry 0, up to the end of its TRM chunk) copied
// three times into new buffers, each fixed up and its scene kept (the live
// one, the saved pose, the file's own); the scenes' layers set, the texts'
// ids cleared and the texts freed, the named animations made
// (sub_8211E538). An archive with more than one entry has a texture set as
// entry 1: an XPR made (XPR::XPR, XPR::Create) and each texture appended
// to the movie's textures with its width and height (Texture::GetLevelDesc,
// into a description on the guest stack, the original's 192-byte frame at
// +96). The archive freed, the movie marked loaded.
void OnLoaded(Guest& g, uint32_t archive, UiMovie* movie) {
  StackFrame frame(g.ctx, 192);
  XmdFile* xmd = At<XmdFile>(archive);
  CheckXmd(xmd);  // The answer isn't used.
  MovieChunk* file = At<MovieChunk>(XmdEntry(xmd, 0));
  MovieChunk* end_chunk = FindChunk(file, kTagEnd);
  // The file up to the end of its end chunk.
  const uint32_t size = end_chunk->next - AddressOf(file) + AddressOf(end_chunk);
  for (u32_be& copy : movie->file_copies) {
    copy = g.Call(sub_82292DD8, 0u, size, kMemCommit, kPageReadWrite);  // VirtualAlloc
  }
  for (u32_be& copy : movie->file_copies) {
    g.Call(sub_8258CB90, copy, file, size);  // memcpy
  }
  for (u32_be& copy : movie->file_copies) {
    FixUpFile(At<MovieFile>(copy));
  }
  movie->live = FileScene(At<MovieFile>(movie->file_copies[0]));
  movie->saved = FileScene(At<MovieFile>(movie->file_copies[1]));
  movie->file = FileScene(At<MovieFile>(movie->file_copies[2]));
  for (MovieGroup* scene : {movie->live.get(), movie->saved.get(), movie->file.get()}) {
    AssignLayers(g, movie, scene, scene, 0, true);
  }
  ClearTextIds(movie);
  FreeTexts(g, movie);
  movie->named_count = 0;
  g.Call(sub_8211E538, movie, movie->live);  // The named animations' set-up.

  if (g.Call(sub_822B8A88, archive) != 1) {  // XmdFile::Count
    const uint32_t records = At<TextureSetFile>(XmdEntry(xmd, 1))->record_count;
    uint32_t set = g.Call(sub_8258C000, 24u);  // operator new
    if (set != 0) {
      set = g.Call(sub_8257C780, set);  // XPR::XPR
    }
    movie->texture_set = set;
    g.Call(sub_8257C880, movie->texture_set, XmdEntry(xmd, 1));  // XPR::Create
    LevelDescription* description = At<LevelDescription>(frame.Local(96));
    for (uint32_t i = 0; i < records; i++) {
      MovieTexture* texture = At<MovieTexture>(g.Call(sub_8258C000, 12u));  // operator new
      texture->texture = 0;
      texture->width = 0;
      texture->height = 0;
      texture->texture = g.Call(sub_8257CA30, movie->texture_set, i);  // XPR::GetTexture
      g.Call(sub_822A3160, texture->texture, 0u, description);        // Texture::GetLevelDesc
      texture->width = description->width;
      texture->height = description->height;
      AppendWord(g, &movie->textures, AddressOf(texture));
    }
  }
  g.Call(sub_82292D70, archive, 0u, kMemRelease);  // VirtualFree
  movie->loaded = 1;
}

// --- The queued texts and the mirror ---

// fctiwz and fctidz: toward zero, held at the type's ends; a NaN gives the
// smallest value.
int32_t TruncateToInt32(double value) {
  if (std::isnan(value) || value <= -2147483648.0) {
    return INT32_MIN;
  }
  return value >= 2147483647.0 ? INT32_MAX : int32_t(value);
}

int64_t TruncateToInt64(double value) {
  constexpr double kTwoTo63 = 9223372036854775808.0;
  if (std::isnan(value) || value < -kTwoTo63) {
    return INT64_MIN;
  }
  return value >= kTwoTo63 ? INT64_MAX : int64_t(value);
}

// The read-only floats the text draw and the mirror load.
constexpr float kOne = 1.0f;               // 0x82000A20
constexpr float kByteToUnit = 1 / 255.0f;  // 0x820021C0
constexpr float kByteMax = 255.0f;         // 0x82000C54
constexpr float kSmallGlyphs = 0.65f;      // 0x820029E4
constexpr float kSecondOffset = 2.0f;      // 0x820021A8
constexpr float kMirrorScale = 0.8f;       // 0x82001068
constexpr uint32_t kTextBufferSize = 2048;
constexpr uint32_t kStyleUnknown64 = 24;
constexpr uint8_t kStyleUnknown92 = 128;
constexpr uint32_t kMirrorQuadKey = 597;
constexpr uint32_t kMirrorBlend = 4;
constexpr uint32_t kMirrorVertices = 16;  // The frame's room for the quad's vertices.

// A word or float into one of TextStyle's unnamed 4-byte fields.
void StoreWord(uint8_t (&field)[4], uint32_t value) { *reinterpret_cast<u32_be*>(field) = value; }

// One color channel as a byte: the node's (0 to 255) as 0 to 1, times the
// queued text's, times 255, toward zero (fctidz), its low byte.
uint8_t ColorByte(int32_t node_value, float item_value) {
  float value = float(node_value) * kByteToUnit;
  value = value * item_value;
  value = value * kByteMax;
  return uint8_t(uint64_t(TruncateToInt64(double(value))));
}

// The font page a text node's size flags pick (and its glyphs' scale).
uint32_t FontPageFor(uint32_t flags, TextStyle* style) {
  if ((flags & MovieTextNode::kPage0) != 0) {
    return 0;
  }
  if ((flags & MovieTextNode::kPage1) != 0) {
    return 1;
  }
  if ((flags & MovieTextNode::kPage2) == 0) {
    if ((flags & MovieTextNode::kPage3) != 0) {
      return 3;
    }
    if ((flags & MovieTextNode::kSmall) != 0) {
      style->glyph_scale_x = kSmallGlyphs;
      style->glyph_scale_y = kSmallGlyphs;
    }
  }
  return 2;
}

// The HUD's rear-view mirror (sub_8211C0B8): a primitive node drawn as a
// quad textured with render target 0 (sub_8209F8A0; its size from
// Texture::GetLevelDesc): each vertex at its place plus the node's,
// through a scale of 0.8 (Vec4::Transform), its texture coordinates its
// pixels over the texture's size, the node's color and alpha. Recorded with
// blend 4 under key 597. The quad, its arrays, the scale and the
// description are on the guest stack (the original's 1,248-byte frame):
// the recompiled callees are handed them.
void DrawMirror(Guest& g, MoviePrimitive* node) {
  StackFrame frame(g.ctx, 1248);
  MovieQuad* quad = At<MovieQuad>(frame.Local(160));
  LevelDescription* description = At<LevelDescription>(frame.Local(288));
  Matrix* scale = At<Matrix>(frame.Local(320));
  Vector* positions = At<Vector>(frame.Local(384));
  MovieColorWords* colors = At<MovieColorWords>(frame.Local(640));
  Vector* texcoords = At<Vector>(frame.Local(896));

  for (uint32_t i = 0; i < kMirrorVertices; i++) {
    positions[i].w = kOne;
  }
  MakeQuad(quad);
  *scale = Matrix{{kMirrorScale, kZero, kZero, kZero},
                  {kZero, kMirrorScale, kZero, kZero},
                  {kZero, kZero, kOne, kZero},
                  {kZero, kZero, kZero, kOne}};
  const uint32_t texture = g.Call(sub_8209F8A0, 0u);  // CameraPasses::RenderTarget
  g.Call(sub_822A3160, texture, 0u, description);     // Texture::GetLevelDesc
  quad->texture = texture;
  quad->positions = positions;
  quad->colors = colors;
  quad->texcoords = texcoords;
  quad->vertex_count = node->vertex_count;
  if (node->vertex_count != 0) {
    const float width = float(double(uint32_t(description->width)));
    const float height = float(double(uint32_t(description->height)));
    for (uint32_t i = 0; i < node->vertex_count; i++) {
      const MovieVertex* vertex = node->VertexAt(i);
      Vector* position = &positions[i];
      position->x = float(vertex->x) + float(node->node.x);
      position->y = float(vertex->y) + float(node->node.y);
      position->z = kZero;
      position->w = kOne;
      g.Call(sub_820AB518, position, scale, position);  // Vec4::Transform
      texcoords[i].x = float(int32_t(vertex->u)) / width;
      texcoords[i].y = float(int32_t(vertex->v)) / height;
      colors[i].r = uint32_t(int32_t(node->node.red));
      colors[i].g = uint32_t(int32_t(node->node.green));
      colors[i].b = uint32_t(int32_t(node->node.blue));
      colors[i].a = uint32_t(TruncateToInt32(double(float(node->node.alpha) * kByteMax)));
      quad->clipped = 0;
      quad->clip = {0, 0, 0, 0};
    }
  }
  quad->blend = kMirrorBlend;
  quad->unknown_72 = 0;
  DrawQuad(g, quad, kMirrorQuadKey);
}

// The registers a variadic original stores in its caller's parameter area
// (rN at r1 + 8 x N - 8, whole), from `first` to r10. Returns the va_list
// it hands on: the first one's address.
uint32_t SpillArguments(PPCContext& ctx, int first) {
  const PPCRegister* registers[] = {&ctx.r6, &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
  for (int n = first; n <= 10; n++) {
    *At<guest::be<uint64_t>>(ctx.r1.u32 + 8 * n - 8) = registers[n - 6]->u64;
  }
  return ctx.r1.u32 + 8 * first - 8;
}

// The registers r3 to r10 as an override came in, for a fallback after
// calls of its own.
struct Arguments {
  explicit Arguments(const PPCContext& ctx)
      : words{ctx.r3.u64, ctx.r4.u64, ctx.r5.u64, ctx.r6.u64,
              ctx.r7.u64, ctx.r8.u64, ctx.r9.u64, ctx.r10.u64} {}
  void Restore(PPCContext& ctx) const {
    PPCRegister* registers[] = {&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6,
                                &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    for (int i = 0; i < 8; i++) {
      registers[i]->u64 = words[i];
    }
  }
  uint64_t words[8];
};

}  // namespace

// UiMovie::DrawQueuedText: `list`'s queued texts drawn and taken off, last
// first. Each is copied out (TextItem::operator=); its matrix's origin
// (Vec4::Transform) is where the text goes; the matrix's scales and the
// item's clip are written into its text node; the font's style is built
// from the node and the item. A scrolling text takes its x from
// TickerText::Offset (sub_8211B9D0) and, unless this is a draw-only pass,
// counts a frame (TickerText::Step sub_8211BB10), both through Guest::Call
// (subtick wraps them). The text's page is copied into a buffer
// (Strings::CopyPage) and drawn (Text::Draw), and the copy destroyed.
//
// The origin, the matrix's copy, the style and the buffer are on the guest
// stack (the original's 2,704-byte frame, +96, +304, +112 and +496): the
// recompiled Vec4::Transform, TickerText::Offset and the font's
// recompiled layout are handed them.
void DrawQueuedText(Guest& g, UiMovie* movie, MovieTextList* list, uint32_t draw_only) {
  StackFrame frame(g.ctx, 2704);
  Vector* origin = At<Vector>(frame.Local(96));
  UiMovieTextDraw* draw = At<UiMovieTextDraw>(frame.Local(112));
  TextStyle* style = &draw->style;
  Matrix* matrix = At<Matrix>(frame.Local(304));
  const uint32_t buffer = frame.Local(496);
  while (list->count != 0) {
    g.Call(sub_8258C950, buffer, 0u, kTextBufferSize);  // memset
    const uint32_t last = list->count - 1;
    list->count = last;
    MovieTextItem item;
    CopyTextItem(&item, Element<MovieTextItem>(list->items.address, last));
    MovieTextNode* node = At<MovieTextNode>(item.node.address);
    *matrix = item.matrix;
    origin->x = kZero;
    origin->y = kZero;
    origin->z = kZero;
    origin->w = kOne;
    g.Call(sub_820AB518, origin, matrix, origin);  // Vec4::Transform
    node->node.scale_y = matrix->up.y;
    node->node.scale_x = matrix->right.x;
    node->node.clip = item.clip;

    style->x = TruncateToInt32(double(float(origin->x)));
    style->y = TruncateToInt32(double(float(origin->y)));
    style->scale_origin_x = node->node.pivot_x;
    style->scale_origin_y = node->node.pivot_y;
    style->rotation = node->node.rotation;
    style->red = ColorByte(node->node.red, item.red);
    style->green = ColorByte(node->node.green, item.green);
    style->blue = ColorByte(node->node.blue, item.blue);
    style->alpha = float(node->node.alpha) * float(item.alpha);
    style->scale_x = node->node.scale_x;
    style->scale_y = node->node.scale_y;
    style->glyph_scale_x = kOne;
    style->glyph_scale_y = kOne;
    style->clip_left = node->node.clip.x0;
    style->clip_top = node->node.clip.y0;
    style->clip_width = node->node.clip.x1;
    style->clip_height = node->node.clip.y1;
    StoreWord(style->unknown_64, kStyleUnknown64);
    style->box_width = TruncateToInt32(double(float(node->box_width)));
    style->box_height = TruncateToInt32(double(float(node->box_height)));
    style->horizontal_align = node->horizontal_align;
    style->vertical_align = node->vertical_align;
    style->letter_spacing = TruncateToInt32(double(float(node->letter_spacing)));
    style->line_spacing = TruncateToInt32(double(float(node->line_spacing)));
    style->unknown_92[0] = kStyleUnknown92;
    style->unknown_92[1] = kStyleUnknown92;
    style->unknown_92[2] = kStyleUnknown92;
    style->second_offset_x = kSecondOffset;
    style->second_offset_y = kSecondOffset;
    StoreWord(style->unknown_104, std::bit_cast<uint32_t>(kOne));
    style->second_pass = 0;
    style->layer = node->node.layer;
    draw->text_id = node->text_id;
    style->page = FontPageFor(node->node.size_flags, style);

    if (TextScrolls(movie, node->text_id) != 0) {
      g.Call(sub_8211B9D0, movie, draw);  // TickerText::Offset
      style->x = TruncateToInt32(g.ctx.f1.f64);
      if (draw_only == 0) {
        g.Call(sub_8211BB10, movie, node->text_id);  // TickerText::Step
      }
    }
    const uint32_t id = node->text_id;
    const int32_t skip = int32_t(TextFirstLine(movie, id));
    const int32_t page = int32_t(TextPage(movie, id));
    const uint32_t text = TextString(movie, id);
    CopyPage(At<u16_be>(buffer), At<u16_be>(text), page, skip);
    TextDraw(g, style, buffer);
    ClearTextItem(&item);
  }
}

}  // namespace game

using namespace game;

#define SBX_UI_MOVIE_TEXT_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                   \
  if (!On()) {                           \
    __imp__##name(ctx, base);            \
    return;                              \
  }                                      \
  Guest g(ctx, base);

// The setters that need a text node: one that isn't loops forever in the
// original (an assert), so the original runs for it.
#define SBX_UI_MOVIE_TEXT_NODE_CHECK(name, node, arguments) \
  if ((node)->node.tag != kTagText) {                       \
    (arguments).Restore(ctx);                               \
    __imp__##name(ctx, base);                               \
    return;                                                 \
  }

// --- Texts ---

// UiMovie::SetText: r3 the movie, r4 a scene's index or -1, r5 a child's
// index, r6 a string id, r7 on its arguments. The node must be a text.
// Returns the movie's text map (a caller reads it).
extern "C" REX_FUNC(sub_8211C3C8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C3C8)
  const Arguments arguments(ctx);
  const uint32_t string_id = ctx.r6.u32;
  const uint32_t args = SpillArguments(ctx, 7);
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  MovieTextNode* node = FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32);
  SBX_UI_MOVIE_TEXT_NODE_CHECK(sub_8211C3C8, node, arguments)
  FormatText(g, movie, node, string_id, args);
  ctx.r3.u64 = AddressOf(&movie->text_map);
}

// UiMovie::SetCharText: r3 the movie, r4 a scene's index or -1, r5 a
// child's index, r6 an 8-bit string. The node must be a text.
extern "C" REX_FUNC(sub_8211C498) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C498)
  const Arguments arguments(ctx);
  const uint32_t string = ctx.r6.u32;
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  MovieTextNode* node = FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32);
  SBX_UI_MOVIE_TEXT_NODE_CHECK(sub_8211C498, node, arguments)
  SetCharText(g, movie, node, string);
}

// UiMovie::ClearText: r3 the movie, r4 a scene's index or -1, r5 a child's.
extern "C" REX_FUNC(sub_8211C640) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C640)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  ClearText(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32));
}

// r3 the movie, r4 a scene's index or -1, r5 a child's, r6 the frames the
// text waits before it scrolls, f1 its speed.
extern "C" REX_FUNC(sub_8211C6C0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C6C0)
  const uint32_t delay = ctx.r6.u32;
  const float speed = float(ctx.f1.f64);
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  SetScrollSpeed(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32), delay, speed);
}

// UiMovie::ScrollText: r3 the movie, r4 a scene's index or -1, r5 a
// child's, r6 whether it scrolls. Returns the movie's text map (a caller
// may read it).
extern "C" REX_FUNC(sub_8211C740) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C740)
  const uint32_t scrolling = ctx.r6.u32;
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  ScrollText(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32), scrolling);
  ctx.r3.u64 = AddressOf(&movie->text_map);
}

// The text's scroll restarted: r3 the movie, r4 a scene's index or -1, r5
// a child's.
extern "C" REX_FUNC(sub_8211C7C8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C7C8)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  RestartScroll(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32));
}

// UiMovie::SetNumberText: r3 the movie, r4 a scene's index or -1, r5 a
// child's, r6 the number.
extern "C" REX_FUNC(sub_8211C908) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C908)
  const uint64_t value = ctx.r6.u64;
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  SetNumberText(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32), value);
}

// UiMovie::SetNodeText: r3 the movie, r4 a text node, r5 a string id, r6
// on its arguments.
extern "C" REX_FUNC(sub_8211C9B0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C9B0)
  MovieTextNode* node = At<MovieTextNode>(ctx.r4.u32);
  if (node->node.tag != kTagText) {
    __imp__sub_8211C9B0(ctx, base);
    return;
  }
  const uint32_t string_id = ctx.r5.u32;
  const uint32_t args = SpillArguments(ctx, 6);
  FormatText(g, At<UiMovie>(ctx.r3.u32), node, string_id, args);
}

// UiMovie::SetNodeCharText: r3 the movie, r4 a text node, r5 an 8-bit string.
extern "C" REX_FUNC(sub_8211CA68) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CA68)
  MovieTextNode* node = At<MovieTextNode>(ctx.r4.u32);
  if (node->node.tag != kTagText) {
    __imp__sub_8211CA68(ctx, base);
    return;
  }
  SetCharText(g, At<UiMovie>(ctx.r3.u32), node, ctx.r5.u32);
}

// r3 the movie, r4 a text node: its next page shown. 1 when there was one.
extern "C" REX_FUNC(sub_8211CAF8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CAF8)
  ctx.r3.u64 = ShowNextPage(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32)) ? 1 : 0;
}

// r3 the movie, r4 a text node: whether it has a page after the one shown.
extern "C" REX_FUNC(sub_8211CB88) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CB88)
  ctx.r3.u64 = HasMorePages(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32)) ? 1 : 0;
}

// UiMovie::SetNodeNumberText: r3 the movie, r4 a text node, r5 the number.
extern "C" REX_FUNC(sub_8211CBE0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CBE0)
  const uint64_t value = ctx.r5.u64;
  SetNumberText(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32), value);
}

// UiMovie::SetNameText: r3 the movie, r4 a text node, r5 a name record.
extern "C" REX_FUNC(sub_8211CC68) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CC68)
  SetNameText(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32),
              At<NameRecord>(ctx.r5.u32));
}

// UiMovie::SetNodeTextV: r3 the movie, r4 a text node, r5 a string id, r6
// a va_list of its arguments.
extern "C" REX_FUNC(sub_8211CDA0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CDA0)
  FormatTextV(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32), ctx.r5.u32, ctx.r6.u32);
}

// UiMovie::ClearNodeText: r3 the movie, r4 a node.
extern "C" REX_FUNC(sub_8211CE18) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CE18)
  ClearNodeText(g, At<UiMovie>(ctx.r3.u32), At<MovieTextNode>(ctx.r4.u32));
}

// UiMovie::RevealText: r3 the movie, r4 a scene's index or -1, r5 a
// child's: its next page shown. 1 when there was one.
extern "C" REX_FUNC(sub_8211DA40) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DA40)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  ctx.r3.u64 = ShowNextPage(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32)) ? 1 : 0;
}

// r3 the movie, r4 a scene's index or -1, r5 a child's: whether its text
// has a page after the one shown.
extern "C" REX_FUNC(sub_8211DA98) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DA98)
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  ctx.r3.u64 = HasMorePages(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32)) ? 1 : 0;
}

// UiMovie::SetChildNameText: r3 the movie, r4 a scene's index or -1, r5 a
// child's, r6 a name record.
extern "C" REX_FUNC(sub_8211DAF0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DAF0)
  const NameRecord* name = At<NameRecord>(ctx.r6.u32);
  UiMovie* movie = At<UiMovie>(ctx.r3.u32);
  SetNameText(g, movie, FindNode(movie, int32_t(ctx.r4.u32), ctx.r5.u32), name);
}

// --- Animations ---

// UiMovie::Draw: r3 the movie.
extern "C" REX_FUNC(sub_8211D440) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211D440)
  Draw(g, At<UiMovie>(ctx.r3.u32));
}

// UiMovie::Play: r3 the movie, r4 a scene's index or -1, r5 an
// animation's index in it, r6 to r9 the slot's with-next, backwards,
// remove-at-end and hold.
extern "C" REX_FUNC(sub_8211DB38) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DB38)
  Play(g, At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), ctx.r6.u32,
       ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

// UiMovie::LoopNamed: MoveNamedAnimation to the looping list.
extern "C" REX_FUNC(sub_8211DFA0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DFA0)
  MoveNamedAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), true);
}

// UiMovie::FinishNamed: r3 the movie, r4 a named animation's index.
extern "C" REX_FUNC(sub_8211E0E0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E0E0)
  FinishNamed(g, At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32));
}

// UiMovie::Run: a jump to UiMovie::Update (sub_8211D4B8, through subtick's
// override): r3 the movie, r4 1 to draw only. Returns whether it's going.
extern "C" REX_FUNC(sub_8211E378) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E378)
  ctx.r3.u64 = g.Call(sub_8211D4B8, ctx.r3.u32, ctx.r4.u32);
}

// UiMovie::DrawQueuedText: r3 the movie, r4 its text list, r5 nonzero on a
// draw-only pass.
extern "C" REX_FUNC(sub_8211CF58) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211CF58)
  DrawQueuedText(g, At<UiMovie>(ctx.r3.u32), At<MovieTextList>(ctx.r4.u32), ctx.r5.u32);
}

// The HUD's rear-view mirror: r3 the primitive node it's drawn over.
extern "C" REX_FUNC(sub_8211C0B8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C0B8)
  DrawMirror(g, At<MoviePrimitive>(ctx.r3.u32));
}

// UiMovie::PlayNamed: r3 the movie, r4 a named animation's index, r5 to r8
// the slot's with_next, backwards, remove_at_end and hold.
extern "C" REX_FUNC(sub_8211DC88) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DC88)
  PlayNamed(g, At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
            ctx.r8.u32);
}

// UiMovie::MoveAnimation: r3 the movie, r4 a scene's index or -1, r5 an
// animation's index, r6 nonzero to loop it (queue to looping list), 0 to
// play it out (looping list to queue).
extern "C" REX_FUNC(sub_8211DDA0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DDA0)
  MoveAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), int32_t(ctx.r5.u32),
                ctx.r6.u32 != 0);
}

// UiMovie::MoveNamedAnimation: r3 the movie, r4 a named animation's index,
// r5 the direction as MoveAnimation's r6.
extern "C" REX_FUNC(sub_8211DEA8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DEA8)
  MoveNamedAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), ctx.r5.u32 != 0);
}

// UiMovie::Loop and Unloop: MoveAnimation to the looping list, and back.
extern "C" REX_FUNC(sub_8211DF98) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DF98)
  MoveAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), true);
}

extern "C" REX_FUNC(sub_8211DFE8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DFE8)
  MoveAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), int32_t(ctx.r5.u32), false);
}

// UiMovie::UnloopNamed: MoveNamedAnimation back to the queue.
extern "C" REX_FUNC(sub_8211DFF0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DFF0)
  MoveNamedAnimation(At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), false);
}

// UiMovie::LoopLast: r3 the movie.
extern "C" REX_FUNC(sub_8211DFA8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DFA8)
  LoopLast(At<UiMovie>(ctx.r3.u32));
}

// UiMovie::Finish: r3 the movie, r4 a scene's index or -1, r5 an
// animation's index.
extern "C" REX_FUNC(sub_8211DFF8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211DFF8)
  Finish(g, At<UiMovie>(ctx.r3.u32), int32_t(ctx.r4.u32), int32_t(ctx.r5.u32));
}

// UiMovie::FinishAll: r3 the movie.
extern "C" REX_FUNC(sub_8211E1B8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E1B8)
  FinishAll(g, At<UiMovie>(ctx.r3.u32));
}

// A named animation copied: r3 the entry, r4 another; its name assigned
// (std::string::assign, sub_820917E8, all of it) and its scene and
// animation copied. Returns r3.
extern "C" REX_FUNC(sub_8211E4D8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E4D8)
  const uint32_t to_address = ctx.r3.u32;
  NamedAnimation* to = At<NamedAnimation>(to_address);
  const NamedAnimation* from = At<NamedAnimation>(ctx.r4.u32);
  g.Call(sub_820917E8, to, from, 0u, -1);
  to->scene = from->scene;
  to->animation = from->animation;
  ctx.r3.u64 = to_address;
}

// --- The movie table ---

// UiMovies::IsLoaded: r3 a movie's id. 1 when its file has loaded.
extern "C" REX_FUNC(sub_8211C068) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C068)
  ctx.r3.u64 = IsLoaded(ctx.r3.u32) ? 1 : 0;
}

// The table's set: r3 the table, r4 the key's address, r5 the value's.
extern "C" REX_FUNC(sub_8211C338) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211C338)
  SetMoviePair(g, At<MoviePairs>(ctx.r3.u32), *At<i32_be>(ctx.r4.u32), *At<u32_be>(ctx.r5.u32));
}

// UiMovie::~UiMovie: r3 the movie.
extern "C" REX_FUNC(sub_8211E320) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E320)
  DestroyMovie(g, At<UiMovie>(ctx.r3.u32));
}

// UiMovies::Delete: r3 a movie's id.
extern "C" REX_FUNC(sub_8211E418) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E418)
  DeleteMovie(g, ctx.r3.u32);
}

// UiMovies::DeleteAll, and the jump to it (sub_8211E530).
extern "C" REX_FUNC(sub_8211E488) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E488)
  DeleteAllMovies(g);
}

extern "C" REX_FUNC(sub_8211E530) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E530)
  DeleteAllMovies(g);
}

// UiMovie::OnLoaded: r4 the loaded archive, r6 the movie.
extern "C" REX_FUNC(sub_8211E6C0) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E6C0)
  OnLoaded(g, ctx.r4.u32, At<UiMovie>(ctx.r6.u32));
}

// UiMovie::Load: r3 the movie, r4 its file's id.
extern "C" REX_FUNC(sub_8211E958) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E958)
  LoadMovie(g, ctx.r3.u32, ctx.r4.u32);
}

// UiMovies::Open: r3 a movie's id, r4 its file's id. 1 when opened.
extern "C" REX_FUNC(sub_8211E9C8) {
  SBX_UI_MOVIE_TEXT_FALLBACK(sub_8211E9C8)
  ctx.r3.u64 = OpenMovie(g, ctx.r3.u32, ctx.r4.u32) ? 1 : 0;
}
