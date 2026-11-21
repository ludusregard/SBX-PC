// The archive as our own C++ (--native_archive, on by default): Archive::Init
// and Shutdown, the TOC lookups, Archive::Load and the request queue
// (queueing, cancelling, waiting for it to drain), the list the queue is made
// of, the UCL unpacking, and the loader thread (`docs/native-code.md`,
// "native_archive").
//
// The queue's state stays where the game keeps it (ArchiveState and
// ArchiveQueue, in guest memory): the game's functions that are still
// recompiled (the loose read, the semaphores) work on the same slots under
// the same lock.

#include "game/archive.h"

#include "base/settings.h"
#include "game/ucl.h"
#include "hooks/native_calls.h"

#include <rex/logging.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>

SBX_SETTING_BOOL(native_archive, true, "Overrides", "Data and Saves",
                 "Our own C++ for the archive (the TOC lookups, Archive::Load, the request "
                 "queue, Archive::Init, the loader thread with its .DAT reads and UCL "
                 "unpacking) instead of the recompiled functions. The loader thread's choice "
                 "is made once, at boot");
SBX_SETTING_BOOL(log_archive, false, "Logging", "Debugging",
                 "Log every archive load the native loader finishes: id, where it came "
                 "from, its size, and how long its read (and how much of it the loader "
                 "waited for), unpacking and callback took");
SBX_SETTING_INT(archive_shared_buffer_kb, game::kGameSharedBufferSize / 1024, "Hooks", "Patches",
                "The size in KiB of the load buffer the car and scenery code share (set b "
                "and c car bodies, decals, area models); the game's is 12800 (12.5 MiB), "
                "also the smallest allowed. For mods with bigger files. Needs "
                "--native_archive; takes effect at the next start", sbx::kRestart);

namespace {

std::filesystem::path g_data_root;

}  // namespace

namespace game {

bool NativeArchive() { return SBX_GET(native_archive); }

void SetArchiveDataRoot(const std::filesystem::path& root) { g_data_root = root; }

}  // namespace game

namespace {

using game::ArchiveQueue;
using game::ArchiveState;
using game::GuestSemaphore;
using game::kFirstCrsId;
using game::List;
using game::ListNode;
using game::LoadRequest;
using game::LooseFile;
using game::RequestSlot;
using game::TocEntry;
using game::TocHeader;
using guest::AddressOf;
using guest::At;
using guest::Guest;
using guest::u32_be;

constexpr uint32_t kArchiveState = 0x82694DE4;
constexpr uint32_t kArchiveQueue = 0x826AA748;
constexpr uint32_t kLooseFiles = 0x825F0AB0;

// Vtables the constructors store (read-only image data).
constexpr uint32_t kListNodeVtable = 0x82000A40;
constexpr uint32_t kListVtable = 0x820125F0;

// Every load is cancelable and goes into its destination.
constexpr uint32_t kLoadFlags =
    LoadRequest::kCancelable | LoadRequest::kUnknown20 | LoadRequest::kIntoDestination;

ArchiveState& State() { return *At<ArchiveState>(kArchiveState); }
ArchiveQueue& Queue() { return *At<ArchiveQueue>(kArchiveQueue); }

// The TOC an id is in, and the id's entry there.
uint32_t TocFor(uint32_t id) {
  return id < kFirstCrsId ? State().build_toc : State().crs_toc;
}

TocEntry& EntryFor(uint32_t id) {
  const uint32_t index = id < kFirstCrsId ? id : id - kFirstCrsId;
  return *guest::Element<TocEntry>(TocFor(id) + uint32_t(sizeof(TocHeader)), index);
}

// --- The list ---

bool Append(List& list, ListNode* node) {
  if (node == nullptr) {
    return false;
  }
  if (ListNode* tail = At<ListNode>(list.tail)) {
    tail->next = AddressOf(node);
  } else {
    list.head = AddressOf(node);
  }
  node->next = 0;
  list.tail = AddressOf(node);
  list.count += 1;
  return true;
}

bool Remove(List& list, ListNode* node) {
  if (node == nullptr) {
    return false;
  }
  ListNode* previous = nullptr;
  ListNode* at = At<ListNode>(list.head);
  while (at != nullptr && at != node) {
    previous = at;
    at = At<ListNode>(at->next);
  }
  if (at == nullptr) {
    return false;
  }
  // The removed node keeps its link, so a walk standing on it can go on.
  if (previous != nullptr) {
    previous->next = at->next;
  }
  if (At<ListNode>(list.head) == at) {
    list.head = at->next;
  }
  if (At<ListNode>(list.tail) == at) {
    list.tail = AddressOf(previous);
  }
  list.count -= 1;
  return true;
}

// --- The queue ---

// The game's semaphore calls.
void Wait(Guest& g, GuestSemaphore& semaphore) { g.Call(sub_82577EF0, &semaphore); }
void Signal(Guest& g, GuestSemaphore& semaphore) { g.Call(sub_82577F68, &semaphore); }

// While one lives, the queue's lock is held.
class QueueLock {
 public:
  explicit QueueLock(Guest& g) : g_(g) { Wait(g_, Queue().lock); }
  ~QueueLock() { Signal(g_, Queue().lock); }
  QueueLock(const QueueLock&) = delete;
  QueueLock& operator=(const QueueLock&) = delete;

 private:
  Guest& g_;
};

// The slot holding `serial` (RequestSlot::kFree for a free one), or nullptr.
RequestSlot* FindSlot(int32_t serial) {
  for (RequestSlot& slot : State().slots) {
    if (slot.serial == serial) {
      return &slot;
    }
  }
  return nullptr;
}

void ClearLoaderFields(RequestSlot& slot) {
  slot.dat_offset = 0;
  slot.sectors = 0;
  slot.loose_offset = 0;
  slot.read_size = 0;
  slot.size = 0;
  slot.data = 0;
  std::memset(slot.unknown_80, 0, sizeof(slot.unknown_80));
  slot.chained = 0;
  slot.destination = 0;
}

// A request into a free slot at the end of the queue: its serial, or -1 when
// every slot is taken.
int32_t QueueRequest(Guest& g, const LoadRequest& request) {
  QueueLock lock(g);
  RequestSlot* slot = FindSlot(RequestSlot::kFree);
  if (slot == nullptr) {
    return -1;
  }
  // Wake the loader if it's idle: nothing queued and no wake pending.
  ArchiveQueue& queue = Queue();
  if (queue.requests.head == 0 && queue.work.count == 0) {
    Signal(g, queue.work);
  }

  const int32_t serial = State().next_serial;
  slot->serial = serial;
  slot->request = request;
  ClearLoaderFields(*slot);
  slot->state = RequestSlot::kWaiting;
  // Serials count up to 0x7FFFFFFF, then start again from 0.
  State().next_serial = (serial + 1 < 0x7FFFFFFF) ? serial + 1 : 0;
  Append(queue.requests, &slot->node);
  return serial;
}

// Archive::WaitIdle: returns once the loader has finished everything queued.
void WaitIdle(Guest& g) {
  {
    QueueLock lock(g);
    if (Queue().requests.count != 0) {
      State().idle_wanted = 1;
    }
  }
  if (State().idle_wanted != 0) {
    Wait(g, Queue().idle);
  }
}


// --- The loader thread ---
//
// The game's loader overlaps work: while one compressed asset is unpacked,
// the next is read (asynchronously, into the other half of a 5 MB staging
// ring). Ours does the same with a host thread: each .DAT read goes into host
// memory on the reader thread, so while the loader thread unpacks (or copies)
// a request and runs its callback, the next request's bytes are being read.
// A compressed entry has no size limit. Loose files are read through the
// game's file class on the loader thread, as before.
//
// What stays as the game has it: callbacks run on the loader thread, one per
// request, in queue order, with the same arguments; the next request is
// marked as started (so Archive::Cancel refuses it) at the same point as the
// game's picker marks it: before the current one's callback when the current
// one is compressed, after it otherwise. A read started ahead of that point
// is only a host read: it is dropped if the request is cancelled first.

constexpr uint32_t kSectorSize = 2048;
// The serial of the request whose callback ran last (the game sets it; no
// reader found).
constexpr uint32_t kCallbackSerial = 0x825F0A8C;

using Clock = std::chrono::steady_clock;

double MillisecondsSince(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

// The two archives, opened by the loader on the host and read on its reader
// thread.
struct DatFiles {
  std::ifstream build;
  std::ifstream crs;
  uint64_t build_size = 0;
  uint64_t crs_size = 0;

  bool Open() {
    const std::filesystem::path folder = g_data_root / "DATA" / "BUILD" / "TXR";
    return OpenOne(build, build_size, folder / "BUILD.DAT") &&
           OpenOne(crs, crs_size, folder / "BUILD_CRS.DAT");
  }

  static bool OpenOne(std::ifstream& file, uint64_t& size, const std::filesystem::path& path) {
    std::error_code error;
    size = std::filesystem::file_size(path, error);
    if (error) {
      REXLOG_ERROR("archive: can't open {}: {}", path.string(), error.message());
      return false;
    }
    file.open(path, std::ios::binary);
    if (!file) {
      REXLOG_ERROR("archive: can't open {}", path.string());
      return false;
    }
    return true;
  }

  // Up to `size` bytes from `offset` in the id's archive into `to`, cut short
  // at the file's end. The bytes read, or -1 when the read fails.
  int64_t Read(uint32_t id, uint64_t offset, uint8_t* to, uint64_t size) {
    std::ifstream& file = id < kFirstCrsId ? build : crs;
    const uint64_t file_size = id < kFirstCrsId ? build_size : crs_size;
    if (offset > file_size) {
      return -1;
    }
    size = std::min(size, file_size - offset);
    file.clear();
    file.seekg(std::streamoff(offset));
    file.read(reinterpret_cast<char*>(to), std::streamsize(size));
    return file ? int64_t(size) : -1;
  }
};

// Where a request's .DAT read starts, and how much it reads: whole sectors
// of its stored size (its size when it isn't compressed), as the game reads.
uint32_t ReadSize(const LoadRequest& request) {
  return (request.flags & LoadRequest::kCompressed) != 0 ? uint32_t(request.stored_size)
                                                          : uint32_t(request.size);
}

uint32_t SectorsToRead(const LoadRequest& request) {
  return uint32_t((uint64_t(ReadSize(request)) + kSectorSize - 1) / kSectorSize);
}

uint32_t DatOffset(const LoadRequest& request) {
  return (request.flags & LoadRequest::kFromDat) != 0 ? uint32_t(request.offset) : 0;
}

// One .DAT read: which request it is for, and the bytes once read.
struct DatRead {
  // A buffer that grew past this is let go once its request is done, so a
  // rare big entry doesn't keep its memory.
  static constexpr size_t kKeptCapacity = 16 * 1024 * 1024;

  // The request it was started for (slot and serial), and what it reads.
  RequestSlot* slot = nullptr;
  int32_t serial = RequestSlot::kFree;
  uint32_t id = 0;
  uint32_t offset = 0;
  uint64_t length = 0;  // Whole sectors.

  // The host buffer, and what the reader thread put in it.
  std::unique_ptr<uint8_t[]> bytes;
  size_t capacity = 0;
  size_t size = 0;  // Bytes read.
  bool ok = false;
  double read_ms = 0;

  // Set up for `slot`'s request (started or not): its sectors from its
  // offset.
  void Prepare(RequestSlot& for_slot) {
    slot = &for_slot;
    serial = for_slot.serial;
    id = for_slot.request.id;
    offset = DatOffset(for_slot.request);
    length = uint64_t(SectorsToRead(for_slot.request)) * kSectorSize;
    if (capacity < length) {
      // A failed allocation (an absurd size from a broken TOC) fails the read.
      bytes.reset(new (std::nothrow) uint8_t[length]);
      capacity = bytes ? length : 0;
    }
    size = 0;
    ok = false;
    read_ms = 0;
  }

  // Whether this read is the one `for_slot` needs (the same request, still
  // queued under the same serial).
  bool IsFor(const RequestSlot& for_slot) const {
    return slot == &for_slot && serial == for_slot.serial && id == for_slot.request.id &&
           offset == DatOffset(for_slot.request) &&
           length == uint64_t(SectorsToRead(for_slot.request)) * kSectorSize;
  }

  void Release() {
    slot = nullptr;
    serial = RequestSlot::kFree;
    if (capacity > kKeptCapacity) {
      bytes.reset();
      capacity = 0;
    }
  }
};

// The host thread the .DAT reads run on: one read at a time.
class DatReader {
 public:
  explicit DatReader(DatFiles& dats) : dats_(dats) {
    // It lives as long as the loader thread, which never returns.
    std::thread([this] { Run(); }).detach();
  }

  // Starts `read` (prepared). The reader must be idle: the loader never has
  // more than one read in flight.
  void Start(DatRead& read) {
    std::lock_guard lock(mutex_);
    job_ = &read;
    busy_ = true;
    wake_.notify_all();
  }

  // Waits until the read in flight (if any) is done.
  void Finish() {
    std::unique_lock lock(mutex_);
    wake_.wait(lock, [this] { return !busy_; });
  }

 private:
  void Run() {
    for (;;) {
      DatRead* read;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return job_ != nullptr; });
        read = job_;
        job_ = nullptr;
      }
      const auto start = Clock::now();
      const int64_t got = read->capacity >= read->length
                              ? dats_.Read(read->id, read->offset, read->bytes.get(), read->length)
                              : -1;
      read->ok = got >= 0;
      read->size = got >= 0 ? size_t(got) : 0;
      read->read_ms = MillisecondsSince(start);
      {
        std::lock_guard lock(mutex_);
        busy_ = false;
      }
      wake_.notify_all();
    }
  }

  DatFiles& dats_;
  std::mutex mutex_;
  std::condition_variable wake_;
  DatRead* job_ = nullptr;
  bool busy_ = false;
};

// Everything the loader thread keeps between requests.
struct Loader {
  DatFiles dats;
  std::unique_ptr<DatReader> reader;
  DatRead reads[2];  // The current request's and the next one's.
};

bool FromDat(const RequestSlot& slot) {
  return (slot.request.flags & LoadRequest::kLooseFile) == 0;
}

bool Compressed(const RequestSlot& slot) {
  return (slot.request.flags & LoadRequest::kCompressed) != 0;
}

// Whether the game starts `next` while `current` is still to be unpacked and
// delivered: only when `current` is compressed, and not when both have flag
// 0x1, or when `next` is read straight into the destination `current` is
// unpacked into.
bool StartsEarly(const RequestSlot& current, const RequestSlot& next) {
  const uint32_t current_flags = current.request.flags;
  const uint32_t next_flags = next.request.flags;
  if ((current_flags & LoadRequest::kCompressed) == 0) {
    return false;
  }
  if ((current_flags & next_flags & LoadRequest::kUnknown1) != 0) {
    return false;
  }
  if ((next_flags & LoadRequest::kCompressed) == 0 &&
      (current_flags & next_flags & LoadRequest::kIntoDestination) != 0 &&
      next.request.destination == current.request.destination) {
    return false;
  }
  return true;
}

// The first request the loader hasn't started, or nullptr. Under the lock.
RequestSlot* FirstWaiting() {
  RequestSlot* slot = At<RequestSlot>(Queue().requests.head);
  while (slot != nullptr && slot->state != RequestSlot::kWaiting) {
    slot = At<RequestSlot>(slot->node.next);
  }
  return slot;
}

// The next waiting request, marked as being loaded (so Archive::Cancel leaves
// it), its loader fields set up as the game's set-up does; or nullptr. With
// `current` (a request not yet delivered), only when the game would start the
// next one this early (StartsEarly).
RequestSlot* StartNext(Guest& g, const RequestSlot* current = nullptr) {
  QueueLock lock(g);
  RequestSlot* slot = FirstWaiting();
  if (slot == nullptr || (current != nullptr && !StartsEarly(*current, *slot))) {
    return nullptr;
  }
  // A wake left over from Archive::Queue is used up, as the game's picker
  // does (it never blocks: the count isn't 0).
  if (Queue().work.count != 0) {
    Wait(g, Queue().work);
  }

  const LoadRequest& request = slot->request;
  const uint32_t flags = request.flags;
  ClearLoaderFields(*slot);
  slot->state = (flags & LoadRequest::kLooseFile) != 0 ? 7 : 5;  // The game's read steps.
  slot->dat_offset = DatOffset(request);
  if ((flags & LoadRequest::kLooseOffset) != 0) {
    slot->loose_offset = request.loose_offset;
  }
  slot->size = request.size;
  slot->read_size = ReadSize(request);
  slot->sectors = SectorsToRead(request);
  // The data goes straight to the destination (the game stages a compressed
  // one in its ring and keeps the destination aside, as here).
  slot->data = request.destination;
  if ((flags & LoadRequest::kCompressed) != 0) {
    slot->destination = request.destination;
  }
  return slot;
}

// The next waiting request, not started: what to read ahead. Or nullptr.
RequestSlot* PeekNext(Guest& g) {
  QueueLock lock(g);
  return FirstWaiting();
}

// The room a load into `destination` has when the destination is in the
// shared load buffer (the set b and c bodies go in with no size check); else
// no limit.
uint64_t RoomAt(uint32_t destination) {
  const ArchiveState& state = State();
  const uint32_t buffer = state.shared_buffer;
  const uint32_t size = state.shared_buffer_size;
  if (buffer == 0 || destination < buffer || destination - buffer >= size) {
    return UINT64_MAX;
  }
  return uint64_t(size) - (destination - buffer);
}

// Whether the request fits where it goes; logged when it doesn't. A load that
// doesn't fit isn't written at all (the buffer keeps what it held) and its
// callback still runs, so the game's loading goes on: the game itself has no
// failed-load path but the dirty-disc screen, and a truncated XPR would point
// past the buffer.
bool Fits(const RequestSlot& slot) {
  const uint64_t room = RoomAt(slot.request.destination);
  if (uint64_t(slot.request.size) <= room) {
    return true;
  }
  REXLOG_ERROR("archive: id {} ({} bytes) doesn't fit the shared load buffer ({} bytes from "
               "{:08X}); not loaded. A bigger --archive_shared_buffer_kb makes room",
               uint32_t(slot.request.id), uint32_t(slot.request.size), room,
               uint32_t(slot.request.destination));
  return false;
}

// A stored UCL block, copied with the game's memcpy as the game's Ucl::Unpack
// does (the renderer watches it for copies into GPU memory). The block is in
// host memory, so it goes through the game's staging ring, which the native
// loader doesn't otherwise use.
void CopyStoredBlock(Guest& g, uint8_t* to, const uint8_t* from, size_t size) {
  const ArchiveState& state = State();
  const uint32_t ring = state.staging_halves[0];
  const uint32_t ring_size = 2 * uint32_t(state.staging_half_size);
  if (ring == 0 || ring_size == 0) {
    std::memcpy(to, from, size);
    return;
  }
  while (size != 0) {
    const uint32_t part = uint32_t(std::min<size_t>(size, ring_size));
    std::memcpy(At<uint8_t>(ring), from, part);
    g.Call(sub_8258CB90, to, ring, part);  // memcpy
    to += part;
    from += part;
    size -= part;
  }
}

// How long a request's steps took, for --log_archive.
struct LoadTimes {
  double read_ms = 0;    // The read itself (on the reader thread for a .DAT).
  double waited_ms = 0;  // How long the loader waited for it.
  double unpack_ms = 0;  // Unpacking, or copying out of the host buffer.
  double callback_ms = 0;
};

// The request's bytes into its destination: a .DAT read (done) unpacked or
// copied out of its host buffer.
void Deliver(Guest& g, RequestSlot& slot, const DatRead& read, LoadTimes& times) {
  const LoadRequest& request = slot.request;
  const auto start = Clock::now();
  if (!read.ok) {
    REXLOG_ERROR("archive: reading id {} ({} bytes at {}) failed", uint32_t(request.id),
                 read.length, read.offset);
  } else if (Fits(slot)) {
    uint8_t* destination = At<uint8_t>(request.destination);
    if (Compressed(slot)) {
      const size_t stream_length = ucl::StreamLength(read.bytes.get(), read.size);
      const bool ok = stream_length != 0 &&
                      ucl::Unpack({read.bytes.get(), stream_length},
                                  {destination, uint32_t(request.size)},
                                  [&g](uint8_t* to, const uint8_t* from, size_t size) {
                                    CopyStoredBlock(g, to, from, size);
                                  });
      if (!ok) {
        REXLOG_ERROR("archive: id {} didn't unpack to its {} bytes", uint32_t(request.id),
                     uint32_t(request.size));
      }
    } else {
      // The asset's own bytes (the game's read also writes the rest of the
      // last sector, which nothing reads).
      const size_t size = std::min<size_t>(uint32_t(request.size), read.size);
      if (size < uint32_t(request.size)) {
        REXLOG_ERROR("archive: id {}: {} of its {} bytes read", uint32_t(request.id), size,
                     uint32_t(request.size));
      }
      std::memcpy(destination, read.bytes.get(), size);
    }
  }
  times.unpack_ms = MillisecondsSince(start);
}

// The request's callback, then its slot back to the free ones.
void FinishRequest(Guest& g, RequestSlot& slot) {
  const LoadRequest& request = slot.request;
  if ((request.flags & LoadRequest::kHasCallback) != 0) {
    *At<u32_be>(kCallbackSerial) = uint32_t(slot.serial);
    g.CallAddress(request.callback, uint32_t(slot.serial), uint32_t(slot.data),
                  uint32_t(request.size), uint32_t(request.context));
  }
  QueueLock lock(g);
  Remove(Queue().requests, &slot.node);
  slot.serial = RequestSlot::kFree;
  slot.state = RequestSlot::kWaiting;
  slot.request.flags = 0;
}

void LogLoad(const RequestSlot& slot, const LoadTimes& times) {
  const uint32_t id = slot.request.id;
  if (!FromDat(slot)) {
    REXLOG_INFO("archive: id {} from a loose file, {} bytes: read {:.2f} ms (on the loader "
                "thread), callback {:.2f} ms",
                id, uint32_t(slot.request.size), times.read_ms, times.callback_ms);
    return;
  }
  // The read ran on the reader thread; the loader only waited for the part
  // of it that the previous request's delivery didn't cover.
  const double overlapped = std::max(0.0, times.read_ms - times.waited_ms);
  REXLOG_INFO("archive: id {} from {}, {} bytes{}: read {:.2f} ms ({:.2f} ms waited for, "
              "{:.2f} ms overlapped), {} {:.2f} ms, callback {:.2f} ms",
              id, id < kFirstCrsId ? "BUILD.DAT" : "BUILD_CRS.DAT", uint32_t(slot.request.size),
              Compressed(slot) ? " (compressed)" : "", times.read_ms, times.waited_ms,
              overlapped, Compressed(slot) ? "unpack" : "copy", times.unpack_ms,
              times.callback_ms);
}

// Loads every queued request, in queue order, until none is waiting.
void LoadQueued(Guest& g, Loader& loader) {
  DatRead* current_read = &loader.reads[0];
  DatRead* ahead = &loader.reads[1];  // The next request's read, when started.
  RequestSlot* slot = StartNext(g);
  while (slot != nullptr) {
    LoadTimes times;
    const bool from_dat = FromDat(*slot);

    // This request's bytes: the read started ahead for it, or one now.
    if (ahead->slot != nullptr && !(from_dat && ahead->IsFor(*slot))) {
      loader.reader->Finish();  // Not this request's (it was cancelled): dropped.
      ahead->Release();
    }
    const auto wait_start = Clock::now();
    if (from_dat) {
      if (ahead->slot != nullptr) {
        std::swap(current_read, ahead);
      } else {
        current_read->Prepare(*slot);
        loader.reader->Start(*current_read);
      }
      loader.reader->Finish();
      times.waited_ms = MillisecondsSince(wait_start);
      times.read_ms = current_read->read_ms;
    } else {
      // The game's loose read: opens game:\<path> through its file class,
      // seeks to loose_offset and reads read_size bytes into `data`, then
      // signals read_done (taken back here, as the game's loader does).
      if (Compressed(*slot)) {
        REXLOG_ERROR("archive: id {} is a compressed loose file; not supported",
                     uint32_t(slot->request.id));
      } else if (Fits(*slot)) {
        g.Call(sub_8209E0E0, slot);  // the loose read
        Wait(g, Queue().read_done);
      }
      times.waited_ms = MillisecondsSince(wait_start);
      times.read_ms = times.waited_ms;
    }

    // The next request's read starts now, to run while this one is
    // delivered; the next request itself is only started early when the game
    // would start it.
    if (RequestSlot* following = PeekNext(g); following != nullptr && FromDat(*following)) {
      ahead->Prepare(*following);
      loader.reader->Start(*ahead);
    }
    RequestSlot* next = StartNext(g, slot);

    if (from_dat) {
      Deliver(g, *slot, *current_read, times);
      current_read->Release();
    }
    const auto callback_start = Clock::now();
    const RequestSlot finished = *slot;  // For the log, once the slot is free.
    FinishRequest(g, *slot);
    times.callback_ms = MillisecondsSince(callback_start);
    if (SBX_GET(log_archive)) {
      LogLoad(finished, times);
    }

    slot = next != nullptr ? next : StartNext(g);
  }
  if (ahead->slot != nullptr) {
    loader.reader->Finish();
    ahead->Release();
  }
}

// The loader thread's loop: never returns.
[[noreturn]] void RunLoader(Guest& g, Loader& loader) {
  for (;;) {
    Wait(g, Queue().work);
    LoadQueued(g, loader);
    // Archive::WaitIdle asked to hear when the queue drained.
    if (State().idle_wanted != 0) {
      State().idle_wanted = 0;
      Signal(g, Queue().idle);
    }
  }
}

}  // namespace

namespace game {

uint32_t FindLooseFile(uint32_t id) {
  // The original also reads a second table after 23,009 entries; the table
  // ends long before that.
  for (const LooseFile* file = At<LooseFile>(kLooseFiles); file->path != 0; file++) {
    if (file->id == id) {
      return file->path;
    }
  }
  return 0;
}

}  // namespace game

#define SBX_ARCHIVE_FALLBACK(name)     \
  REX_FUNC_PROLOGUE();                 \
  if (!game::NativeArchive()) {        \
    __imp__##name(ctx, base);          \
    return;                            \
  }                                    \
  Guest g(ctx, base);

// --- The list ---

// ListNode::ListNode: r3 the node, r4 its link. Returns it.
extern "C" REX_FUNC(sub_822925A8) {
  SBX_ARCHIVE_FALLBACK(sub_822925A8)
  ListNode* node = At<ListNode>(ctx.r3.u32);
  node->next = ctx.r4.u32;
  node->vtable = kListNodeVtable;
}

// List::List: r3 the list, r4 whether it deletes its nodes; empty. Returns it.
extern "C" REX_FUNC(sub_822925C0) {
  SBX_ARCHIVE_FALLBACK(sub_822925C0)
  List* list = At<List>(ctx.r3.u32);
  list->deletes_nodes = ctx.r4.u32;
  list->vtable = kListVtable;
  list->count = 0;
  list->head = 0;
  list->tail = 0;
}

// List::Append: r3 the list, r4 the node; 1, or 0 for no node.
extern "C" REX_FUNC(sub_822925E8) {
  SBX_ARCHIVE_FALLBACK(sub_822925E8)
  ctx.r3.u64 = Append(*At<List>(ctx.r3.u32), At<ListNode>(ctx.r4.u32)) ? 1 : 0;
}

// List::Remove: r3 the list, r4 the node; 1, or 0 when it isn't in the list.
extern "C" REX_FUNC(sub_82292640) {
  SBX_ARCHIVE_FALLBACK(sub_82292640)
  ctx.r3.u64 = Remove(*At<List>(ctx.r3.u32), At<ListNode>(ctx.r4.u32)) ? 1 : 0;
}

// --- The TOCs ---

// Archive::GetToc: r3 an id; the TOC it's in.
extern "C" REX_FUNC(sub_8209D988) {
  SBX_ARCHIVE_FALLBACK(sub_8209D988)
  ctx.r3.u64 = TocFor(ctx.r3.u32);
}

// Archive::GetSize: r3 an id; its unpacked size, or its stored size when it
// isn't compressed.
extern "C" REX_FUNC(sub_820A0860) {
  SBX_ARCHIVE_FALLBACK(sub_820A0860)
  const TocEntry& entry = EntryFor(ctx.r3.u32);
  ctx.r3.u64 = entry.unpacked_size != 0 ? uint32_t(entry.unpacked_size)
                                        : uint32_t(entry.stored_size);
}

// --- Loading ---

// Archive::Load: r3 the id, r4 the destination, r5 a callback (or 0), r6 its
// context, r7 an offset into a loose file (0 in play), r8 a size to read instead of
// the TOC's stored size (0 in play). Queues the load; returns its serial, or
// -1 when the queue is full.
extern "C" REX_FUNC(sub_8209DF80) {
  SBX_ARCHIVE_FALLBACK(sub_8209DF80)
  const uint32_t id = ctx.r3.u32;
  const uint32_t destination = ctx.r4.u32;
  const uint32_t callback = ctx.r5.u32;
  const uint32_t context = ctx.r6.u32;
  const uint32_t loose_offset = ctx.r7.u32;
  const uint32_t size_override = ctx.r8.u32;

  const TocEntry& entry = EntryFor(id);
  const bool compressed = entry.unpacked_size != 0;

  LoadRequest request;
  request.id = id;
  // The patch loader's files come first (its wrapper of FindLooseFile).
  request.loose_path = g.Call(sub_8209D9F8, id, 0u);  // Archive::FindLooseFile
  request.offset = uint32_t(entry.start_sector) * 2048;
  request.loose_offset = loose_offset;
  if (compressed) {
    request.size = entry.unpacked_size;
    request.stored_size = entry.stored_size;
  } else {
    request.size = size_override != 0 ? size_override : uint32_t(entry.stored_size);
    request.stored_size = 0;
  }
  request.destination = destination;
  request.context = context;
  request.callback = callback;

  uint32_t flags = kLoadFlags;
  if (compressed) {
    flags |= LoadRequest::kCompressed;
  }
  flags |= request.loose_path != 0 ? LoadRequest::kLooseFile : LoadRequest::kFromDat;
  if (callback != 0) {
    flags |= LoadRequest::kHasCallback;
  }
  if (loose_offset != 0) {
    flags |= LoadRequest::kLooseOffset;
  }
  request.flags = flags;

  ctx.r3.u64 = uint64_t(int64_t(QueueRequest(g, request)));
}

// Archive::Queue: r3 a request (Archive::Load's); its serial, or -1 when
// every slot is taken.
extern "C" REX_FUNC(sub_8209DD38) {
  SBX_ARCHIVE_FALLBACK(sub_8209DD38)
  ctx.r3.u64 = uint64_t(int64_t(QueueRequest(g, *At<LoadRequest>(ctx.r3.u32))));
}

// Archive::FindSlot: r3 a serial (-1 for a free slot); its slot, or 0.
extern "C" REX_FUNC(sub_8209D938) {
  SBX_ARCHIVE_FALLBACK(sub_8209D938)
  ctx.r3.u64 = AddressOf(FindSlot(int32_t(ctx.r3.u32)));
}

// Archive::Cancel: r3 a serial. Takes the request off the queue if the
// loader hasn't started it: 1, also when there's no such request; 0 when
// it's already being loaded.
extern "C" REX_FUNC(sub_8209DC40) {
  SBX_ARCHIVE_FALLBACK(sub_8209DC40)
  const int32_t serial = int32_t(ctx.r3.u32);
  bool cancelled = true;
  {
    QueueLock lock(g);
    if (RequestSlot* slot = FindSlot(serial)) {
      if (slot->state == RequestSlot::kWaiting) {
        Remove(Queue().requests, &slot->node);
        slot->serial = RequestSlot::kFree;
      } else {
        cancelled = false;
      }
    }
  }
  ctx.r3.u64 = cancelled ? 1 : 0;
}

// Archive::CancelQueued: r3 whether to wait. Drops every cancelable request
// the loader hasn't started; then, when asked, waits for the rest.
extern "C" REX_FUNC(sub_8209DCB0) {
  SBX_ARCHIVE_FALLBACK(sub_8209DCB0)
  const bool wait = ctx.r3.u32 != 0;
  {
    QueueLock lock(g);
    List& requests = Queue().requests;
    for (RequestSlot* slot = At<RequestSlot>(requests.head); slot != nullptr;
         slot = At<RequestSlot>(slot->node.next)) {
      if (slot->state == RequestSlot::kWaiting &&
          (slot->request.flags & LoadRequest::kCancelable) != 0) {
        Remove(requests, &slot->node);
        slot->serial = RequestSlot::kFree;
      }
    }
  }
  if (wait) {
    WaitIdle(g);
  }
}

// Archive::QueueLength: how many requests are queued or loading.
extern "C" REX_FUNC(sub_8209D8D0) {
  SBX_ARCHIVE_FALLBACK(sub_8209D8D0)
  uint32_t count;
  {
    QueueLock lock(g);
    count = Queue().requests.count;
  }
  ctx.r3.u64 = count;
}

// Archive::WaitIdle: returns once everything queued has been loaded.
extern "C" REX_FUNC(sub_8209D850) {
  SBX_ARCHIVE_FALLBACK(sub_8209D850)
  WaitIdle(g);
}

// --- Unpacking ---

// Ucl::Unpack: r3 the destination, r4 a UCL stream, r5 the size it should
// unpack to; 1 when it does. Stored blocks are copied with the game's memcpy,
// as the original does (the renderer watches it for copies into GPU memory).
extern "C" REX_FUNC(sub_82292050) {
  SBX_ARCHIVE_FALLBACK(sub_82292050)
  uint8_t* destination = At<uint8_t>(ctx.r3.u32);
  const uint8_t* stream = At<uint8_t>(ctx.r4.u32);
  const uint32_t expected = ctx.r5.u32;

  // The stream's length isn't passed: its block headers give it.
  constexpr size_t kLongestStream = 64 * 1024 * 1024;
  const size_t length = ucl::StreamLength(stream, kLongestStream);
  const bool ok =
      length != 0 &&
      ucl::Unpack({stream, length}, {destination, expected},
                  [&g](uint8_t* to, const uint8_t* from, size_t size) {
                    g.Call(sub_8258CB90, to, from, uint32_t(size));  // memcpy
                  });
  ctx.r3.u64 = ok ? 1 : 0;
}

// --- Set-up and shut-down ---

namespace {

// The globals Init and Shutdown use besides ArchiveState.
constexpr uint32_t kThreadRunner = 0x82694DE0;  // The object that starts the loader thread.
constexpr uint32_t kBuildCountCopy = 0x825F0A90;  // The xex's copies of the TOC headers.
constexpr uint32_t kCrsCountCopy = 0x825F0AA0;
constexpr uint32_t kBuildDatHandle = 0x825F0A84;
constexpr uint32_t kCrsDatHandle = 0x825F0A88;
constexpr uint32_t kLoaderFunctor = 0x825F0B30;
constexpr uint32_t kLoaderThread = 0x8209E250;  // The loader thread's body.
constexpr uint32_t kBuildDatPath = 0x82001DEC;  // "game:\DATA\BUILD\TXR\BUILD.DAT"
constexpr uint32_t kCrsDatPath = 0x82001DC8;    // "game:\DATA\BUILD\TXR\BUILD_CRS.DAT"

constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kMemCommitLargePages = 0x801000;
constexpr uint32_t kMemDecommit = 0x4000;
constexpr uint32_t kPageReadWrite = 4;
constexpr uint32_t kStagingSize = 0xA00000;  // Two 5 MB halves.
constexpr uint32_t kGenericRead = 0x80000000;
constexpr uint32_t kOpenExisting = 3;
constexpr uint32_t kOverlappedNoBuffering = 0x60000000;

// The functor the thread runner starts the loader thread with.
struct LoaderFunctor {
  uint8_t unknown_0[8];  // Its vtables (image data).
  u32_be unknown_8;
  u32_be unknown_12;
  uint8_t unknown_16;
  uint8_t unknown_17;
  uint8_t unknown_18;
  uint8_t unknown_19;
  u32_be unknown_20;
  uint8_t unknown_24[4];
  u32_be function;  // The thread's body.
};
GUEST_OFFSET(LoaderFunctor, function, 28);

// A TOC's size in bytes, from its file (the header and one entry per id):
// for the game's own files, the same as the xex's counts give. 0 when the
// file can't be read.
uint32_t TocFileSize(const char* name) {
  std::error_code error;
  const uint64_t size =
      std::filesystem::file_size(g_data_root / "DATA" / "BUILD" / "TXR" / name, error);
  return error || size < sizeof(TocHeader) || size > 0x10000000 ? 0 : uint32_t(size);
}

bool ReadTocFile(const char* name, uint32_t to, uint32_t size) {
  std::ifstream file(g_data_root / "DATA" / "BUILD" / "TXR" / name, std::ios::binary);
  file.read(reinterpret_cast<char*>(At<uint8_t>(to)), std::streamsize(size));
  return bool(file);
}

uint32_t VirtualAlloc(Guest& g, uint32_t size, uint32_t type) {
  return g.Call(sub_82292DD8, 0u, size, type, kPageReadWrite);
}

void VirtualFree(Guest& g, uint32_t address, uint32_t size) {
  g.Call(sub_82292D70, address, size, kMemDecommit);
}

// The .DAT files, opened for the game's own reads (its loader, when ours
// doesn't run, and Archive::Shutdown close them).
uint32_t OpenDat(Guest& g, uint32_t path) {
  const uint32_t handle = g.Call(sub_82292F48, path, kGenericRead, 1u, 0u, kOpenExisting,
                                 kOverlappedNoBuffering, 0u);  // CreateFile
  if (int32_t(handle) == -1) {
    g.Call(sub_8209AD48);  // the read error (the dirty-disc screen)
  }
  return handle;
}

// The shared load buffer, allocated once here at boot as the game does (64 KB
// pages, from the guest's virtual heap): --archive_shared_buffer_kb's size,
// never less than the game's (`game_size`, Init's r4). If the bigger size
// can't be had, the game's.
void AllocateSharedBuffer(Guest& g, uint32_t game_size) {
  ArchiveState& state = State();
  state.shared_buffer_size = 0;
  state.shared_buffer = 0;
  if (game_size == 0) {
    return;
  }
  const uint64_t wanted_kb = uint64_t(std::max(SBX_GET(archive_shared_buffer_kb), 0));
  const uint32_t size = uint32_t(std::clamp<uint64_t>(wanted_kb * 1024, game_size, 0x10000000));
  uint32_t buffer = VirtualAlloc(g, size, kMemCommitLargePages);
  uint32_t got = size;
  if (buffer == 0 && size != game_size) {
    REXLOG_ERROR("archive: no room for a {} KiB shared load buffer; the game's {} KiB",
                 size / 1024, game_size / 1024);
    buffer = VirtualAlloc(g, game_size, kMemCommitLargePages);
    got = game_size;
  }
  if (buffer == 0) {
    return;
  }
  state.shared_buffer = buffer;
  state.shared_buffer_size = got;
  if (got != game_size) {
    REXLOG_INFO("archive: shared load buffer: {} KiB at {:08X} (the game's is {} KiB)",
                got / 1024, buffer, game_size / 1024);
  }
}

// Archive::Init: r3 the thread runner, r4 the shared load buffer's size.
// The TOC buffers are sized from the .TOC files rather than the xex's
// counts (the same for the game's files), and the TOCs are read from them
// on the host. False when the TOC files can't be read (the game's Init
// runs instead).
bool Init(Guest& g, uint32_t runner, uint32_t shared_size) {
  const uint32_t build_size = TocFileSize("BUILD.TOC");
  const uint32_t crs_size = TocFileSize("BUILD_CRS.TOC");
  if (build_size == 0 || crs_size == 0) {
    REXLOG_ERROR("archive: the .TOC files can't be read; the game's Archive::Init runs");
    return false;
  }

  // The original's frame: its +116 is stored into the functor without ever
  // being written. Its caller is recompiled, so the word there is the one
  // the original would see.
  guest::StackFrame frame(g.ctx, 208);
  const uint32_t leftover = *At<u32_be>(frame.Local(116));

  ArchiveState& state = State();
  *At<u32_be>(kThreadRunner) = runner;
  state.build_toc_size = build_size;
  state.crs_toc_size = crs_size;
  if (build_size != (uint32_t(*At<u32_be>(kBuildCountCopy)) + 1) * 16 ||
      crs_size != (uint32_t(*At<u32_be>(kCrsCountCopy)) + 1) * 16) {
    REXLOG_INFO("archive: TOC sizes from the files: {} and {} bytes", build_size, crs_size);
  }

  LoaderFunctor& functor = *At<LoaderFunctor>(kLoaderFunctor);
  functor.unknown_8 = 0;
  functor.unknown_12 = 0;
  functor.unknown_16 = 0;
  functor.unknown_17 = 1;
  functor.unknown_18 = 0;
  functor.unknown_20 = leftover;
  functor.function = kLoaderThread;

  state.next_serial = 0;
  for (RequestSlot& slot : state.slots) {
    slot.serial = RequestSlot::kFree;
  }

  state.build_toc = VirtualAlloc(g, build_size, kMemCommit);
  state.crs_toc = VirtualAlloc(g, crs_size, kMemCommit);
  AllocateSharedBuffer(g, shared_size);

  state.staging_in_use = 0;
  *At<u32_be>(kCallbackSerial) = 0xFFFFFFFFu;
  state.staging_half_size = kStagingSize / 2;
  state.staging_count = 0;
  const uint32_t staging = VirtualAlloc(g, kStagingSize, kMemCommit);
  state.staging_halves[0] = staging;
  state.staging_halves[1] = staging + uint32_t(state.staging_half_size);
  state.idle_wanted = 0;

  // The semaphores start taken.
  Wait(g, Queue().read_done);
  Wait(g, Queue().work);
  Wait(g, Queue().idle);

  if (!ReadTocFile("BUILD.TOC", state.build_toc, build_size) ||
      !ReadTocFile("BUILD_CRS.TOC", state.crs_toc, crs_size)) {
    REXLOG_ERROR("archive: reading the .TOC files failed");
  }

  *At<u32_be>(kBuildDatHandle) = OpenDat(g, kBuildDatPath);
  *At<u32_be>(kCrsDatHandle) = OpenDat(g, kCrsDatPath);

  // Start the loader thread: the runner's second virtual (functor, 1).
  g.CallVirtual(At<uint8_t>(runner), 1, &functor, 1u);
  return true;
}

// Archive::Shutdown: frees the buffers and closes the .DAT files.
void Shutdown(Guest& g) {
  ArchiveState& state = State();
  if (state.shared_buffer != 0) {
    VirtualFree(g, state.shared_buffer, 0);
    state.shared_buffer = 0;
  }
  if (state.staging_half_size != 0) {
    VirtualFree(g, state.staging_halves[0], 0);
    state.staging_halves[0] = 0;
    state.staging_halves[1] = 0;
  }
  for (uint32_t handle : {kBuildDatHandle, kCrsDatHandle}) {
    if (g.Call(sub_82292860, uint32_t(*At<u32_be>(handle))) == 0) {  // CloseHandle
      g.Call(sub_8209AD48);  // the read error
    }
  }
  VirtualFree(g, state.build_toc, state.build_toc_size);
  VirtualFree(g, state.crs_toc, state.crs_toc_size);
}

}  // namespace

// Archive::Init, from the patch loader's wrapper (which applies the
// overrides to the TOCs after it): ours with --native_archive, else the
// recompiled function.
void hooks::NativeArchive_Init(PPCContext& ctx, uint8_t* base) {
  if (game::NativeArchive() && !g_data_root.empty()) {
    Guest g(ctx, base);
    if (Init(g, ctx.r3.u32, ctx.r4.u32)) {
      return;
    }
  }
  __imp__sub_8209E348(ctx, base);
}

// Archive::Shutdown: frees the TOCs, the load and staging buffers, and
// closes the .DAT files.
extern "C" REX_FUNC(sub_8209D4D0) {
  SBX_ARCHIVE_FALLBACK(sub_8209D4D0)
  Shutdown(g);
}

// --- The loader thread ---

// The loader thread's body (started by Archive::Init; never returns). With
// --native_archive ours, when both .DAT files open on the host (it starts
// the host reader thread); else the game's.
extern "C" REX_FUNC(sub_8209E250) {
  REX_FUNC_PROLOGUE();
  // Never destroyed: the reader thread waits on it until the process ends.
  static Loader& loader = *new Loader;
  if (!game::NativeArchive() || g_data_root.empty() || !loader.dats.Open()) {
    if (game::NativeArchive()) {
      REXLOG_ERROR("archive: the .DAT files didn't open; the game's loader runs instead");
    }
    __imp__sub_8209E250(ctx, base);
    return;
  }
  loader.reader = std::make_unique<DatReader>(loader.dats);
  Guest g(ctx, base);
  RunLoader(g, loader);
}
