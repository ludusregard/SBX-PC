// The boot data's set-up as our own C++ (--native_data_load, on by
// default): the boot's load (once) and its unload, the objects it creates
// (the save slot, the race data, the defaults, the input recordings, the
// replay and its records, the data tables), their constructors, the
// defaults' bit sets and the two UI movie lists' loads and frees (the
// loading screen's and another three movies').
//
// The constructors only set up their vectors: each vector's w word set
// (0.0, or 1.0 for some of a record's). Their vectors are listed as runs of
// byte offsets, since the objects' fields are only known by offset. The
// heap, the archive, the UI movies themselves, the data tables' load and the
// other boot steps stay recompiled.

#include "base/settings.h"
#include "game/car_spec.h"
#include "game/data_tables.h"
#include "game/math.h"
#include "game/replay_snapshots.h"
#include "game/save_data.h"

SBX_SETTING_BOOL(native_data_load, true, "Overrides", "Data and Saves",
                 "Our own C++ for the boot data's set-up (the boot's load and unload, the "
                 "objects it creates and their constructors, the defaults' bit sets, the "
                 "boot's UI movie lists) instead of the recompiled functions; the heap, the "
                 "movies and the loads stay recompiled");

namespace {

using game::InputRecordings;
using game::Member4080;
using game::Record5232;
using game::Replay;
using game::Vector;
using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::u32_be;

bool On() { return SBX_GET(native_data_load); }

// Where the boot's objects are kept (writable).
constexpr uint32_t kRaceData = 0x826B580C;
constexpr uint32_t kInputRecordings = 0x826B582C;
constexpr uint32_t kReplay = 0x826B5830;
constexpr uint32_t kBootLoaded = 0x826B586C;

// The loading screen's two words, cleared before its movies load: its
// movie list's count and a word before the list (writable).
constexpr uint32_t kLoadingScreenWords[] = {0x8280EEB0, 0x8280EE68};
// The loading screen's movies, by key.
constexpr uint32_t kLoadingMovies[] = {0x582E, 0x582D, 0x582F};

// The boot's two lists of UI movies: the loading screen's, and three other
// movies whose asset ids are at kOtherMovieIds (all writable).
constexpr uint32_t kLoadingScreenMovies = 0x8280EE70;
constexpr uint32_t kOtherMovies = 0x8280F618;
constexpr uint32_t kOtherMovieIds = 0x8262808C;
constexpr uint32_t kOtherMovieIdCount = 3;
// Words the other movies' load clears: nine words, one more, and a flag.
constexpr uint32_t kOtherMovieWords = 0x8280F5D8;
constexpr uint32_t kOtherMovieWordCount = 9;
constexpr uint32_t kOtherMovieWord = 0x8280F608;
constexpr uint32_t kOtherMovieFlag = 0x82628088;
// The render app (its slot 8 the render manager: slot 1 its command list,
// slot 2 its renderer).
constexpr uint32_t kRenderApp = 0x826AA880;
constexpr uint32_t kRenderManagerSlot = 8;
constexpr uint32_t kCommandListSlot = 1;
constexpr uint32_t kRendererSlot = 2;

constexpr uint32_t kUiMovieBytes = 40976;

// A list of UI movies: the movies (their objects' addresses) and their
// count. The game doesn't check the count against the room.
struct MovieList {
  u32_be movies[16];
  u32_be count;

  u32_be& Movie(uint32_t index) { return *guest::Element<u32_be>(AddressOf(movies), index); }
};
GUEST_OFFSET(MovieList, count, 64);

// One of the 48 group bit sets (60 bytes).
struct GroupBits {
  u32_be words[15];
};
GUEST_SIZE(GroupBits, 60);

// A bit range of the defaults' bit sets: bits first to end - 1.
struct BitRange {
  uint16_t first;
  uint16_t end;
};

// The defaults' bit sets (writable): the unlocked cars' (15 words), two
// new game sets (7 words each), the new game's own (15 words), a scratch
// set (15 words) and 48 group sets of 15 words, 60 bytes apart.
constexpr uint32_t kUnlockedBits = 0x826B2D24;
constexpr uint32_t kNewGameBitsA = 0x826B3904;
constexpr uint32_t kNewGameBitsB = 0x826B4DC4;
constexpr uint32_t kNewGameBits = 0x826B4D88;
constexpr uint32_t kScratchBits = 0x826B3D44;
constexpr uint32_t kGroupBits = 0x826B2D60;
constexpr uint32_t kGroupCount = 48;
constexpr uint32_t kSetWords = 15;
constexpr uint32_t kSmallSetWords = 7;

// The bits each group set gets (by the original's jump table on the
// group's index; group 29 gets none). The groups nest: 0 is 1 and 2, 3 is
// 4 and 5, 12 is 13 to 15...
constexpr BitRange kGroupRanges[kGroupCount] = {
    {0, 20},    {0, 10},    {10, 20},   {20, 34},   {20, 27},   {27, 34},   {34, 41},
    {41, 48},   {48, 55},   {55, 62},   {62, 69},   {69, 76},   {76, 88},   {76, 80},
    {80, 84},   {84, 88},   {88, 95},   {95, 108},  {95, 96},   {96, 100},  {100, 104},
    {104, 108}, {108, 119}, {119, 130}, {130, 141}, {141, 152}, {152, 170}, {152, 158},
    {158, 164}, {0, 0},     {170, 174}, {174, 178}, {178, 182}, {182, 186}, {186, 189},
    {189, 389}, {389, 391}, {391, 395}, {395, 400}, {400, 404}, {404, 410}, {410, 435},
    {410, 415}, {415, 420}, {420, 425}, {425, 430}, {430, 435}, {435, 454}};

// The unlocked cars' set: each group's first bit, and bits 404 to 410.
constexpr BitRange kUnlockedRanges[] = {
    {0, 1},     {10, 11},   {20, 21},   {27, 28},   {34, 35},   {41, 42},   {48, 49},
    {55, 56},   {62, 63},   {69, 70},   {76, 77},   {80, 81},   {84, 85},   {88, 89},
    {95, 96},   {108, 109}, {119, 120}, {130, 131}, {141, 142}, {152, 153}, {170, 171},
    {174, 175}, {178, 179}, {182, 183}, {186, 187}, {189, 190}, {389, 390}, {391, 392},
    {395, 396}, {400, 401}, {404, 411}, {415, 416}, {420, 421}, {425, 426}, {430, 431},
    {435, 436}};

// The new game's set.
constexpr BitRange kNewGameRanges[] = {
    {0, 3},     {10, 13},   {20, 23},   {27, 30},   {34, 37},   {41, 44},   {48, 51},
    {55, 58},   {62, 65},   {69, 72},   {76, 78},   {80, 82},   {84, 86},   {88, 91},
    {95, 97},   {100, 101}, {104, 105}, {108, 114}, {119, 125}, {130, 136}, {141, 147},
    {152, 164}, {170, 187}, {189, 391}, {395, 399}, {400, 454}};

// The two static replays' records (seven each, 5232 bytes apart) and their
// destructors, registered with atexit.
constexpr uint32_t kReplay826D8520Records = 0x826FD440;
constexpr uint32_t kReplay826D8520Destructor = 0x825C0510;
constexpr uint32_t kReplay82816E50Records = 0x828452D0;
constexpr uint32_t kReplay82816E50Destructor = 0x825C0550;
constexpr uint32_t kStaticReplayRecords = 7;

// A run of vectors' constructors: `count` vectors from byte `first` of the
// object, `stride` bytes apart, each one's w set.
void ConstructVectors(uint32_t object, uint32_t first, uint32_t count, uint32_t stride,
                      float w = 0.0f) {
  for (uint32_t i = 0; i < count; i++) {
    At<Vector>(object + first + stride * i)->w = w;
  }
}

// Part464::Part464's vectors.
void ConstructPart464(uint32_t part) {
  ConstructVectors(part, 0, 10, 16);
  ConstructVectors(part, 176, 6, 32);
  ConstructVectors(part, 384, 4, 16);
}

// Member1136::Member1136's vectors.
void ConstructMember1136(uint32_t member) {
  ConstructVectors(member, 0, 3, 16);
  ConstructVectors(member, 64, 2, 32);
  ConstructVectors(member, 112, 2, 16);
  ConstructVectors(member, 336, 4, 16);
  ConstructVectors(member, 464, 2, 16);
  ConstructVectors(member, 512, 4, 16);
  // The same three runs in three blocks 368 bytes apart.
  for (uint32_t wheel = 0; wheel < 3; wheel++) {
    const uint32_t at = 736 + 368 * wheel;
    ConstructVectors(member, at, 2, 96);
    ConstructVectors(member, at + 112, 2, 32);
    ConstructVectors(member, at + 160, 3, 16);
  }
  ConstructVectors(member, 1840, 2, 32);
  ConstructVectors(member, 1888, 5, 16);
  ConstructVectors(member, 2000, 6, 16);
  ConstructVectors(member, 2416, 2, 16);
  ConstructVectors(member, 2480, 6, 16);
  ConstructVectors(member, 2864, 4, 16);
}

// The bits of a set: its words cleared and its first `bits` set (bit i at
// word i / 32, 1 << (i % 32)).
void FillBits(u32_be* words, uint32_t word_count, uint32_t bits) {
  for (uint32_t i = 0; i < word_count; i++) {
    const uint32_t from = i * 32;
    uint32_t word = 0;
    if (bits >= from + 32) {
      word = 0xFFFFFFFFu;
    } else if (bits > from) {
      word = (1u << (bits - from)) - 1;
    }
    words[i] = word;
  }
}

// A bit set's words cleared, then the bits of each range set (bit i at
// word i / 32, 1 << (i % 32)).
template <size_t N>
void SetBitRanges(u32_be* words, uint32_t word_count, const BitRange (&ranges)[N]) {
  for (uint32_t i = 0; i < word_count; i++) {
    words[i] = 0;
  }
  for (const BitRange& range : ranges) {
    for (uint32_t bit = range.first; bit < range.end; bit++) {
      words[bit / 32] |= 1u << (bit % 32);
    }
  }
}

// Defaults::Init (sub_820CCB60): the defaults' bit sets, each cleared and
// then its bits set (the original sets them one by one, with fixed words
// ORed in; these ranges are what it leaves). The scratch set is ORed with
// five group sets and filled, then cleared: it ends cleared.
void InitDefaults() {
  SetBitRanges(At<u32_be>(kUnlockedBits), kSetWords, kUnlockedRanges);
  SetBitRanges(At<u32_be>(kNewGameBitsA), kSmallSetWords, {BitRange{0, 61}});
  SetBitRanges(At<u32_be>(kNewGameBitsB), kSmallSetWords, {BitRange{0, 199}});
  SetBitRanges(At<u32_be>(kNewGameBits), kSetWords, kNewGameRanges);
  for (uint32_t group = 0; group < kGroupCount; group++) {
    GroupBits* set = guest::Element<GroupBits>(kGroupBits, group);
    SetBitRanges(set->words, kSetWords, {kGroupRanges[group]});
  }
  u32_be* scratch = At<u32_be>(kScratchBits);
  for (uint32_t i = 0; i < kSetWords; i++) {
    scratch[i] = 0;
  }
}

// LoadingScreen::AddMovie (sub_82245658) and the other list's: a UI movie
// made (operator new, UiMovie::UiMovie; 0 when the allocation failed),
// sub_8211ABB8 on it (also on 0), loaded with the asset `id` and appended
// to the list.
void AddMovie(Guest& g, MovieList* list, uint32_t id) {
  uint32_t movie = g.Call(sub_8258C000, kUiMovieBytes);  // operator new
  if (movie != 0) {
    movie = g.Call(sub_8211E380, movie);  // UiMovie::UiMovie
  }
  g.Call(sub_8211ABB8, movie);
  g.Call(sub_8211E958, movie, id);  // UiMovie::Load
  const uint32_t index = list->count;
  list->count = index + 1;
  list->Movie(index) = movie;
}

// LoadingScreen::Free (sub_822456E0) and the other list's: the movies from
// the last, each destroyed (UiMovie::~UiMovie) and freed (operator
// delete); empty entries skipped.
void FreeMovies(Guest& g, MovieList* list) {
  while (list->count != 0) {
    const uint32_t index = list->count - 1;
    list->count = index;
    const uint32_t movie = list->Movie(index);
    if (movie != 0) {
      g.Call(sub_8211E320, movie);  // UiMovie::~UiMovie
      g.Call(sub_8258C868, movie);  // operator delete
    }
  }
}

// Member4080::Member4080 (sub_82104800): its two parts made as Part464s,
// then its own vectors.
void ConstructMember4080(Member4080& member) {
  for (auto& part : member.shapes.parts) {
    ConstructPart464(AddressOf(&part));
  }
  ConstructVectors(AddressOf(member.shapes.field_928), 0, 2, 16);
  ConstructVectors(AddressOf(member.field_1056), 0, 2, 16);
  ConstructVectors(AddressOf(member.field_1104), 0, 2, 32);
}

// Record5232::Record5232 (sub_82104D90): its own vectors (five runs of four
// with w = 1.0 first), its block at +1136 made as a Member1136, then its
// Member4080.
void ConstructRecord5232(Record5232& record) {
  const uint32_t address = AddressOf(&record);
  for (uint32_t i = 0; i < 5; i++) {
    ConstructVectors(address, 144 + 96 * i, 4, 16, 1.0f);
  }
  ConstructVectors(address, 624, 2, 16);
  ConstructVectors(address, 672, 3, 32);
  ConstructVectors(address, 752, 2, 32);
  ConstructVectors(address, 800, 2, 16);
  ConstructVectors(address, 960, 2, 16);
  ConstructVectors(address, 1008, 2, 80);
  ConstructVectors(address, 1104, 1, 16);
  ConstructMember1136(AddressOf(record.member_1136));
  ConstructMember4080(record.member_4080);
}

// InputRecordings::InputRecordings (sub_82105618): its fourteen records
// made.
void ConstructInputRecordings(InputRecordings& recordings) {
  for (Record5232& record : recordings.records_a) {
    ConstructRecord5232(record);
  }
  for (Record5232& record : recordings.records_b) {
    ConstructRecord5232(record);
  }
}

// A static replay's StaticInit (sub_825BF238, sub_825BF340): its seven
// records made, its destructor registered (atexit, sub_8258C7E8).
void InitStaticReplay(Guest& g, uint32_t records, uint32_t destructor) {
  for (uint32_t i = 0; i < kStaticReplayRecords; i++) {
    ConstructRecord5232(*guest::Element<Record5232>(records, i));
  }
  g.Call(sub_8258C7E8, destructor);
}

// RaceData::Create (sub_82102390): the race data (827,904 bytes) made
// (sub_82102340) into [0x826B580C], or 0.
void CreateRaceData(Guest& g) {
  uint32_t data = g.Call(sub_8258C000, 827904u);  // operator new
  if (data != 0) {
    data = g.Call(sub_82102340, data);
  }
  *At<u32_be>(kRaceData) = data;
}

// InputRecordings::Create (sub_82105678): made into [0x826B582C] (0 when
// the allocation fails), and its +2238992 cleared (also when it failed, as
// the original). Returns the recordings.
uint32_t CreateInputRecordings(Guest& g) {
  const uint32_t recordings = g.Call(sub_8258C000, uint32_t(sizeof(InputRecordings)));
  if (recordings != 0) {
    ConstructInputRecordings(*At<InputRecordings>(recordings));
  }
  *At<u32_be>(kInputRecordings) = recordings;
  *At<u32_be>(recordings + uint32_t(offsetof(InputRecordings, unknown_2238992))) = 0;
  return recordings;
}

// Replay::Create (sub_821056D0): made with its seven records into
// [0x826B5830] (0 when the allocation fails), and its +215372 = 1 (also
// when it failed, as the original).
void CreateReplay(Guest& g) {
  const uint32_t address = g.Call(sub_8258C000, uint32_t(sizeof(Replay)));
  if (address != 0) {
    for (Record5232& record : At<Replay>(address)->records) {
      ConstructRecord5232(record);
    }
  }
  *At<u32_be>(kReplay) = address;
  *At<u32_be>(address + uint32_t(offsetof(Replay, unknown_215372))) = 1;
}

// BootData::CreateObjects (sub_821059D8): save slot 0, the race data,
// sub_82102330, the defaults, the input recordings, the replay, the data
// tables' init and load.
void CreateBootObjects(Guest& g) {
  game::CreateSave(g, 0);
  CreateRaceData(g);
  g.Call(sub_82102330);
  InitDefaults();
  CreateInputRecordings(g);
  CreateReplay(g);
  game::InitDataTables(g);
  game::LoadDataTables(g);
}

// LoadingScreen::Load (sub_82245750): its two words cleared, then its three
// movies loaded (LoadingScreen::AddMovie).
void LoadLoadingScreen(Guest& g) {
  for (uint32_t word : kLoadingScreenWords) {
    *At<u32_be>(word) = 0;
  }
  for (uint32_t movie : kLoadingMovies) {
    AddMovie(g, At<MovieList>(kLoadingScreenMovies), movie);
  }
}

// OtherMovies::Load (sub_8228E6E0): the three movies loaded, their words
// cleared, then the UI movies given the render manager's renderer and
// command list (UiMovie::SetRenderer, sub_82582E90).
void LoadOtherMovies(Guest& g) {
  for (uint32_t i = 0; i < kOtherMovieIdCount; i++) {
    AddMovie(g, At<MovieList>(kOtherMovies), At<u32_be>(kOtherMovieIds)[i]);
  }
  for (uint32_t i = 0; i < kOtherMovieWordCount; i++) {
    At<u32_be>(kOtherMovieWords)[i] = 0;
  }
  *At<u32_be>(kOtherMovieWord) = 0;
  *At<u32_be>(kOtherMovieFlag) = 0;
  void* app = At<u32_be>(*At<u32_be>(kRenderApp));
  const uint32_t command_list =
      g.CallVirtual(At<u32_be>(g.CallVirtual(app, kRenderManagerSlot)), kCommandListSlot);
  const uint32_t renderer =
      g.CallVirtual(At<u32_be>(g.CallVirtual(app, kRenderManagerSlot)), kRendererSlot);
  g.Call(sub_82582E90, renderer, command_list);  // UiMovie::SetRenderer
}

// Text::Unload (sub_8210AF60): the font's teardown (sub_82109B68), then
// sub_8210A608.
void UnloadText(Guest& g) {
  g.Call(sub_82109B68);
  g.Call(sub_8210A608);
}

// BootData::Load (sub_821067A0): once ([0x826B586C]), the boot's steps
// between two calls of sub_822927E0 (GlobalMemoryStatus, probably) with a
// buffer of the original's 128-byte frame (+80).
void LoadBootData(Guest& g) {
  u32_be* loaded = At<u32_be>(kBootLoaded);
  if (*loaded != 0) {
    return;
  }
  *loaded = 1;
  struct BootLoadFrame {
    uint8_t linkage[80];
    uint8_t memory_status[32];
  };
  guest::StackFrame frame(g.ctx, 128);
  uint8_t* memory_status = At<BootLoadFrame>(frame.Local(0))->memory_status;
  g.Call(sub_822927E0, memory_status);
  g.Call(sub_820A0A88);
  g.Call(sub_8210AF20);
  CreateBootObjects(g);
  LoadLoadingScreen(g);
  LoadOtherMovies(g);
  game::InitCarParams(g);
  game::LoadCarParams(g);
  g.Call(sub_822927E0, memory_status);
}

// BootData::Unload (sub_82106858): when the boot's load ran ([0x826B586C],
// not cleared), its objects freed in turn: the car parameters, the two
// movie lists, the boot objects (the save data's teardown), the text, then
// sub_821611F0.
void UnloadBootData(Guest& g) {
  if (*At<u32_be>(kBootLoaded) == 0) {
    return;
  }
  game::FreeCarParams(g);
  FreeMovies(g, At<MovieList>(kOtherMovies));
  FreeMovies(g, At<MovieList>(kLoadingScreenMovies));
  game::TearDownBootObjects(g);
  UnloadText(g);
  g.Call(sub_821611F0);
}

}  // namespace

#define SBX_DATA_LOAD_FALLBACK(name) \
  REX_FUNC_PROLOGUE();               \
  if (!On()) {                       \
    __imp__##name(ctx, base);        \
    return;                          \
  }                                  \
  Guest g(ctx, base);

// LoadingScreen::Load.
extern "C" REX_FUNC(sub_82245750) {
  SBX_DATA_LOAD_FALLBACK(sub_82245750)
  LoadLoadingScreen(g);
}

// LoadingScreen::AddMovie: r3 the movie's asset id.
extern "C" REX_FUNC(sub_82245658) {
  SBX_DATA_LOAD_FALLBACK(sub_82245658)
  AddMovie(g, At<MovieList>(kLoadingScreenMovies), ctx.r3.u32);
}

// OtherMovies::Add: r3 the movie's asset id.
extern "C" REX_FUNC(sub_8228E658) {
  SBX_DATA_LOAD_FALLBACK(sub_8228E658)
  AddMovie(g, At<MovieList>(kOtherMovies), ctx.r3.u32);
}

// LoadingScreen::Free and OtherMovies::Free: the lists' movies freed.
extern "C" REX_FUNC(sub_822456E0) {
  SBX_DATA_LOAD_FALLBACK(sub_822456E0)
  FreeMovies(g, At<MovieList>(kLoadingScreenMovies));
}

extern "C" REX_FUNC(sub_8228E7C0) {
  SBX_DATA_LOAD_FALLBACK(sub_8228E7C0)
  FreeMovies(g, At<MovieList>(kOtherMovies));
}

// OtherMovies::Load.
extern "C" REX_FUNC(sub_8228E6E0) {
  SBX_DATA_LOAD_FALLBACK(sub_8228E6E0)
  LoadOtherMovies(g);
}

// Defaults::Init.
extern "C" REX_FUNC(sub_820CCB60) {
  SBX_DATA_LOAD_FALLBACK(sub_820CCB60)
  InitDefaults();
}

// Text::Unload.
extern "C" REX_FUNC(sub_8210AF60) {
  SBX_DATA_LOAD_FALLBACK(sub_8210AF60)
  UnloadText(g);
}

// BootData::Unload.
extern "C" REX_FUNC(sub_82106858) {
  SBX_DATA_LOAD_FALLBACK(sub_82106858)
  UnloadBootData(g);
}

// InputRecordings::InputRecordings: r3 the recordings, returned.
extern "C" REX_FUNC(sub_82105618) {
  SBX_DATA_LOAD_FALLBACK(sub_82105618)
  ConstructInputRecordings(*At<InputRecordings>(ctx.r3.u32));
}

// Replay826D8520::StaticInit and Replay82816E50::StaticInit: the two static
// replays.
extern "C" REX_FUNC(sub_825BF238) {
  SBX_DATA_LOAD_FALLBACK(sub_825BF238)
  InitStaticReplay(g, kReplay826D8520Records, kReplay826D8520Destructor);
}

extern "C" REX_FUNC(sub_825BF340) {
  SBX_DATA_LOAD_FALLBACK(sub_825BF340)
  InitStaticReplay(g, kReplay82816E50Records, kReplay82816E50Destructor);
}

// Part464::Part464: r3 the part, returned.
extern "C" REX_FUNC(sub_82104410) {
  SBX_DATA_LOAD_FALLBACK(sub_82104410)
  ConstructPart464(ctx.r3.u32);
}

// Member4080::Member4080: r3 the member, returned.
extern "C" REX_FUNC(sub_82104800) {
  SBX_DATA_LOAD_FALLBACK(sub_82104800)
  ConstructMember4080(*At<Member4080>(ctx.r3.u32));
}

// Member1136::Member1136: r3 the member, returned.
extern "C" REX_FUNC(sub_82102A10) {
  SBX_DATA_LOAD_FALLBACK(sub_82102A10)
  ConstructMember1136(ctx.r3.u32);
}

// Record5232::Record5232: r3 the record, returned.
extern "C" REX_FUNC(sub_82104D90) {
  SBX_DATA_LOAD_FALLBACK(sub_82104D90)
  ConstructRecord5232(*At<Record5232>(ctx.r3.u32));
}

// Bits224::Fill: 7 words, bits 0 to 198 set.
extern "C" REX_FUNC(sub_820CCAB0) {
  SBX_DATA_LOAD_FALLBACK(sub_820CCAB0)
  FillBits(At<u32_be>(ctx.r3.u32), 7, 199);
}

// Bits480::Fill: 15 words, bits 0 to 453 set.
extern "C" REX_FUNC(sub_820CCB08) {
  SBX_DATA_LOAD_FALLBACK(sub_820CCB08)
  FillBits(At<u32_be>(ctx.r3.u32), 15, 454);
}

// RaceData::Create.
extern "C" REX_FUNC(sub_82102390) {
  SBX_DATA_LOAD_FALLBACK(sub_82102390)
  CreateRaceData(g);
}

// InputRecordings::Create: returns the recordings.
extern "C" REX_FUNC(sub_82105678) {
  SBX_DATA_LOAD_FALLBACK(sub_82105678)
  ctx.r3.u64 = CreateInputRecordings(g);
}

// Replay::Create.
extern "C" REX_FUNC(sub_821056D0) {
  SBX_DATA_LOAD_FALLBACK(sub_821056D0)
  CreateReplay(g);
}

// BootData::CreateObjects.
extern "C" REX_FUNC(sub_821059D8) {
  SBX_DATA_LOAD_FALLBACK(sub_821059D8)
  CreateBootObjects(g);
}

// BootData::Load.
extern "C" REX_FUNC(sub_821067A0) {
  SBX_DATA_LOAD_FALLBACK(sub_821067A0)
  LoadBootData(g);
}
