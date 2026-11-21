// The UI movies as our own C++ (--native_ui_movies, on by default): here
// the movie file's tables (its animations, node tracks, keys,
// sub-animations, vertices), the two magic checks, the texts' lookups by
// id, the movie's constructor and its named animations' destroy, its update
// and the track step. The update and the track step (sub_8211D4B8,
// sub_82585C40) keep subtick's overrides (subtick/per_frame_logic.cpp),
// which call ours (at the end of this file) in place of the recompiled
// functions; the movies' own code calls them through Guest::Call so the
// wrappers run. The nodes and texts are in ui_movie_nodes.cpp, the drawing
// and the animations in ui_movie_draw.cpp, the text setters, the animation
// starts and the movie table in ui_movie_text.cpp.
//
// Nothing is kept between calls: subtick replays updates and puts guest
// memory back.

#include "game/ui_movies.h"
#include "base/settings.h"
#include "game/ui_movie_text.h"
#include "hooks/native_calls.h"

#include <climits>
#include <cmath>
#include <cstdint>

SBX_SETTING_BOOL(native_ui_movies, true, "Overrides", "Menus and UI",
                 "Our own C++ for the UI movies (menus and HUD: loading, the update, "
                 "drawing, nodes, texts, animations) instead of the recompiled functions; "
                 "the ticker text's step and offset stay subtick's");

namespace game {
namespace {

using guest::AddressOf;
using guest::At;
using guest::Guest;

bool On() { return SBX_GET(native_ui_movies); }

// The read-only floats the originals load.
constexpr float kFrameStep = 2.0f;   // 0x820021A8: the time a step adds.
constexpr float kNearOne = 1.0e-5f;  // 0x82002A20: how near 1 a step key's time counts as there.
constexpr float kCurve = 3.0f;       // 0x820021A4: the curve keys' factor.

// The 8-byte magic at `at`: four characters, a 0, then the version's
// three and a 0.
bool Magic(const uint8_t* at, const char (&magic)[8]) {
  for (int i = 0; i < 8; i++) {
    if (at[i] != uint8_t(magic[i])) {
      return false;
    }
  }
  return true;
}

// VertexOffset (sub_82582CF8): a vertex's offset in a primitive with
// `texture_count` texture entries, 16 x (texture_count + 2 x (index + 4)).
uint32_t VertexOffset(uint32_t texture_count, uint32_t index) {
  return (((index + 4) << 1) + texture_count) << 4;
}

}  // namespace

bool IsXmd(const uint8_t* file) { return Magic(file, {'X', 'M', 'D', 0, '0', '0', '1', 0}); }

bool IsMovieFile(const uint8_t* file) {
  return Magic(file + 8, {'X', '2', 'D', 0, '0', '0', '5', 0});
}

// UiMovie::UiMovie: its textures' array, its text list, its draw context
// and its named animations made (sub_8211D3D0, recompiled), its named
// animations' count and its text map cleared.
void ConstructMovie(Guest& g, UiMovie* movie) {
  MakeWords(g, &movie->textures);
  MakeTextList(g, &movie->texts);
  movie->texts.count = 0;
  InitContext(&movie->draw_context);
  for (NamedAnimation& named : movie->named) {
    g.Call(sub_8211D3D0, &named);  // NamedAnimation::NamedAnimation
  }
  movie->named_count = 0;
  for (MovieTextMap::Pair& pair : movie->text_map.pairs) {
    pair.id = 0;
    pair.record = nullptr;
  }
  movie->text_map.count = 0;
}

// UiMovie::DestroyNamedAnimations: each destroyed (std::string's
// destructor, sub_820913F0, r4 = 1), last first.
void DestroyNamedAnimations(Guest& g, NamedAnimation* named) {
  for (int i = UiMovie::kNamedAnimations - 1; i >= 0; i--) {
    g.Call(sub_820913F0, &named[i], 1u, 0u);
  }
}

uint32_t TextFirstLine(UiMovie* movie, uint32_t id) {
  const MovieTextRecord* text = FindText(movie, id);
  return text != nullptr ? uint32_t(text->first_line) : 0;
}

uint32_t TextScrolls(UiMovie* movie, uint32_t id) {
  const MovieTextRecord* text = FindText(movie, id);
  return text != nullptr ? uint32_t(text->scrolling) : 0;
}

namespace {

// UiMovie::ApplyAnimation of a slot at its time.
bool ApplySlot(Guest& g, AnimationSlot* slot, uint32_t backwards) {
  return ApplyAnimation(g, slot->target, slot->animation, double(float(slot->time)), backwards);
}

// The movie's nodes laid out and drawn into its lists after the
// animations: the scene drawn, its queued texts drawn (on a draw-only pass
// without the ticker's step), the clip reset after both of the update's
// draw keys, and the live pose put back from the saved one.
void FinishUpdate(Guest& g, UiMovie* movie, uint32_t draw_only) {
  DrawMovie(g, &movie->draw_context, movie->live, &movie->textures, &movie->texts, 0xFFFFFFFFu);
  DrawQueuedText(g, movie, &movie->texts, draw_only);
  ResetClip(g, 676);
  ResetClip(g, 148);
  RestorePose(movie->live, movie->saved, nullptr, nullptr);
}

// The live pose saved (the update's first step, and its last after the
// ended animations).
void SavePose(UiMovie* movie) { RestorePose(movie->saved, movie->live, nullptr, nullptr); }

// A list's first link taken out of use and put first on its free links.
void UnlinkFirst(AnimationList& list) {
  const int16_t first = list.head;
  AnimationList::Node* node = list.NodeAt(first);
  list.head = node->next;
  node->next = list.free_head;
  list.free_head = first;
  list.count = int16_t(list.count - 1);
}

// The list as a frame's local list starts: every link free.
void ResetEnded(AnimationList& list) {
  for (int k = 0; k < 32; k++) {
    list.nodes[k].slot = int16_t(k);
    list.nodes[k].next = int16_t(k + 1);
  }
  list.free_head = 0;
  list.count = 0;
  list.nodes[31].next = -1;
  list.head = -1;
  list.unknown_head = -1;
  list.unknown_free = -1;
}

// UiMovie::Update: returns whether the movie is still going (1 on a
// draw-only pass).
//
// A draw-only pass applies the queued animations (up to the last that
// runs with its next) and the looping ones at their times, and finishes.
// Otherwise each queued animation is applied and its time stepped (unless
// held; backwards when set to, kept within 0 and its length). One that
// has ended (the apply returns 0) and is taken off when it ends moves to a
// list of ended ones; the walk stops at the first that doesn't run with
// its next. When none is still going, the ones walked are all moved to the
// ended list. The looping animations are applied and stepped (wrapping to
// 0 past their length, to their length below 0). After the finish, the
// ended animations are applied once more, in order, with the live pose
// saved after them.
//
// The ended list is on the guest stack (the original's 1,616-byte frame,
// +80): the lists' links are found by guest address (a -1 link reads the
// memory before the list, as the game does).
uint32_t UpdateMovie(Guest& g, UiMovie* movie, bool draw_only) {
  guest::StackFrame frame(g.ctx, 1616);
  AnimationList& ended = *At<AnimationList>(frame.Local(80));
  AnimationList& queue = movie->queue;
  AnimationList& looping = movie->looping;

  SavePose(movie);
  ResetEnded(ended);

  if (draw_only) {
    const int32_t count = queue.count;
    for (int32_t i = 0; i < count; i++) {
      AnimationSlot* slot = queue.ItemAtPosition(uint32_t(i));
      ApplySlot(g, slot, slot->backwards);
      if (slot->with_next == 0) {
        break;
      }
    }
    for (uint32_t i = 0; i < uint32_t(int32_t(looping.count)); i++) {
      ApplySlot(g, looping.ItemAtPosition(i), 0);
    }
    FinishUpdate(g, movie, 1);
    return 1;
  }

  g.ctx.fpscr.disableFlushMode();
  const float zero = 0.0f;
  uint32_t going = 0;
  int32_t count = queue.count;
  if (count == 0) {
    going = 1;
  } else {
    uint32_t walked = 0;
    for (int32_t i = 0; i < count; i++) {
      AnimationSlot* slot = queue.ItemAtPosition(uint32_t(i));
      const bool still = ApplySlot(g, slot, slot->backwards);
      walked++;
      if (slot->hold == 0) {
        g.ctx.fpscr.disableFlushMode();
        float time = slot->time;
        time = slot->backwards != 0 ? time - kFrameStep : time + kFrameStep;
        if (time < zero) {
          time = zero;
        } else {
          const float length = slot->animation->length;
          if (time > length) {
            time = length;
          }
        }
        slot->time = time;
      }
      if (still) {
        going = 1;
      }
      if (slot->with_next == 0) {
        break;  // The last of the ones that run together.
      }
      if (!still && slot->remove_at_end != 0) {
        InsertSlot(&ended, slot, uint32_t(int32_t(ended.count)));
        RemoveAt(&queue, uint32_t(i));
        walked--;
        count--;
        i--;
      }
    }
    // The walked ones are moved only when none is still going.
    if (going == 0 && uint16_t(int16_t(queue.count)) != 0) {
      for (; walked != 0; walked--) {
        InsertSlot(&ended, queue.ItemAtPosition(0), uint32_t(int32_t(ended.count)));
        UnlinkFirst(queue);
      }
    }
  }

  for (uint32_t i = 0; i < uint32_t(int32_t(looping.count)); i++) {
    AnimationSlot* slot = looping.ItemAtPosition(i);
    ApplySlot(g, slot, 0);
    if (slot->hold == 0) {
      g.ctx.fpscr.disableFlushMode();
      float time = slot->time + kFrameStep;
      if (time < zero) {
        time = slot->animation->length;
      } else if (time > slot->animation->length) {
        time = zero;
      }
      slot->time = time;
    }
  }

  FinishUpdate(g, movie, 0);

  uint32_t left = uint32_t(int32_t(ended.count));
  if (left != 0) {
    for (; left != 0; left--) {
      ApplySlot(g, ended.ItemAtPosition(0), 0);
      UnlinkFirst(ended);
    }
    SavePose(movie);
    if (uint16_t(int16_t(queue.count)) != 0) {
      going = 1;
    }
  }
  return going;
}

// A single-precision result of a double (the originals' fmuls, fadds,
// fsubs and fmadds, on the caller's t as it came).
double Single(double value) { return double(float(value)); }

// Whether a step key's time counts as its end: |t - 1| not above
// kNearOne (so a NaN counts as there, as the original's ble has it).
bool AtStepEnd(double t) {
  const double one = 1.0;
  const double distance = t > one ? Single(t - one) : Single(one - t);
  return !(distance > double(kNearOne));
}

// UiMovie::ApplyTrack: a track's property on the node at `t` between the
// keys `from` and `to` (0 to 1; either key may be missing). The property's
// descriptor gives the value's address (in the node, or in its vertex).
//
// With no first key, a key on the guest stack stands for it (the
// original's 224-byte frame, +112: the recompiled unsigned curves may be
// handed it), holding the value now when the property is set; with no
// second, the first is used. The blended value is stored when the property
// is set, else added to the value there.
void ApplyTrack(Guest& g, MovieNode* node, const MovieTrack* track, const MovieTrackKey* from,
                const MovieTrackKey* to, double t) {
  guest::StackFrame frame(g.ctx, 224);
  MovieTrackKey* local = At<MovieTrackKey>(frame.Local(112));

  const MovieTrackProperty* descriptor = TrackProperty(g, track->property);
  g.ctx.fpscr.disableFlushMode();
  const float zero = 0.0f;
  uint32_t target = descriptor->offset + AddressOf(node);
  const bool in_array = descriptor->in_array != 0;
  local->time = zero;
  local->curve_12 = zero;
  local->curve_16 = zero;
  local->blend = 0;
  local->value = 0;
  if (in_array) {
    target += VertexOffset(reinterpret_cast<MoviePrimitive*>(node)->texture_count, track->index);
  }
  if (from == nullptr) {
    from = local;
    if (descriptor->set != 0 && descriptor->type < 3) {
      // The value now, a float's or a word's bits alike.
      local->value = uint32_t(*At<u32_be>(target));
    }
  }
  if (to == nullptr) {
    to = from;
  }
  const uint32_t type = descriptor->type;
  if (type >= 3) {
    return;
  }
  const uint32_t blend = to->blend;
  const double one = 1.0;
  g.ctx.fpscr.disableFlushMode();

  if (type == MovieTrackProperty::kFloat) {
    double value = double(zero);
    if (blend < 1) {
      value = double((AtStepEnd(t) ? to : from)->FloatValue());
    } else if (blend == 1) {
      const double scaled = Single(double(to->FloatValue()) * t);
      value = Single(std::fma(Single(one - t), double(from->FloatValue()), scaled));
    } else if (blend == 2) {
      value = CubicFloat(g, from, to, t);
    } else if (blend == 3) {
      // The original's register steps, kept in their order.
      double f11 = Single(double(float(to->curve_16)) * t);
      double f10 = double(float(to->curve_12));
      const double f9 = double(to->FloatValue()), f8 = double(from->FloatValue());
      double f13 = one;
      double f0 = Single(f13 - t);
      double f12 = double(kCurve);
      f11 = Single(f11 * f12);
      f10 = Single(f10 * f0);
      f11 = Single(f11 * f0);
      f12 = Single(std::fma(f10, f12, t));
      f0 = Single(f11 * f0);
      f12 = Single(f12 * t);
      f0 = Single(std::fma(f12, t, f0));
      f13 = Single(f13 - f0);
      f0 = Single(f9 * f0);
      value = Single(std::fma(f13, f8, f0));
    }
    f32_be* out = At<f32_be>(target);
    const double before = descriptor->set != 0 ? double(zero) : double(float(*out));
    *out = float(before + value);
    return;
  }

  // A word: type 1 signed, type 2 unsigned.
  const bool is_signed = type == MovieTrackProperty::kSigned;
  uint32_t value = 0;
  if (blend < 1) {
    value = (AtStepEnd(t) ? to : from)->value;
  } else if (blend == 1) {
    const uint32_t to_word = to->value, from_word = from->value;
    const double to_value =
        is_signed ? double(float(double(int32_t(to_word)))) : double(float(double(to_word)));
    const double from_value =
        is_signed ? double(float(double(int32_t(from_word)))) : double(float(double(from_word)));
    const double scaled = Single(to_value * t);
    const double blended = Single(std::fma(from_value, Single(one - t), scaled));
    if (is_signed) {
      // fctiwz: toward zero, held at the largest word; a NaN gives the
      // smallest.
      value = std::isnan(blended)          ? 0x80000000u
              : blended >= double(INT_MAX) ? uint32_t(INT_MAX)
              : blended <= double(INT_MIN) ? 0x80000000u
                                           : uint32_t(int32_t(blended));
    } else {
      // fctidz, its low word kept.
      constexpr double kTwoTo63 = 9223372036854775808.0;
      const int64_t wide = std::isnan(blended)                             ? INT64_MIN
                           : blended > kTwoTo63                            ? LLONG_MAX
                           : (blended >= kTwoTo63 || blended < -kTwoTo63) ? INT64_MIN
                                                                           : int64_t(blended);
      value = uint32_t(wide);
    }
  } else if (blend == 2) {
    if (is_signed) {
      value = uint32_t(CubicInteger(g, from, to, t));
    } else {
      g.ctx.f1.f64 = t;
      value = g.Call(sub_82585438, from, to);  // The unsigned cubic.
    }
  } else if (blend == 3) {
    if (is_signed) {
      value = uint32_t(EaseInteger(g, from, to, t));
    } else {
      g.ctx.f1.f64 = t;
      value = g.Call(sub_825851A0, from, to);  // The unsigned ease.
    }
  }
  u32_be* out = At<u32_be>(target);
  const uint32_t before = descriptor->set != 0 ? 0 : uint32_t(*out);
  *out = before + value;
}

}  // namespace
}  // namespace game

using namespace game;

#define SBX_UI_MOVIES_FALLBACK(name) \
  REX_FUNC_PROLOGUE();               \
  if (!On()) {                       \
    __imp__##name(ctx, base);        \
    return;                          \
  }                                  \
  Guest g(ctx, base);

// UiMovie::DestroyNamedAnimations: r3 the movie's named animations.
extern "C" REX_FUNC(sub_8211D378) {
  SBX_UI_MOVIES_FALLBACK(sub_8211D378)
  DestroyNamedAnimations(g, At<NamedAnimation>(ctx.r3.u32));
}

// UiMovie::UiMovie: r3 the movie, returned.
extern "C" REX_FUNC(sub_8211E380) {
  SBX_UI_MOVIES_FALLBACK(sub_8211E380)
  const uint32_t movie = ctx.r3.u32;
  ConstructMovie(g, At<UiMovie>(movie));
  ctx.r3.u64 = movie;
}

// Animation::NodeTrack: r3 a node track list, r4 the index.
extern "C" REX_FUNC(sub_8258BCC8) {
  SBX_UI_MOVIES_FALLBACK(sub_8258BCC8)
  ctx.r3.u64 = AddressOf(At<MovieNodeTrackList>(ctx.r3.u32)->Track(ctx.r4.u32));
}

// Track::Key: r3 a key list, r4 the index.
extern "C" REX_FUNC(sub_8258BCD8) {
  SBX_UI_MOVIES_FALLBACK(sub_8258BCD8)
  ctx.r3.u64 = AddressOf(At<MovieKeyList>(ctx.r3.u32)->Key(ctx.r4.u32));
}

// An animation's tables: r3 the animation, r4 the index.
extern "C" REX_FUNC(sub_8258BCE8) {  // NodeTrackList
  SBX_UI_MOVIES_FALLBACK(sub_8258BCE8)
  ctx.r3.u64 = AddressOf(At<MovieAnimation>(ctx.r3.u32)->NodeTrackList(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_8258BD00) {  // KeyList
  SBX_UI_MOVIES_FALLBACK(sub_8258BD00)
  ctx.r3.u64 = AddressOf(At<MovieAnimation>(ctx.r3.u32)->KeyList(ctx.r4.u32));
}

extern "C" REX_FUNC(sub_8258BD18) {  // SubAnimation
  SBX_UI_MOVIES_FALLBACK(sub_8258BD18)
  ctx.r3.u64 = AddressOf(At<MovieAnimation>(ctx.r3.u32)->SubAnimation(ctx.r4.u32));
}

// MovieFile::Animation: r3 the movie file, r4 the index.
extern "C" REX_FUNC(sub_825865D0) {
  SBX_UI_MOVIES_FALLBACK(sub_825865D0)
  ctx.r3.u64 = At<MovieFile>(ctx.r3.u32)->Animation(ctx.r4.u32);
}

// VertexOffset: r3 a primitive's texture count, r4 a vertex's index.
extern "C" REX_FUNC(sub_82582CF8) {
  SBX_UI_MOVIES_FALLBACK(sub_82582CF8)
  ctx.r3.u64 = VertexOffset(ctx.r3.u32, ctx.r4.u32);
}

// IsXmd: "XMD\0001\0" at r3.
extern "C" REX_FUNC(sub_82586358) {
  SBX_UI_MOVIES_FALLBACK(sub_82586358)
  ctx.r3.u64 = IsXmd(At<uint8_t>(ctx.r3.u32)) ? 1 : 0;
}

// IsMovieFile: "X2D\0005\0" at r3 + 8.
extern "C" REX_FUNC(sub_82586538) {
  SBX_UI_MOVIES_FALLBACK(sub_82586538)
  ctx.r3.u64 = IsMovieFile(At<uint8_t>(ctx.r3.u32)) ? 1 : 0;
}

// A text's first line and whether it scrolls: r3 the movie, r4 the text's
// id; 0 when the movie hasn't the text.
extern "C" REX_FUNC(sub_8211B988) {
  SBX_UI_MOVIES_FALLBACK(sub_8211B988)
  ctx.r3.u64 = TextFirstLine(At<UiMovie>(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_8211BB88) {
  SBX_UI_MOVIES_FALLBACK(sub_8211BB88)
  ctx.r3.u64 = TextScrolls(At<UiMovie>(ctx.r3.u32), ctx.r4.u32);
}

// UiMovie::Update: r3 the movie, r4 nonzero for a draw-only pass. Returns
// whether the movie is still going.
void hooks::NativeUiMovies_Update(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8211D4B8(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ctx.r3.u64 = UpdateMovie(g, At<UiMovie>(ctx.r3.u32), ctx.r4.u32 != 0);
}

// UiMovie::ApplyTrack: r3 the node, r4 the track, r5 and r6 the keys the
// time is between (either may be 0), f1 how far between them (0 to 1).
void hooks::NativeUiMovies_ApplyTrack(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_82585C40(ctx, base);
    return;
  }
  Guest g(ctx, base);
  ApplyTrack(g, At<MovieNode>(ctx.r3.u32), At<MovieTrack>(ctx.r4.u32),
             At<MovieTrackKey>(ctx.r5.u32), At<MovieTrackKey>(ctx.r6.u32), ctx.f1.f64);
}
