// The save interface's objects: the save slots (one per device, 1,000 bytes
// each), the signed-in users, and the app's save devices and users, called
// through their vtables.

#pragma once

#include "game/guest_types.h"

namespace game {

using guest::i32_be;
using guest::u32_be;

// A save slot (one per player's controller, 1,000 bytes): its device, the
// saves enumerated on it, the request being worked on and the slot's state,
// which the save devices' update steps through by the handler for each
// state (SaveDevices::handlers).
struct SaveSlot {
  // The states (+988).
  enum State : uint32_t {
    kNoUser = 0,
    kNoDevice = 1,
    kFailed = 2,  // Until the failure is acknowledged.
    kReady = 3,
    kSelectingDevice = 4,
    kEnumerating = 5,
    kChecking = 6,
    kSaving = 7,
    kLoading = 8,
    kDeleting = 9,
  };
  // The requests (+960), by index.
  enum Request : uint32_t {
    kDeviceRequest = 0,
    kEnumerateRequest = 1,
    kSaveRequest = 2,
    kLoadRequest = 3,
    kDeleteRequest = 4,
    kFailureAcknowledged = 5,
  };
  // The failures (+992).
  enum Failure : uint32_t {
    kNoFailure = 0,
    kDeleteFailed = 5,
  };
  // What the steps' waits answer.
  static constexpr uint32_t kIoPending = 996;
  static constexpr uint32_t kIoIncomplete = 997;
  static constexpr uint32_t kDiskFull = 112;

  u32_be vtable;
  uint8_t unknown_4[32];
  u32_be device;  // 0 for none.
  guest::be<uint64_t> free_bytes;
  uint8_t unknown_48[32];
  u32_be saves;       // The address of its saves (308 bytes each).
  u32_be save_flags;  // The address of a word a save: 1 when it couldn't be opened.
  uint8_t unknown_88[8];
  u32_be enumerate_step;  // 1 enumerate, 2 wait.
  uint8_t open_overlapped[28];  // The open of the save being checked.
  u32_be check_step;  // 1 the flags set, 2 open, 3 wait for the open, 4 wait for the close.
  u32_be check_item;
  uint8_t content_overlapped[28];  // The content's close or delete.
  uint8_t unknown_164[308];
  uint8_t file_overlapped[20];
  u32_be file_event;   // 0 for none.
  i32_be file_handle;  // -1 for none.
  u32_be file_bytes;
  guest::u16_be display_name[128];  // UTF-16.
  char file_name[42];
  char path[130];
  u32_be data;
  u32_be data_size;
  u32_be thumbnail;
  u32_be thumbnail_size;
  u32_be operation_step;  // The save's, load's or delete's sub-step.
  u32_be delete_failed;
  i32_be user;  // The controller signed in, 0 to 3.
  u32_be requests[6];  // Pending requests, by Request.
  u32_be save_count;
  u32_be state;
  u32_be failure;
  uint8_t unknown_996[4];
};
GUEST_SIZE(SaveSlot, 1000);
GUEST_OFFSET(SaveSlot, device, 36);
GUEST_OFFSET(SaveSlot, free_bytes, 40);
GUEST_OFFSET(SaveSlot, saves, 80);
GUEST_OFFSET(SaveSlot, save_flags, 84);
GUEST_OFFSET(SaveSlot, enumerate_step, 96);
GUEST_OFFSET(SaveSlot, open_overlapped, 100);
GUEST_OFFSET(SaveSlot, check_step, 128);
GUEST_OFFSET(SaveSlot, content_overlapped, 136);
GUEST_OFFSET(SaveSlot, file_overlapped, 472);
GUEST_OFFSET(SaveSlot, file_event, 492);
GUEST_OFFSET(SaveSlot, file_handle, 496);
GUEST_OFFSET(SaveSlot, file_bytes, 500);
GUEST_OFFSET(SaveSlot, display_name, 504);
GUEST_OFFSET(SaveSlot, file_name, 760);
GUEST_OFFSET(SaveSlot, path, 802);
GUEST_OFFSET(SaveSlot, data, 932);
GUEST_OFFSET(SaveSlot, operation_step, 948);
GUEST_OFFSET(SaveSlot, user, 956);
GUEST_OFFSET(SaveSlot, requests, 960);
GUEST_OFFSET(SaveSlot, save_count, 984);
GUEST_OFFSET(SaveSlot, state, 988);
GUEST_OFFSET(SaveSlot, failure, 992);

struct UserRecord {
  uint8_t unknown_0[8];
  u32_be signed_in;
  uint8_t unknown_12[28];
};
GUEST_SIZE(UserRecord, 40);

// The signed-in users: a record per controller, each user's controller.
struct Users {
  UserRecord records[5];
  i32_be controllers[4];
  u32_be count;
};
GUEST_OFFSET(Users, controllers, 200);
GUEST_OFFSET(Users, count, 216);

// The save devices (116 bytes, the app's service): its slots, the
// storage devices' listener and the handler for each slot state. Its
// vtable's slots are the save interface's calls (the k...Slot below).
struct SaveDevices {
  static constexpr uint32_t kVtable = 0x82002158;
  static constexpr uint32_t kBaseVtable = 0x82002108;  // The service's base.
  static constexpr uint32_t kRequestDeviceSlot = 1;
  static constexpr uint32_t kDeviceSlot = 2;
  static constexpr uint32_t kFreeBytesSlot = 3;
  static constexpr uint32_t kSaveCountSlot = 4;
  static constexpr uint32_t kSaveAtSlot = 5;
  static constexpr uint32_t kSaveFlagSlot = 6;
  static constexpr uint32_t kStateSlot = 7;
  static constexpr uint32_t kPendingSlot = 8;
  static constexpr uint32_t kFailureSlot = 11;
  static constexpr uint32_t kAcknowledgeFailureSlot = 12;
  static constexpr uint32_t kSetRootNameSlot = 14;
  static constexpr uint32_t kRequestSaveSlot = 15;
  static constexpr uint32_t kRequestLoadSlot = 16;
  static constexpr uint32_t kRequestDeleteSlot = 17;

  u32_be vtable;
  u32_be tick_key;      // Its update's key in the run loop's tick handlers.
  u32_be listener_key;  // Its event handler's key in the run loop's listeners.
  u32_be unknown_12;
  char root_name[28];
  u32_be slots;  // The address of the SaveSlot array (a count header 8 bytes before).
  i32_be slot_count;
  u32_be selector_flags;
  u32_be listener;  // The storage devices' notification listener, 0 for none.
  u32_be unknown_60;
  u32_be unknown_64;
  u32_be enumerate_count;  // How many saves a slot enumerates.
  u32_be owner;  // The slot checking or deleting saves, 0 for none.
  u32_be handlers[10];  // By SaveSlot::State.

  SaveSlot* Slot(uint32_t index) { return guest::Element<SaveSlot>(slots, index); }
};
GUEST_OFFSET(SaveDevices, root_name, 16);
GUEST_OFFSET(SaveDevices, slots, 44);
GUEST_OFFSET(SaveDevices, listener, 56);
GUEST_OFFSET(SaveDevices, owner, 72);
GUEST_OFFSET(SaveDevices, handlers, 76);
GUEST_SIZE(SaveDevices, 116);

// What the save devices are made with (SaveDevices::SaveDevices' r4): the
// run loop, the framework's interface and the two keys.
struct SaveDevicesSetUp {
  u32_be run_loop;
  u32_be framework;
  u32_be tick_key;
  u32_be listener_key;
};
GUEST_SIZE(SaveDevicesSetUp, 16);

// A member delegate (12 bytes): calls `method` on `object`. The run loop
// keeps a copy (its slot 2, Clone).
struct MemberDelegate {
  static constexpr uint32_t kVtable = 0x820028DC;
  static constexpr uint32_t kBaseVtable = 0x82000AC0;  // The functors' base.

  u32_be vtable;
  u32_be object;
  u32_be method;
};
GUEST_SIZE(MemberDelegate, 12);

// A slot array's header, 8 bytes before its first slot (operator new[]'s).
struct SlotArrayHeader {
  u32_be count;
  uint8_t unknown_4[4];
};
GUEST_SIZE(SlotArrayHeader, 8);

// A run loop event (the save devices' event handler's): its kind first.
struct RunLoopEvent {
  static constexpr uint32_t kShutDown = 1;  // Probably.

  u32_be kind;
};

// The run loop: its vtable's slots 1 and 2 add a listener and a tick
// handler (a delegate, copied), slots 3 and 4 remove them, each by key.
struct RunLoop {
  static constexpr uint32_t kAddListenerSlot = 1;
  static constexpr uint32_t kAddTickHandlerSlot = 2;
  static constexpr uint32_t kRemoveListenerSlot = 3;
  static constexpr uint32_t kRemoveTickHandlerSlot = 4;

  u32_be vtable;
};

// The app ([0x826AD970]): its vtable's slot 3 the save devices, slot 2 the
// users; +8 the object those come from.
struct App {
  static constexpr uint32_t kSaveDevicesSlot = 3;
  static constexpr uint32_t kUsersSlot = 2;

  u32_be vtable;
  uint8_t unknown_4[4];
  u32_be services;
};
GUEST_OFFSET(App, services, 8);

// The local time (KeQuerySystemTime's fields, sub_82294300): year, month,
// day of week, day, hour, minute...
struct LocalTime {
  guest::u16_be year;
  guest::u16_be month;
  guest::u16_be day_of_week;
  guest::u16_be day;
  guest::u16_be hour;
  guest::u16_be minute;
  guest::u16_be second;
  guest::u16_be millisecond;
};
GUEST_SIZE(LocalTime, 16);

// An enumerated save (XCONTENT_DATA, 308 bytes).
struct ContentData {
  uint8_t unknown_0[8];
  guest::u16_be display_name[128];
  char file_name[42];
  uint8_t unknown_306[2];
};
GUEST_OFFSET(ContentData, file_name, 264);
GUEST_SIZE(ContentData, 308);

// A saved replay's record (36 bytes): when it was saved (as a SYSTEMTIME's
// first fields), its time, its mode and its number.
struct ReplayRecord {
  guest::u16_be year;
  guest::u16_be month;
  guest::u16_be unknown_4;
  guest::u16_be day;
  guest::u16_be hour;
  guest::u16_be minute;
  uint8_t unknown_12[4];
  u32_be time;  // Minutes (bits 16-30), seconds (10-15) and thousandths (0-9).
  u32_be mode;
  u32_be unknown_24;
  i32_be number;
  u32_be unknown_32;
};
GUEST_OFFSET(ReplayRecord, time, 16);
GUEST_OFFSET(ReplayRecord, number, 28);
GUEST_SIZE(ReplayRecord, 36);

// A saved replay's entry in the list the replay screens show (304 bytes).
struct ReplayEntry {
  char file_name[42];
  guest::u16_be display_name[128];
  uint8_t unknown_298[2];
  i32_be number;
};
GUEST_OFFSET(ReplayEntry, display_name, 42);
GUEST_OFFSET(ReplayEntry, number, 300);
GUEST_SIZE(ReplayEntry, 304);

// The save buffers ([0x8280F1F8], 24 bytes, all from VirtualAlloc): the
// game data's and a replay's buffers, the replay record being saved, and
// the saved replays' records and entries.
struct SaveBuffers {
  static constexpr uint32_t kGameDataBytes = 129200;  // A save (SaveData).
  static constexpr uint32_t kReplayBytes = 1334880;

  u32_be game_data;
  u32_be replay;
  u32_be record;   // A ReplayRecord.
  u32_be records;  // The ReplayRecords.
  u32_be entries;  // The ReplayEntries.
  i32_be count;

  ReplayRecord* Record(uint32_t index) { return guest::Element<ReplayRecord>(records, index); }
  ReplayEntry* Entry(uint32_t index) { return guest::Element<ReplayEntry>(entries, index); }
};
GUEST_SIZE(SaveBuffers, 24);

// save_interface.cpp's C++, for the options screen's load and save pages
// (options_load_save.cpp). `player` is a player (0 to 3); their save slot is
// their controller's.

// SaveSlots::Device (sub_8228CE78): the player's slot's device (0 for
// none).
uint32_t PlayerSaveDevice(guest::Guest& g, uint32_t player);

// GameData::Find (probably; sub_8228D138): the game data's save among the
// player's slot's saves: 7 found, 8 found but damaged, 3 not there or no
// device, 4 busy.
uint32_t FindGameData(guest::Guest& g, uint32_t player);

// SaveBuffers::Destroy (sub_8228CCE0): the save buffers freed.
void DestroySaveBuffers(guest::Guest& g);

}  // namespace game
