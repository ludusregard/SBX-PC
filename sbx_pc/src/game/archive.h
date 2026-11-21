// The archives: BUILD.DAT and BUILD_CRS.DAT, each with its table of contents
// (`docs/game_data.md`, "The archives"). The game loads every asset by its
// id through Archive::Load, which queues a request for the loader thread;
// the loader reads it (from the .DAT, or a loose file on disc), unpacks it
// (UCL) and calls the request's callback. The game's loader reads the next
// request while it unpacks the current one; the native loader does the same,
// with the .DAT reads on a host thread into host memory.
//
// Ids from 23009 are in BUILD_CRS, at id - 23009 in its TOC.

#pragma once

#include "game/guest_types.h"

#include <filesystem>

namespace game {

using guest::i32_be;
using guest::u32_be;

constexpr uint32_t kFirstCrsId = 23009;

// --- The tables of contents ---

// A TOC entry: where an asset is in its .DAT and how big it is.
struct TocEntry {
  u32_be start_sector;   // 2,048-byte sectors into the .DAT; 0 for a loose file.
  u32_be stored_size;
  u32_be unpacked_size;  // 0 for an asset stored as it is.
  uint8_t unknown_12[4];  // Always 0 in both TOCs.
};
GUEST_SIZE(TocEntry, 16);

// A TOC: this header, then one entry per id.
struct TocHeader {
  u32_be count;
  uint8_t unknown_4[8];
  u32_be dat_size;  // The .DAT's size in bytes.
};
GUEST_SIZE(TocHeader, 16);

// An asset's size: its unpacked size, or the stored one when that's 0.
// `toc` is the TOC's guest address, `index` the id within that archive.
inline uint32_t AssetSize(uint32_t toc, uint32_t index) {
  const TocEntry* entry = guest::Element<TocEntry>(toc + sizeof(TocHeader), index);
  return entry->unpacked_size != 0 ? uint32_t(entry->unpacked_size) : uint32_t(entry->stored_size);
}

// --- The request queue ---

// The game's singly linked list: nodes linked through their +4.
struct ListNode {
  u32_be vtable;
  u32_be next;
};
GUEST_SIZE(ListNode, 8);

struct List {
  u32_be vtable;
  u32_be deletes_nodes;  // Whether the list deletes its nodes when cleared.
  u32_be count;
  u32_be head;
  u32_be tail;
};
GUEST_SIZE(List, 20);

// What a load asks for (Archive::Load builds it; the queue copies it into a
// slot). The flags say where the data comes from and what happens once it's in.
struct LoadRequest {
  static constexpr uint32_t kUnknown1 = 0x1;  // Never set by Archive::Load.
  static constexpr uint32_t kIntoDestination = 0x4;  // Read into `destination`.
  static constexpr uint32_t kUnknown20 = 0x20;
  static constexpr uint32_t kLooseOffset = 0x80;  // Set when `loose_offset` was given.
  static constexpr uint32_t kCompressed = 0x100;
  static constexpr uint32_t kLooseFile = 0x200;
  static constexpr uint32_t kFromDat = 0x800;
  static constexpr uint32_t kHasCallback = 0x2000;
  static constexpr uint32_t kCancelable = 0x4000;  // Archive::CancelQueued may drop it.

  u32_be flags;
  u32_be id;
  u32_be loose_path;  // The file's path (a guest string), or 0.
  u32_be offset;      // Bytes into the .DAT.
  u32_be loose_offset;  // Bytes into a loose file to start at (Archive::Load's r7; 0 in play).
  u32_be size;         // Bytes to end up with: unpacked, when compressed.
  u32_be stored_size;  // Bytes to read, when compressed; else 0.
  u32_be destination;
  u32_be context;   // Passed to the callback.
  u32_be callback;  // A guest function, or 0.
};
GUEST_SIZE(LoadRequest, 40);

// One of the 960 request slots: a list node with a request and the loader's
// working fields.
struct RequestSlot {
  static constexpr int32_t kFree = -1;
  static constexpr int32_t kWaiting = 0;  // Queued, not started by the loader.

  ListNode node;
  i32_be serial;  // kFree when the slot is free.
  LoadRequest request;
  i32_be state;  // kWaiting, else the loader's step for it.
  // The loader's working fields, from the request.
  u32_be dat_offset;    // Bytes into the .DAT.
  u32_be sectors;       // read_size in 2,048-byte sectors, rounded up.
  u32_be loose_offset;  // Bytes into the loose file.
  u32_be read_size;     // Bytes to read: the stored size.
  u32_be size;          // Bytes the asset ends up as.
  u32_be data;          // Where it's read to (the destination, or a staging buffer).
  uint8_t unknown_80[4];
  u32_be chained;       // The compressed request being unpacked meanwhile.
  u32_be destination;   // A compressed request's destination, while `data` stages it.
};
GUEST_SIZE(RequestSlot, 92);
GUEST_OFFSET(RequestSlot, request, 12);
GUEST_OFFSET(RequestSlot, state, 52);
GUEST_OFFSET(RequestSlot, data, 76);
GUEST_OFFSET(RequestSlot, destination, 88);

// The game's counting semaphore (a lock and a count). Waited on and signaled
// through the game's own functions; only the count is read here.
struct GuestSemaphore {
  uint8_t unknown_0[32];
  i32_be count;
  uint8_t unknown_36[4];
};
GUEST_SIZE(GuestSemaphore, 40);

// The archive's globals at 0x82694DE4.
struct ArchiveState {
  static constexpr uint32_t kSlotCount = 960;

  i32_be next_serial;
  u32_be idle_wanted;  // The loader signals `idle` once the queue drains.
  // The game's staging ring: two 5 MB halves that compressed entries are read
  // into. Only the archive's own functions use it; the native loader stages
  // on the host and uses the ring only to hand stored UCL blocks to the
  // game's memcpy.
  u32_be staging_half_size;  // 5 MB: each half of the staging ring.
  u32_be staging_count;       // Picks the staging ring's half for the next read.
  u32_be staging_halves[2];
  u32_be staging_in_use;
  u32_be build_toc;       // BUILD.TOC, read whole.
  u32_be build_toc_size;
  u32_be crs_toc;         // BUILD_CRS.TOC.
  u32_be crs_toc_size;
  // The load buffer the car and scenery code share: 0xC80000 bytes in the
  // game, --archive_shared_buffer_kb with the native Init.
  u32_be shared_buffer_size;
  u32_be shared_buffer;
  uint8_t unknown_52[48];
  RequestSlot slots[kSlotCount];
};
GUEST_OFFSET(ArchiveState, build_toc, 0x1C);
GUEST_OFFSET(ArchiveState, shared_buffer, 0x30);
GUEST_OFFSET(ArchiveState, slots, 0x64);
GUEST_SIZE(ArchiveState, 0x64 + 960 * 92);

// The archive's queue and its semaphores, at 0x826AA748.
struct ArchiveQueue {
  GuestSemaphore read_done;  // Signaled when a read has finished.
  GuestSemaphore idle;  // Archive::WaitIdle waits here.
  GuestSemaphore work;  // The loader waits here for requests.
  GuestSemaphore lock;  // Held around every change to the queue and slots.
  List requests;        // RequestSlots, oldest first.
};
GUEST_SIZE(ArchiveQueue, 180);

// The loose files' table (id, path) at 0x825F0AB0; a path of 0 ends it.
struct LooseFile {
  u32_be id;
  u32_be path;
};
GUEST_SIZE(LooseFile, 8);

// Whether --native_archive is on.
bool NativeArchive();

// The game's size for the shared load buffer (Archive::Init's r4, from
// sub_821066D0).
constexpr uint32_t kGameSharedBufferSize = 0xC80000;

// Where the game's files are (assets/): the loader reads the .DAT files from
// there. Set once at start-up.
void SetArchiveDataRoot(const std::filesystem::path& root);

// Archive::FindLooseFile's walk (sub_8209D9F8): the loose file's path for
// `id`, or 0. The patch loader's wrapper looks at its own files first.
uint32_t FindLooseFile(uint32_t id);

}  // namespace game
