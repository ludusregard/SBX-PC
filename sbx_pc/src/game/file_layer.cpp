// The file layer's objects as our own C++ (--native_file_layer, on by
// default): the file classes' constructors and destructors, deleting
// destructors included (GameFile over AsyncFile over File), GameFile's opens
// by name and the path objects (Path, the game's GamePath), which the game
// builds its file names in. The objects live on the recompiled callers'
// stacks.
//
// The file opens and reads themselves and the archive stay recompiled (and
// the patch loader's and --log_file_opens' hooks on them still apply).

#include "base/settings.h"
#include "game/std_types.h"
#include "hooks/guest_call.h"

#include <cstring>

SBX_SETTING_BOOL(native_file_layer, true, "Overrides", "Data and Saves",
                 "Our own C++ for the file layer's objects (the file classes' constructors "
                 "and destructors, GameFile's opens by name, Path::Build, GamePath's sets) "
                 "instead of the recompiled functions; opening and reading files stays "
                 "recompiled");

namespace {

using game::String;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

// The classes' vtables, from the game's class down.
constexpr uint32_t kGameFileVtable = 0x82002228;
constexpr uint32_t kAsyncFileVtable = 0x820699EC;
constexpr uint32_t kFileVtable = 0x82068E3C;
constexpr uint32_t kStreamVtable = 0x82068DD8;
constexpr uint32_t kPathVtable = 0x82069A1C;
constexpr uint32_t kGamePathVtable = 0x82002258;

// File, and AsyncFile and GameFile over it (they add no fields here).
struct File {
  u32_be vtable;
  i32_be handle;  // -1 while closed.
  u32_be position;
  u32_be size;

  void Construct(uint32_t class_vtable) {
    position = 0;
    handle = -1;
    size = 0;
    vtable = class_vtable;
  }
  // The destructors only step the vtable down to the stream's; the handle
  // isn't closed.
  void Destroy() { vtable = kStreamVtable; }
};

GUEST_SIZE(File, 16);
GUEST_OFFSET(File, handle, 4);

// Path, and GamePath over it: a file name built in place.
struct Path {
  u32_be vtable;
  char text[540];

  void Construct() {
    text[0] = 0;
    vtable = kPathVtable;
  }
};
GUEST_SIZE(Path, 544);
GUEST_OFFSET(Path, text, 4);

enum PathKind : uint32_t {
  kPathPlain = 0,
  kPathPlain2 = 1,
  kPathWithFolders = 2,  // The two folder strings below, then the name.
};
constexpr uint32_t kPlainFormat = 0x82069A28;
constexpr uint32_t kPlain2Format = 0x82069A34;
constexpr uint32_t kFoldersFormat = 0x82069A40;
constexpr uint32_t kFolderString1 = 0x8264EBF8;  // std::strings
constexpr uint32_t kFolderString2 = 0x8264EBDC;

// Path::Build: prints the path's text from `name` by the kind (kinds past
// the three print nothing). Returns the text, or nullptr for no name.
char* BuildPath(Guest& g, Path* path, uint32_t kind, const char* name) {
  if (name == nullptr) {
    return nullptr;
  }
  if (kind == kPathWithFolders) {
    guest_call::InFrame(g.ctx, g.base, 96, 0x825792F0, [&](uint32_t) {
      g.Call(sub_8258ED70, path->text, kFoldersFormat, At<String>(kFolderString2)->Text(),
             At<String>(kFolderString1)->Text(), name);  // sprintf
    });
  } else if (kind == kPathPlain || kind == kPathPlain2) {
    guest_call::InFrame(g.ctx, g.base, 96, 0x82579310, [&](uint32_t) {
      g.Call(sub_8258ED70, path->text, kind == kPathPlain ? kPlainFormat : kPlain2Format, name);
    });
  }
  return path->text;
}

// The frame the opens by name and GamePath's sets build their Path in, as
// the originals have it.
constexpr uint32_t kOpenByNameFrame = 640;
constexpr uint32_t kSetPathFrame = 624;
struct PathFrame {
  uint8_t linkage[80];
  Path path;
};
GUEST_OFFSET(PathFrame, path, 80);

// GameFile's opens by name: the name made a plain path in a Path of the
// original's frame, then the file's own open (`open`, with `mode` passed on)
// on its text. True when it opened.
bool OpenByName(Guest& g, File* file, const char* name, uint32_t mode, Guest::Function* open) {
  guest::StackFrame frame(g.ctx, kOpenByNameFrame);
  Path* path = &At<PathFrame>(frame.Local(0))->path;
  path->Construct();
  const char* text = BuildPath(g, path, kPathPlain, name);
  const bool opened = uint8_t(g.Call(open, file, text, mode)) != 0;
  path->vtable = kPathVtable;  // ~Path
  return opened;
}

// GamePath's sets: the name built by `kind` in a Path of the original's
// frame and its text copied over the game path's. Returns the game path's
// text.
char* SetGamePath(Guest& g, Path* game_path, uint32_t kind, const char* name,
                  uint32_t return_address) {
  guest_call::InFrame(g.ctx, g.base, kSetPathFrame, return_address, [&](uint32_t frame) {
    Path* built = &At<PathFrame>(frame)->path;
    built->Construct();
    if (const char* text = BuildPath(g, built, kind, name)) {
      std::strcpy(game_path->text, text);
    }
    built->vtable = kPathVtable;  // ~Path
  });
  return game_path->text;
}

// What the deleting destructors do after the destructor: the object freed
// (operator delete) when bit 0 of the flags is set.
void DeleteIfAsked(Guest& g, void* object, uint32_t flags) {
  if ((flags & 1) != 0) {
    g.Call(sub_8258C868, object);  // operator delete
  }
}

}  // namespace

#define SBX_FILE_LAYER_FALLBACK(name)     \
  REX_FUNC_PROLOGUE();                    \
  if (!SBX_GET(native_file_layer)) {  \
    __imp__##name(ctx, base);             \
    return;                               \
  }                                       \
  Guest g(ctx, base);

// The constructors and destructors: r3 = the object, and r3 left as it.

extern "C" REX_FUNC(sub_82572488) {  // File::File
  SBX_FILE_LAYER_FALLBACK(sub_82572488)
  At<File>(ctx.r3.u32)->Construct(kFileVtable);
}

extern "C" REX_FUNC(sub_82572130) {  // File::~File
  SBX_FILE_LAYER_FALLBACK(sub_82572130)
  At<File>(ctx.r3.u32)->Destroy();
}

extern "C" REX_FUNC(sub_82579070) {  // AsyncFile::AsyncFile
  SBX_FILE_LAYER_FALLBACK(sub_82579070)
  At<File>(ctx.r3.u32)->Construct(kAsyncFileVtable);
}

extern "C" REX_FUNC(sub_825790B0) {  // AsyncFile::~AsyncFile
  SBX_FILE_LAYER_FALLBACK(sub_825790B0)
  At<File>(ctx.r3.u32)->Destroy();
}

extern "C" REX_FUNC(sub_820A69B8) {  // GameFile::GameFile
  SBX_FILE_LAYER_FALLBACK(sub_820A69B8)
  At<File>(ctx.r3.u32)->Construct(kGameFileVtable);
}

extern "C" REX_FUNC(sub_820A69F8) {  // GameFile::~GameFile
  SBX_FILE_LAYER_FALLBACK(sub_820A69F8)
  At<File>(ctx.r3.u32)->Destroy();
}

extern "C" REX_FUNC(sub_825791D8) {  // Path::Path
  SBX_FILE_LAYER_FALLBACK(sub_825791D8)
  At<Path>(ctx.r3.u32)->Construct();
}

extern "C" REX_FUNC(sub_825791F8) {  // Path::~Path
  SBX_FILE_LAYER_FALLBACK(sub_825791F8)
  At<Path>(ctx.r3.u32)->vtable = kPathVtable;
}

extern "C" REX_FUNC(sub_820A6B98) {  // GamePath::GamePath (also its destructor)
  SBX_FILE_LAYER_FALLBACK(sub_820A6B98)
  At<Path>(ctx.r3.u32)->vtable = kGamePathVtable;
}

// Path::Build: r3 = the path, r4 = the kind, r5 = the name. Returns the
// text, or 0 for no name.
extern "C" REX_FUNC(sub_82579250) {
  SBX_FILE_LAYER_FALLBACK(sub_82579250)
  ctx.r3.u64 = guest::AddressOf(
      BuildPath(g, At<Path>(ctx.r3.u32), ctx.r4.u32, At<char>(ctx.r5.u32)));
}

// GamePath::SetGame: r3 = the game's path, r4 = the name: the game path's
// text set to the plain path of the name. Returns the text.
extern "C" REX_FUNC(sub_820A6BA8) {
  SBX_FILE_LAYER_FALLBACK(sub_820A6BA8)
  ctx.r3.u64 = guest::AddressOf(
      SetGamePath(g, At<Path>(ctx.r3.u32), kPathPlain, At<char>(ctx.r4.u32), 0x820A6BDC));
}

// GamePath::Set: as SetGame with the second plain kind.
extern "C" REX_FUNC(sub_820A6C20) {
  SBX_FILE_LAYER_FALLBACK(sub_820A6C20)
  ctx.r3.u64 = guest::AddressOf(
      SetGamePath(g, At<Path>(ctx.r3.u32), kPathPlain2, At<char>(ctx.r4.u32), 0x820A6C54));
}

// GameFile::OpenWrite and OpenAsync by name: r3 = the file, r4 = the name,
// r5 passed on to File::OpenWrite (sub_82572140) or AsyncFile::Open
// (sub_825790C0). Return 1 when the file opened, else 0.
extern "C" REX_FUNC(sub_820A6A08) {
  SBX_FILE_LAYER_FALLBACK(sub_820A6A08)
  ctx.r3.u64 = OpenByName(g, At<File>(ctx.r3.u32), At<char>(ctx.r4.u32), ctx.r5.u32, sub_82572140);
}

extern "C" REX_FUNC(sub_820A6AD8) {
  SBX_FILE_LAYER_FALLBACK(sub_820A6AD8)
  ctx.r3.u64 = OpenByName(g, At<File>(ctx.r3.u32), At<char>(ctx.r4.u32), ctx.r5.u32, sub_825790C0);
}

// The deleting destructors (each class's vtable slot 0): r3 = the object,
// r4 = the flags. The destructor, then the object freed with bit 0. Return
// the object.
extern "C" REX_FUNC(sub_820A6B40) {  // GameFile
  SBX_FILE_LAYER_FALLBACK(sub_820A6B40)
  File* file = At<File>(ctx.r3.u32);
  file->Destroy();
  DeleteIfAsked(g, file, ctx.r4.u32);
  ctx.r3.u64 = guest::AddressOf(file);
}

extern "C" REX_FUNC(sub_82579178) {  // AsyncFile
  SBX_FILE_LAYER_FALLBACK(sub_82579178)
  File* file = At<File>(ctx.r3.u32);
  file->Destroy();
  DeleteIfAsked(g, file, ctx.r4.u32);
  ctx.r3.u64 = guest::AddressOf(file);
}

extern "C" REX_FUNC(sub_82579208) {  // Path
  SBX_FILE_LAYER_FALLBACK(sub_82579208)
  Path* path = At<Path>(ctx.r3.u32);
  path->vtable = kPathVtable;
  DeleteIfAsked(g, path, ctx.r4.u32);
  ctx.r3.u64 = guest::AddressOf(path);
}

extern "C" REX_FUNC(sub_820A6C98) {  // GamePath
  SBX_FILE_LAYER_FALLBACK(sub_820A6C98)
  Path* path = At<Path>(ctx.r3.u32);
  path->vtable = kGamePathVtable;
  DeleteIfAsked(g, path, ctx.r4.u32);
  ctx.r3.u64 = guest::AddressOf(path);
}

// AsyncFile::Detach: r3 = the file. Returns its handle and forgets it
// (without closing it).
extern "C" REX_FUNC(sub_82579160) {
  SBX_FILE_LAYER_FALLBACK(sub_82579160)
  File* file = At<File>(ctx.r3.u32);
  const int32_t handle = file->handle;
  file->handle = -1;
  ctx.r3.u64 = uint32_t(handle);
}

// Path::Text: r3 = the path. Returns its text.
extern "C" REX_FUNC(sub_825791F0) {
  SBX_FILE_LAYER_FALLBACK(sub_825791F0)
  ctx.r3.u64 = guest::AddressOf(At<Path>(ctx.r3.u32)->text);
}
