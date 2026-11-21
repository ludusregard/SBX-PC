// The save interface as our own C++ (--native_save_interface, on by
// default): the save interface's calls into the app's save devices and
// users (its set-up, the slots' queries, the save, load and delete
// requests), the save devices' getters and requests, the handlers the save
// devices' update runs for each slot state (waiting for a user or a device,
// ready, a failure, the enumeration, the saves' check, the delete) and
// their waits, the devices' teardown and the slots' buffers, the signed-in
// users, a slot by its controller, the play time's update, the devices'
// root name, and SaveGame::Write (for autosave).
//
// The content and file calls themselves (enumerate, open, read, write,
// delete; the XContent and overlapped imports), the save and load steps and
// the save devices' constructor stay recompiled (the constructor hands the
// run loop delegates built in its own frame).

#include "base/settings.h"
#include "game/save_data.h"
#include "game/save_interface.h"
#include "hooks/native_calls.h"

#include <cstring>

SBX_SETTING_BOOL(native_save_interface, true, "Overrides", "Data and Saves",
                 "Our own C++ for the save interface (its calls into the save devices, the "
                 "slots' getters, requests and state handlers, the users, a slot by its "
                 "controller, the play time) instead of the recompiled functions; the "
                 "content and file calls stay recompiled");

namespace {

using game::App;
using game::ContentData;
using game::LocalTime;
using game::PlayTime;
using game::ReplayEntry;
using game::ReplayRecord;
using game::RunLoop;
using game::RunLoopEvent;
using game::SaveBuffers;
using game::SaveDevices;
using game::SaveSlot;
using game::SaveStats;
using game::SlotArrayHeader;
using game::Users;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_save_interface); }

constexpr uint32_t kApp = 0x826AD970;           // [it] the app (writable)
// [it + 4 x player] each player's controller (writable); player 0's saves.
constexpr uint32_t kSaveController = 0x826AA884;
constexpr uint32_t kSaveNameFormat = 0x82011BAC;  // Read-only strings.
constexpr uint32_t kSaveNameText = 0x82011BDC;
constexpr uint32_t kSavePath = 0x82011B98;          // "TXR360:\TXR_DATA_1" (read-only)
constexpr uint32_t kGameDataFileName = 0x82628040;  // The game data's save (writable)
constexpr uint32_t kSaveBytes = 129200;

App* TheApp() { return At<App>(*At<u32_be>(kApp)); }

// The app's save devices (vtable slot 3) or users (slot 2).
uint32_t AppPart(Guest& g, uint32_t slot) { return g.CallVirtual(TheApp(), slot); }

uint32_t UserCount(Guest& g) {
  const uint32_t users = AppPart(g, App::kUsersSlot);
  return g.CallVirtual(At<u32_be>(users), 1);
}

uint32_t UserAt(Guest& g, uint32_t i) {
  const uint32_t users = AppPart(g, App::kUsersSlot);
  return g.CallVirtual(At<u32_be>(users), 2, i);
}

SaveSlot* Slot(PPCContext& ctx) { return At<SaveDevices>(ctx.r3.u32)->Slot(ctx.r4.u32); }

// The app's save devices.
SaveDevices* Devices(Guest& g) { return At<SaveDevices>(AppPart(g, App::kSaveDevicesSlot)); }

constexpr uint32_t kRootName = 0x82001EAC;  // "TXR360" (read-only)
constexpr uint32_t kSaveRequestFrame = 160;
// A save request's stack argument (the 9th), at the caller's r1 + 84.
struct SaveRequestStack {
  uint8_t linkage[84];
  u32_be thumbnail_size;
};
GUEST_OFFSET(SaveRequestStack, thumbnail_size, 84);
constexpr uint32_t kSlotArrayDeleteFlags = 3;  // The slots' vector deleting destructor's.

// SaveInterface::SlotState (sub_820A0200): the slot's state; anything past
// the ten states answers 0.
uint32_t SlotState(Guest& g, uint32_t slot) {
  const uint32_t state = g.CallVirtual(Devices(g), SaveDevices::kStateSlot, slot);
  return state <= SaveSlot::kDeleting ? state : 0;
}

// SaveInterface::Save (sub_820A0678): the save devices' RequestSave for
// `slot`: the names, the path, the data and its size, a thumbnail and its
// size (that one passed on the stack, at +84 of the original's 160-byte
// frame, where the devices' slot reads it). Returns its answer.
uint32_t RequestSave(Guest& g, uint32_t slot, const guest::u16_be* display_name,
                     const char* file_name, const char* path, uint32_t data, uint32_t size,
                     uint32_t thumbnail, uint32_t thumbnail_size) {
  guest::StackFrame frame(g.ctx, kSaveRequestFrame);
  SaveDevices* devices = Devices(g);
  At<SaveRequestStack>(frame.Local(0))->thumbnail_size = thumbnail_size;
  return g.CallVirtual(devices, SaveDevices::kRequestSaveSlot, slot, display_name, file_name,
                       path, data, size, thumbnail);
}

// What a save, load or delete request copies in: the names, the data and
// the thumbnail.
struct SaveRequest {
  uint32_t display_name;  // UTF-16
  const char* file_name;
  const char* path;
  uint32_t data;
  uint32_t data_size;
  uint32_t thumbnail;
  uint32_t thumbnail_size;
};

// SaveSlot::RequestSave, RequestLoad and RequestDelete (sub_820A5500,
// sub_820A5608, sub_820A5708): unless the slot is ready, 0. Else the
// request's names copied into the slot (the display name by sub_82293790, a
// wide string copy; the path not for a delete), its data and thumbnail
// set (not for a delete) and its request raised; 1.
uint32_t RequestSlot(Guest& g, SaveDevices* devices, uint32_t index, const SaveRequest& request,
                     SaveSlot::Request kind) {
  if (g.CallVirtual(devices, SaveDevices::kStateSlot, index) != SaveSlot::kReady) {
    return 0;
  }
  SaveSlot* slot = devices->Slot(index);
  g.Call(sub_82293790, slot->display_name, request.display_name);
  std::strcpy(slot->file_name, request.file_name);
  if (kind != SaveSlot::kDeleteRequest) {
    std::strcpy(slot->path, request.path);
    slot->data = request.data;
    slot->data_size = request.data_size;
    slot->thumbnail = request.thumbnail;
    slot->thumbnail_size = request.thumbnail_size;
  }
  slot->requests[kind] = 1;
  return 1;
}

// The waits on a content call's overlapped (XGetOverlappedResult,
// sub_822936E0, not waiting): 1 when it's done, 0 while it's pending, else
// `error`.
int32_t WaitContent(Guest& g, uint8_t* overlapped, int32_t error) {
  const uint32_t result = g.Call(sub_822936E0, overlapped, 0u, 0u);
  if (result == 0) {
    return 1;
  }
  return result == SaveSlot::kIoPending ? 0 : error;
}

// The waits on the save file's read or write (GetOverlappedResult,
// sub_82293968, not waiting, then GetLastError, sub_82293960): 1 when it's
// done; 0 while it's pending; else `disk_full_error` for a full disk or
// `error`.
int32_t WaitFile(Guest& g, SaveSlot* slot, int32_t error, int32_t disk_full_error) {
  const uint32_t done = g.Call(sub_82293968, uint32_t(slot->file_handle), slot->file_overlapped,
                               &slot->file_bytes, 0u);
  const uint32_t last_error = g.Call(sub_82293960);
  if (done != 0) {
    return 1;
  }
  if (last_error == SaveSlot::kIoPending || last_error == SaveSlot::kIoIncomplete) {
    return 0;
  }
  return last_error == SaveSlot::kDiskFull ? disk_full_error : error;
}

// SaveSlot::WaitClose (sub_820A5290): the content's close waited on; once
// it's done or failed, the file's handle and event closed (CloseHandle,
// sub_82292860, where they're set). 1 done, 0 pending, -8 failed.
int32_t WaitClose(Guest& g, SaveSlot* slot) {
  const uint32_t result = g.Call(sub_822936E0, slot->content_overlapped, 0u, 0u);
  if (result == SaveSlot::kIoPending) {
    return 0;
  }
  if (slot->file_handle != -1) {
    g.Call(sub_82292860, uint32_t(slot->file_handle));
  }
  if (slot->file_event != 0) {
    g.Call(sub_82292860, slot->file_event);
  }
  return result == 0 ? 1 : -8;
}

// The handlers the save devices' update runs for a slot by its state; each
// answers the slot's next state.

// SaveSlot::WaitForUser (state 0): ready once the slot has a user.
uint32_t WaitForUser(const SaveSlot* slot) {
  return slot->user >= 0 && slot->user <= 3 ? SaveSlot::kReady : SaveSlot::kNoUser;
}

// SaveSlot::ShowDeviceSelector (sub_820A4B80): 1 once it's shown.
bool ShowDeviceSelector(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  return int32_t(g.Call(sub_820A4B80, devices, slot)) == 1;
}

// SaveSlot::WaitForDevice (state 1): with the device request, the device
// selector; else ready once the slot has a device.
uint32_t WaitForDevice(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  if (slot->requests[SaveSlot::kDeviceRequest] != 0) {
    return ShowDeviceSelector(g, devices, slot) ? SaveSlot::kSelectingDevice : SaveSlot::kNoDevice;
  }
  return slot->device != 0 ? SaveSlot::kReady : SaveSlot::kNoDevice;
}

// SaveSlot::Failed (state 2): until the failure is acknowledged; then it
// and the failure are cleared.
uint32_t Failed(SaveSlot* slot) {
  if (slot->requests[SaveSlot::kFailureAcknowledged] == 0) {
    return SaveSlot::kFailed;
  }
  slot->requests[SaveSlot::kFailureAcknowledged] = 0;
  slot->failure = SaveSlot::kNoFailure;
  return SaveSlot::kReady;
}

// SaveSlot::Ready (state 3): without a device, back to waiting for one.
// Else the first request raised starts its work: the device selector, the
// enumeration, the save, the load or the delete (each one's first
// sub-step).
uint32_t Ready(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  if (slot->device == 0) {
    return SaveSlot::kNoDevice;
  }
  u32_be* requests = slot->requests;
  if (requests[SaveSlot::kDeviceRequest] != 0) {
    return ShowDeviceSelector(g, devices, slot) ? SaveSlot::kSelectingDevice : SaveSlot::kReady;
  }
  if (requests[SaveSlot::kEnumerateRequest] != 0) {
    slot->enumerate_step = 1;
    requests[SaveSlot::kEnumerateRequest] = 0;
    return SaveSlot::kEnumerating;
  }
  if (requests[SaveSlot::kSaveRequest] != 0) {
    slot->operation_step = 1;
    requests[SaveSlot::kSaveRequest] = 0;
    return SaveSlot::kSaving;
  }
  if (requests[SaveSlot::kLoadRequest] != 0) {
    slot->operation_step = 1;
    requests[SaveSlot::kLoadRequest] = 0;
    return SaveSlot::kLoading;
  }
  if (requests[SaveSlot::kDeleteRequest] != 0) {
    slot->operation_step = 9;
    requests[SaveSlot::kDeleteRequest] = 0;
    return SaveSlot::kDeleting;
  }
  return SaveSlot::kReady;
}

// SaveSlot::EnumerateStep (state 5): the enumeration (SaveSlot::Enumerate,
// sub_820A4BF0), then its wait (SaveSlot::WaitEnumerate, sub_820A4CA8); once
// done, the saves' check. No device or a failed wait: back to ready; a
// failed enumeration counts no saves.
uint32_t EnumerateStep(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  if (slot->enumerate_step == 1) {
    const int32_t result = int32_t(g.Call(sub_820A4BF0, devices, slot));
    if (result == 1) {
      slot->enumerate_step = 2;
    } else if (result == -2) {
      slot->enumerate_step = 0;
      return SaveSlot::kReady;
    } else if (result == -4) {
      slot->save_count = 0;
      slot->enumerate_step = 2;
    }
  } else if (slot->enumerate_step == 2) {
    const int32_t result = int32_t(g.Call(sub_820A4CA8, devices, slot));
    if (result == 1) {
      slot->enumerate_step = 0;
      slot->check_step = 1;
      return SaveSlot::kChecking;
    }
    if (result == -6) {
      slot->enumerate_step = 0;
      return SaveSlot::kReady;
    }
  }
  return SaveSlot::kEnumerating;
}

// Claims the devices for `slot` when no slot has them. True when the slot
// has them.
bool ClaimDevices(SaveDevices* devices, SaveSlot* slot) {
  if (devices->owner == 0) {
    devices->owner = guest::AddressOf(slot);
  }
  return devices->owner == guest::AddressOf(slot);
}

u32_be& SaveFlag(SaveSlot* slot, uint32_t item) {
  return *guest::Element<u32_be>(slot->save_flags, item);
}

// SaveSlot::CheckStep (state 6): each enumerated save opened and closed
// again, its flag cleared when it opened (the flags all set first), while
// the slot holds the devices; done, back to ready.
uint32_t CheckStep(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  if (!ClaimDevices(devices, slot)) {
    return SaveSlot::kChecking;
  }
  switch (uint32_t(slot->check_step)) {
    case 1:
      for (uint32_t item = 0; item < slot->save_count; item++) {
        SaveFlag(slot, item) = 1;
      }
      slot->check_item = 0;
      slot->check_step = 2;
      break;
    case 2: {
      if (slot->check_item < slot->save_count) {
        const int32_t result =
            int32_t(g.Call(sub_820A4D38, devices, slot, slot->check_item));  // SaveSlot::OpenItem
        if (result == 1) {
          slot->check_step = 3;
          break;
        }
        if (result != -2) {
          slot->check_item += 1;
          break;
        }
      }
      slot->check_step = 0;
      devices->owner = 0;
      return SaveSlot::kReady;
    }
    case 3: {
      // SaveSlot::WaitOpenItem
      const int32_t result = int32_t(g.Call(sub_820A4DB8, devices, slot));
      if (result == 1) {
        SaveFlag(slot, slot->check_item) = 0;
        slot->check_step = 4;
      } else if (result == -7) {
        slot->check_step = 2;
        slot->check_item += 1;
      }
      break;
    }
    case 4:
      if (WaitContent(g, slot->open_overlapped, -8) != 0) {  // SaveSlot::WaitCloseItem
        slot->check_step = 2;
        slot->check_item += 1;
      }
      break;
  }
  return SaveSlot::kChecking;
}

// SaveSlot::DeleteStep (state 9): the delete (SaveSlot::Delete,
// sub_820A5400), then its wait, while the slot holds the devices; done,
// back to ready, or failed (the failure kDeleteFailed).
uint32_t DeleteStep(Guest& g, SaveDevices* devices, SaveSlot* slot) {
  if (!ClaimDevices(devices, slot)) {
    return SaveSlot::kDeleting;
  }
  if (slot->operation_step == 9) {
    const int32_t result = int32_t(g.Call(sub_820A5400, devices, slot));
    slot->delete_failed = 0;
    if (result == 1) {
      slot->operation_step = 10;
      return SaveSlot::kDeleting;
    }
    if (result == -2) {
      slot->operation_step = 0;
      devices->owner = 0;
      return SaveSlot::kReady;
    }
    if (result == -12) {
      slot->operation_step = 0;
      slot->failure = SaveSlot::kDeleteFailed;
      devices->owner = 0;
      return SaveSlot::kFailed;
    }
    return SaveSlot::kDeleting;
  }
  if (slot->operation_step == 10) {
    const int32_t result = WaitContent(g, slot->content_overlapped, -12);  // SaveSlot::WaitDelete
    if (result == 0) {
      return SaveSlot::kDeleting;
    }
    if (result == -12 && slot->delete_failed == 0) {
      slot->delete_failed = 1;
    }
    slot->operation_step = 0;
    devices->owner = 0;
    if (slot->delete_failed == 0) {
      return SaveSlot::kReady;
    }
    slot->failure = SaveSlot::kDeleteFailed;
    return SaveSlot::kFailed;
  }
  return SaveSlot::kDeleting;
}

// The slot array's buffers (SaveSlot::AllocBuffers, sub_820A4788): its
// saves' (`count` of 308 bytes) and their flags (a word each), each from
// operator new[] (sub_8258C0C0; -1 bytes asked for when the size would
// overflow).
void AllocBuffers(Guest& g, SaveSlot* slot, uint32_t count) {
  constexpr uint32_t kSaveBytes = 308;
  constexpr uint32_t kTooMany = 0xFFFFFFFFu;
  const uint32_t save_bytes = count <= kTooMany / kSaveBytes ? count * kSaveBytes : kTooMany;
  slot->saves = g.Call(sub_8258C0C0, save_bytes);
  const uint32_t flag_bytes = count <= kTooMany / 4 ? count * 4 : kTooMany;
  slot->save_flags = g.Call(sub_8258C0C0, flag_bytes);
}

// SlotArray::Reset (sub_820A5810): the slots freed, by their vector
// deleting destructor (the first slot's vtable slot 0) when the header
// counts any, else operator delete[] (sub_8258C940) on the header; the
// word cleared.
void ResetSlotArray(Guest& g, u32_be& slots) {
  if (slots == 0) {
    return;
  }
  SlotArrayHeader* header = At<SlotArrayHeader>(slots - uint32_t(sizeof(SlotArrayHeader)));
  if (header->count != 0) {
    g.CallVirtual(At<SaveSlot>(slots), 0, kSlotArrayDeleteFlags);
  } else {
    g.Call(sub_8258C940, header);  // operator delete[]
  }
  slots = 0;
}

// SaveDevices::Destroy (sub_820A5908): the storage devices' listener
// closed (CloseHandle, sub_82292860) and the slots freed; the vtable
// stepped down to the service's base. The run loop's handlers aren't
// removed.
void DestroyDevices(Guest& g, SaveDevices* devices) {
  devices->vtable = SaveDevices::kVtable;
  if (devices->listener != 0) {
    g.Call(sub_82292860, devices->listener);
  }
  ResetSlotArray(g, devices->slots);
  devices->vtable = SaveDevices::kBaseVtable;
}

// SaveDevices::SaveDevices' frame (176 bytes): the two member delegates it
// hands the run loop (which copies them).
struct SaveDevicesFrame {
  uint8_t linkage[80];
  game::MemberDelegate update;
  uint8_t unknown_92[4];
  game::MemberDelegate on_event;
  uint8_t unknown_108[68];
};
GUEST_OFFSET(SaveDevicesFrame, update, 80);
GUEST_OFFSET(SaveDevicesFrame, on_event, 96);
GUEST_SIZE(SaveDevicesFrame, 176);

constexpr uint32_t kSlotVtable = 0x82002150;
constexpr uint32_t kSlotCount = 4;
constexpr uint32_t kSelectorFlags = 512;
constexpr uint32_t kDevicesRootName = 0x820021A0;  // Read-only; SetRootName replaces it.
constexpr uint32_t kUpdate = 0x820A4838;            // SaveDevices::Update
constexpr uint32_t kOnEvent = 0x820A4A28;           // SaveDevices::OnEvent
// The slot state handlers, by SaveSlot::State.
constexpr uint32_t kSlotHandlers[10] = {0x820A4A98, 0x820A5AA0, 0x820A4AB8, 0x820A5B00,
                                        0x820A4AE0, 0x820A5BE8, 0x820A5CB0, 0x820A5E10,
                                        0x820A6090, 0x820A6238};

// SaveDevices::SaveDevices (sub_820A6388): r3 the devices, r4 what they're
// made with, r5 how many saves a slot enumerates. Its update and event
// handler added to the run loop (member delegates built in its frame, which
// the run loop copies), the storage devices' listener made
// (XNotifyCreateListener, sub_82292CF8), the four slots (operator new[],
// sub_8258C0C0, with a count header) set up and given their buffers, the
// state handlers set and the root name's default copied in.
void ConstructDevices(Guest& g, SaveDevices* devices, const game::SaveDevicesSetUp* set_up,
                      uint32_t enumerate_count) {
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(SaveDevicesFrame)));
  SaveDevicesFrame* frame = At<SaveDevicesFrame>(stack.Local(0));
  devices->vtable = SaveDevices::kVtable;
  frame->update = {game::MemberDelegate::kVtable, guest::AddressOf(devices), kUpdate};
  devices->unknown_12 = set_up->framework;
  devices->tick_key = set_up->tick_key;
  devices->listener_key = set_up->listener_key;
  g.CallVirtual(At<RunLoop>(set_up->run_loop), RunLoop::kAddTickHandlerSlot, &devices->tick_key,
                &frame->update);
  frame->update.vtable = game::MemberDelegate::kBaseVtable;
  frame->on_event = {game::MemberDelegate::kVtable, guest::AddressOf(devices), kOnEvent};
  g.CallVirtual(At<RunLoop>(set_up->run_loop), RunLoop::kAddListenerSlot, &devices->listener_key,
                &frame->on_event);
  frame->on_event.vtable = game::MemberDelegate::kBaseVtable;

  devices->listener = g.Call(sub_82292CF8, 1u);  // XNotifyCreateListener (storage devices)
  devices->selector_flags = kSelectorFlags;
  devices->slot_count = int32_t(kSlotCount);
  const uint32_t array = g.Call(
      sub_8258C0C0, uint32_t(sizeof(SlotArrayHeader) + sizeof(SaveSlot) * kSlotCount));
  uint32_t slots = 0;
  if (array != 0) {
    At<SlotArrayHeader>(array)->count = kSlotCount;
    slots = array + uint32_t(sizeof(SlotArrayHeader));
    for (uint32_t i = 0; i < kSlotCount; i++) {
      SaveSlot* slot = guest::Element<SaveSlot>(slots, i);
      slot->vtable = kSlotVtable;
      slot->device = 0;
      slot->free_bytes = 0;
      for (u32_be& request : slot->requests) {
        request = 0;
      }
      slot->save_count = 0;
      slot->state = SaveSlot::kReady;
      slot->enumerate_step = 0;
      slot->operation_step = 0;
      slot->failure = SaveSlot::kNoFailure;
    }
  }
  devices->slots = slots;
  devices->enumerate_count = enumerate_count;
  // With no slots (the allocation failed) the original gives buffers to
  // slots at low addresses; here none.
  if (devices->slots != 0) {
    for (int32_t i = 0; i < devices->slot_count; i++) {
      AllocBuffers(g, devices->Slot(uint32_t(i)), devices->enumerate_count);
    }
  }
  devices->owner = 0;
  for (uint32_t state = 0; state < 10; state++) {
    devices->handlers[state] = kSlotHandlers[state];
  }
  devices->unknown_60 = 1024;
  devices->unknown_64 = 1;
  std::strcpy(devices->root_name, At<char>(kDevicesRootName));
}

// --- The save layer: the players' slots, the save buffers, the replays ---

constexpr uint32_t kSaveBuffers = 0x8280F1F8;  // [it] the save buffers (writable)
constexpr uint32_t kPlayClock = 0x826B5870;    // Frames played (writable)
constexpr uint32_t kMemCommit = 0x1000;
constexpr uint32_t kMemRelease = 0x8000;
constexpr uint32_t kPageReadWrite = 4;
constexpr int32_t kMostReplays = 99;  // With fewer, the list can start with an empty entry.

// What the save layer's checks answer.
enum SaveAnswer : uint32_t {
  kRoom = 0,
  kNoRoom = 2,
  kNoDevice = 3,
  kBusy = 4,
  kNotReplay = 5,
  kReplayRead = 6,
  kDone = 7,  // Also: the game data found.
  kFoundDamaged = 8,
};

SaveBuffers* Buffers() { return At<SaveBuffers>(*At<u32_be>(kSaveBuffers)); }

// SaveSlots::OfController (sub_8228CB68): the first user who is the
// controller, or the user count (also when that's 0 or below).
int32_t SlotOfController(Guest& g, uint32_t controller) {
  const int32_t count = int32_t(UserCount(g));
  int32_t i = 0;
  while (i < count && UserAt(g, uint32_t(i)) != controller) {
    i++;
  }
  return count <= 0 ? count : i;
}

// A player's save slot: their controller's.
uint32_t PlayerSlot(Guest& g, uint32_t player) {
  return uint32_t(SlotOfController(g, *guest::Element<u32_be>(kSaveController, player)));
}

// The save interface's calls into the save devices (SaveInterface::...).
template <typename... Args>
uint32_t AskDevices(Guest& g, uint32_t vtable_slot, Args... args) {
  return g.CallVirtual(Devices(g), vtable_slot, args...);
}

bool Pending(Guest& g, uint32_t slot) {
  return AskDevices(g, SaveDevices::kPendingSlot, slot) != 0;
}
uint32_t SaveCount(Guest& g, uint32_t slot) {
  return AskDevices(g, SaveDevices::kSaveCountSlot, slot);
}
ContentData* SaveAt(Guest& g, uint32_t slot, uint32_t index) {
  return At<ContentData>(AskDevices(g, SaveDevices::kSaveAtSlot, slot, index));
}
// SaveInterface::FreeBytes: the low word only.
uint32_t FreeBytes(Guest& g, uint32_t slot) {
  return uint32_t(AskDevices(g, SaveDevices::kFreeBytesSlot, slot));
}

// The checks before a slot's saves are read: no device, or busy (not ready
// or with a request pending). 0 when it can go on.
uint32_t CheckSlot(Guest& g, uint32_t slot, uint32_t state) {
  if (state == SaveSlot::kFailed) {
    return kNoDevice;
  }
  if (state != SaveSlot::kReady || Pending(g, slot)) {
    return kBusy;
  }
  return 0;
}

bool IsReplayName(const char* name) { return name[0] == 'r' && name[1] == 'e' && name[2] == 'p'; }

}  // namespace

// GameData::Find: the game data's save (by its file name) among the slot's
// saves: kDone, or kFoundDamaged when it couldn't be opened; kNoDevice when
// it's not there (or kNoDevice or kBusy from the slot's checks).
uint32_t game::FindGameData(Guest& g, uint32_t player) {
  const uint32_t slot = PlayerSlot(g, player);
  const int32_t count = int32_t(SaveCount(g, slot));
  if (const uint32_t answer = CheckSlot(g, slot, SlotState(g, slot))) {
    return answer;
  }
  const char* name = At<char>(kGameDataFileName);
  for (int32_t i = 0; i < count; i++) {
    if (std::strcmp(SaveAt(g, slot, uint32_t(i))->file_name, name) == 0) {
      const bool damaged = AskDevices(g, SaveDevices::kSaveFlagSlot, slot, uint32_t(i)) != 0;
      return damaged ? kFoundDamaged : kDone;
    }
  }
  return kNoDevice;
}

// SaveSlots::Device: the device's slot 2 for the player's slot.
uint32_t game::PlayerSaveDevice(Guest& g, uint32_t player) {
  const uint32_t slot = PlayerSlot(g, player);
  return AskDevices(g, SaveDevices::kDeviceSlot, slot);
}

namespace {

// Replays::CheckRoom (sub_8228D428): r3 a player. Whether the device has
// room for a replay (its size in content blocks, sub_82293660): kRoom or
// kNoRoom.
uint32_t CheckReplayRoom(Guest& g, uint32_t player) {
  const uint32_t slot = PlayerSlot(g, player);
  if (const uint32_t answer = CheckSlot(g, slot, SlotState(g, slot))) {
    return answer;
  }
  const uint32_t free_bytes = FreeBytes(g, slot);
  const uint32_t needed = g.Call(sub_82293660, SaveBuffers::kReplayBytes, 0u);
  return free_bytes >= needed ? kRoom : kNoRoom;
}

// Replays::Count (sub_8228D688): player 0's saved replays (the saves whose
// file name starts with "rep") counted into `count`; kDone.
uint32_t CountReplays(Guest& g, u32_be& count) {
  count = 0;
  const uint32_t state = SlotState(g, PlayerSlot(g, 0));
  const int32_t saves = int32_t(SaveCount(g, PlayerSlot(g, 0)));
  if (state == SaveSlot::kFailed) {
    return kNoDevice;
  }
  if (state != SaveSlot::kReady || Pending(g, PlayerSlot(g, 0))) {
    return kBusy;
  }
  for (int32_t i = 0; i < saves; i++) {
    if (IsReplayName(SaveAt(g, PlayerSlot(g, 0), uint32_t(i))->file_name)) {
      count += 1;
    }
  }
  return kDone;
}

// Replays::Sort (sub_8228CC28): the records sorted by number, lowest
// first, by swapping pairs (through a copy in the original's 176-byte
// frame, with memcpy).
void SortReplays(Guest& g, uint32_t records) {
  struct SortFrame {
    uint8_t linkage[80];
    ReplayRecord copy;
  };
  guest::StackFrame frame(g.ctx, 176);
  ReplayRecord* copy = &At<SortFrame>(frame.Local(0))->copy;
  constexpr uint32_t kBytes = sizeof(ReplayRecord);
  for (int32_t i = 1; i - 1 < Buffers()->count - 1; i++) {
    ReplayRecord* first = guest::Element<ReplayRecord>(records, uint32_t(i - 1));
    for (int32_t j = i; j < Buffers()->count; j++) {
      ReplayRecord* other = guest::Element<ReplayRecord>(records, uint32_t(j));
      if (first->number > other->number) {
        g.Call(sub_8258CB90, copy, first, kBytes);  // memcpy
        g.Call(sub_8258CB90, first, other, kBytes);
        g.Call(sub_8258CB90, other, copy, kBytes);
      }
    }
  }
}

// A word passed on the stack (the 9th argument on), 8 bytes apart.
struct StackArgument {
  u32_be value;
  uint8_t unknown_4[4];
};
GUEST_SIZE(StackArgument, 8);

// A replay's file name: "rep_" and its record's twelve numbers.
constexpr uint32_t kReplayNameFormat = 0x82011C54;  // Read-only.
constexpr uint32_t kReplayDisplayFormat = 0x82011BF8;  // UTF-16, read-only.
constexpr uint32_t kReplayDataText = 0x82011C3C;  // "REPLAY DATA", UTF-16, read-only.
constexpr uint32_t kModeNames = 0x82628050;  // A UTF-16 name a mode (writable).
constexpr uint32_t kTimeSignBit = 0x80000000u;  // The time's bit the name doesn't carry.
constexpr uint32_t kMostMinutes = 59;
constexpr uint32_t kMostTime = 0x3BEFE7;  // 59:59.999.

// The packed time's parts (ReplayRecord::time).
uint32_t TimeMinutes(uint32_t time) { return (time >> 16) & 0x7FFF; }
uint32_t TimeSeconds(uint32_t time) { return (time >> 10) & 0x3F; }
uint32_t TimeThousandths(uint32_t time) { return time & 0x3FF; }

// Replays::ReadEntry's frame (240 bytes): sscanf's last seven pointers on
// the stack, then the twelve numbers it reads.
struct ReadEntryFrame {
  uint8_t linkage[84];
  StackArgument arguments[7];
  uint8_t unknown_140[4];
  i32_be minutes;
  i32_be seconds;
  i32_be thousandths;
  i32_be year;
  i32_be month;
  i32_be day;
  i32_be hour;
  i32_be minute;
  i32_be mode;
  i32_be number;
  i32_be unknown_24;
  i32_be unknown_32;
  uint8_t unknown_192[48];
};
GUEST_OFFSET(ReadEntryFrame, minutes, 144);
GUEST_OFFSET(ReadEntryFrame, number, 180);
GUEST_SIZE(ReadEntryFrame, 240);

// Replays::ReadEntry (sub_8228D4C8): player 0's enumerated save `index`
// read into a record and a list entry when it's a replay: the twelve
// numbers from its file name (sscanf, sub_8258FA50, with the name's
// length), its file name (strcpy_s, sub_8258F7D0) and display name
// (sub_8258F8B8, a wide copy of up to 128) into the entry. kReplayRead, or
// kNotReplay, kNoDevice or kBusy.
uint32_t ReadReplayEntry(Guest& g, uint32_t index, ReplayRecord* record, ReplayEntry* entry) {
  const uint32_t state = SlotState(g, PlayerSlot(g, 0));
  if (state == SaveSlot::kFailed) {
    return kNoDevice;
  }
  if (state != SaveSlot::kReady || Pending(g, PlayerSlot(g, 0))) {
    return kBusy;
  }
  ContentData* save = SaveAt(g, PlayerSlot(g, 0), index);
  if (!IsReplayName(save->file_name)) {
    return kNotReplay;
  }
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(ReadEntryFrame)));
  ReadEntryFrame* frame = At<ReadEntryFrame>(stack.Local(0));
  // The numbers a short name leaves unread are 0 here (the original's are
  // what its stack held).
  for (i32_be* number : {&frame->minutes, &frame->seconds, &frame->thousandths, &frame->year,
                         &frame->month, &frame->day, &frame->hour, &frame->minute, &frame->mode,
                         &frame->number, &frame->unknown_24, &frame->unknown_32}) {
    *number = 0;
  }
  i32_be* on_stack[7] = {&frame->minute,  &frame->mode,    &frame->unknown_24, &frame->unknown_32,
                         &frame->minutes, &frame->seconds, &frame->thousandths};
  for (int i = 0; i < 7; i++) {
    frame->arguments[i].value = guest::AddressOf(on_stack[i]);
  }
  g.Call(sub_8258FA50, save->file_name, uint32_t(std::strlen(save->file_name)), kReplayNameFormat,
         &frame->number, &frame->year, &frame->month, &frame->day, &frame->hour);  // sscanf
  record->year = uint16_t(frame->year);
  record->month = uint16_t(frame->month);
  record->day = uint16_t(frame->day);
  record->hour = uint16_t(frame->hour);
  record->minute = uint16_t(frame->minute);
  record->mode = uint32_t(frame->mode);
  record->unknown_24 = uint32_t(frame->unknown_24);
  record->unknown_32 = uint32_t(frame->unknown_32);
  record->number = frame->number;
  record->time = (record->time & kTimeSignBit) | ((uint32_t(frame->minutes) & 0x7FFF) << 16) |
                 ((uint32_t(frame->seconds) & 0x3F) << 10) | (uint32_t(frame->thousandths) & 0x3FF);
  g.Call(sub_8258F7D0, entry->file_name, uint32_t(sizeof(entry->file_name)), save->file_name);
  g.Call(sub_8258F8B8, entry->display_name, 128u, save->display_name);
  entry->number = record->number;
  return kReplayRead;
}

// Replays::Save's frame (480 bytes): sprintf's last six arguments on the
// stack (swprintf's last two over the first two), then the file name and
// the display name it builds.
struct SaveReplayFrame {
  uint8_t linkage[84];
  StackArgument arguments[5];
  u32_be last_argument;
  char file_name[42];
  uint8_t unknown_170[6];
  guest::u16_be display_name[128];
  uint8_t unknown_432[48];
};
GUEST_OFFSET(SaveReplayFrame, last_argument, 124);
GUEST_OFFSET(SaveReplayFrame, file_name, 128);
GUEST_OFFSET(SaveReplayFrame, display_name, 176);
GUEST_SIZE(SaveReplayFrame, 480);

// Replays::Save (sub_8228D228): r3 a record, r4 the replay's data. Its file
// name printed from its twelve numbers (sprintf, sub_8258ED70; the time
// capped at 59:59.999), its display name ("REPLAY DATA", the number, the
// mode's name, the date and time; swprintf, sub_8210A5C0), then the save
// asked of player 0's slot (RequestSave, no thumbnail). Returns its answer.
uint32_t SaveReplay(Guest& g, const ReplayRecord* record, uint32_t data) {
  guest::StackFrame stack(g.ctx, uint32_t(sizeof(SaveReplayFrame)));
  SaveReplayFrame* frame = At<SaveReplayFrame>(stack.Local(0));
  std::memset(frame->file_name, 0, sizeof(frame->file_name));
  g.Call(sub_8258C950, frame->display_name, 0u, uint32_t(sizeof(frame->display_name)));  // memset
  uint32_t time = record->time;
  if ((time & 0x7FFF0000) > (kMostMinutes << 16)) {
    time = (time & kTimeSignBit) | kMostTime;
  }
  frame->arguments[0].value = record->mode;
  frame->arguments[1].value = record->unknown_24;
  frame->arguments[2].value = record->unknown_32;
  frame->arguments[3].value = TimeMinutes(time);
  frame->arguments[4].value = TimeSeconds(time);
  frame->last_argument = TimeThousandths(time);
  g.Call(sub_8258ED70, frame->file_name, kReplayNameFormat, uint32_t(record->number),
         uint32_t(record->year), uint32_t(record->month), uint32_t(record->day),
         uint32_t(record->hour), uint32_t(record->minute));  // sprintf
  const uint32_t mode_name = *guest::Element<u32_be>(kModeNames, record->mode);
  frame->arguments[0].value = uint32_t(record->hour);
  frame->arguments[1].value = uint32_t(record->minute);
  g.Call(sub_8210A5C0, frame->display_name, kReplayDisplayFormat, kReplayDataText,
         uint32_t(record->number), mode_name, uint32_t(record->year), uint32_t(record->month),
         uint32_t(record->day));  // swprintf
  const uint32_t slot = PlayerSlot(g, 0);
  return RequestSave(g, slot, frame->display_name, frame->file_name, At<char>(kSavePath), data,
                     SaveBuffers::kReplayBytes, 0, 0);
}

// Replays::BuildList (sub_8228D760): the list of player 0's saved replays
// made again: the old arrays freed (VirtualFree; not cleared), new ones for
// the count (VirtualAlloc), with an empty first entry (number 0) when
// `add_empty` and there are fewer than 99; each save read into the next
// record and entry (Replays::ReadEntry, sub_8228D4C8; a replay's moves the
// next on), then the records sorted. kDone, or the count's or a read's
// kNoDevice or kBusy.
uint32_t BuildReplayList(Guest& g, bool add_empty) {
  u32_be count;
  const uint32_t answer = CountReplays(g, count);
  if (answer != kDone) {
    return answer;
  }
  SaveBuffers* buffers = Buffers();
  if (buffers->records != 0) {
    g.Call(sub_82292D70, buffers->records, 0u, kMemRelease);  // VirtualFree
    buffers = Buffers();
  }
  if (buffers->entries != 0) {
    g.Call(sub_82292D70, buffers->entries, 0u, kMemRelease);
  }
  int32_t total = int32_t(uint32_t(count));
  const bool with_empty = total < kMostReplays && add_empty;
  if (with_empty) {
    total += 1;
  }
  const uint32_t record_bytes = uint32_t(total) * uint32_t(sizeof(ReplayRecord));
  const uint32_t entry_bytes = uint32_t(total) * uint32_t(sizeof(ReplayEntry));
  // VirtualAlloc
  Buffers()->records = g.Call(sub_82292DD8, 0u, record_bytes, kMemCommit, kPageReadWrite);
  Buffers()->entries = g.Call(sub_82292DD8, 0u, entry_bytes, kMemCommit, kPageReadWrite);
  uint32_t next = 0;
  if (with_empty) {
    Buffers()->Record(0)->number = 0;
    Buffers()->Entry(0)->number = 0;
    next = 1;
  }
  Buffers()->count = total;
  ReplayRecord* record = Buffers()->Record(next);
  ReplayEntry* entry = Buffers()->Entry(next);
  for (uint32_t i = 0; i < SaveCount(g, PlayerSlot(g, 0)); i++) {
    const int32_t read = int32_t(ReadReplayEntry(g, i, record, entry));
    if (read == kNoDevice || read == kBusy) {
      return uint32_t(read);
    }
    if (read == kReplayRead) {
      record++;
      entry++;
    }
  }
  SortReplays(g, Buffers()->records);
  return kDone;
}

// The entry whose number is `number`, or nullptr.
ReplayEntry* FindReplayEntry(int32_t number) {
  SaveBuffers* buffers = Buffers();
  for (int32_t i = 0; i < buffers->count; i++) {
    ReplayEntry* entry = buffers->Entry(uint32_t(i));
    if (entry->number == number) {
      return entry;
    }
  }
  return nullptr;
}

// Replays::Load (sub_8228D370): r3 a record. The load of its replay (the
// entry with its number) into the replay buffer asked of player 0's slot
// (SaveInterface::Load). Returns its answer.
uint32_t LoadReplay(Guest& g, uint32_t index) {
  const int32_t number = Buffers()->Record(index)->number;
  const uint32_t entry = guest::AddressOf(FindReplayEntry(number));
  const uint32_t slot = PlayerSlot(g, 0);
  // With no entry the original passes 42 and 0 on.
  return AskDevices(g, SaveDevices::kRequestLoadSlot, slot,
                    entry + uint32_t(offsetof(ReplayEntry, display_name)), entry, kSavePath,
                    uint32_t(Buffers()->replay), SaveBuffers::kReplayBytes);
}

// Replays::Delete (sub_8228D9A8): r3 a replay's number. Its delete asked
// of player 0's slot (SaveInterface::Delete); 1, or 0 when it isn't in the
// list.
uint32_t DeleteReplay(Guest& g, int32_t number) {
  ReplayEntry* entry = FindReplayEntry(number);
  if (entry == nullptr) {
    return 0;
  }
  const uint32_t slot = PlayerSlot(g, 0);
  AskDevices(g, SaveDevices::kRequestDeleteSlot, slot, entry->display_name, entry->file_name);
  return 1;
}

// Replays::SaveNew (sub_8228D918): the record being saved numbered with
// the first number from 1 the sorted list doesn't have at its place (or
// the count + 1, also when the list is empty), then saved (Replays::Save, sub_8228D228) from the
// replay buffer. Returns its answer.
uint32_t SaveNewReplay(Guest& g) {
  SaveBuffers* buffers = Buffers();
  const int32_t end = buffers->count + 1;
  int32_t number = end;
  if (end > 1) {
    number = 1;
    while (number < end && buffers->Record(uint32_t(number))->number == number) {
      number++;
    }
  }
  At<ReplayRecord>(buffers->record)->number = number;
  return SaveReplay(g, At<ReplayRecord>(Buffers()->record), Buffers()->replay);
}

// SaveBuffers::Create (sub_8228CA98): when there are none, the save
// buffers' header and their three buffers (VirtualAlloc), the header
// cleared first.
void CreateSaveBuffers(Guest& g) {
  if (*At<u32_be>(kSaveBuffers) != 0) {
    return;
  }
  auto alloc = [&](uint32_t bytes) {
    return g.Call(sub_82292DD8, 0u, bytes, kMemCommit, kPageReadWrite);  // VirtualAlloc
  };
  *At<u32_be>(kSaveBuffers) = alloc(uint32_t(sizeof(SaveBuffers)));
  std::memset(Buffers(), 0, sizeof(SaveBuffers));
  Buffers()->game_data = alloc(SaveBuffers::kGameDataBytes);
  Buffers()->replay = alloc(SaveBuffers::kReplayBytes);
  Buffers()->record = alloc(uint32_t(sizeof(ReplayRecord)));
}

// A save buffer freed (VirtualFree) and cleared, when it's set.
void FreeSaveBuffer(Guest& g, u32_be& buffer) {
  if (buffer != 0) {
    g.Call(sub_82292D70, buffer, 0u, kMemRelease);  // VirtualFree
    buffer = 0;
  }
}

// SaveBuffers::FreeReplayList (sub_8228CBD0): the replays' records.
void FreeReplayList(Guest& g) { FreeSaveBuffer(g, Buffers()->records); }

// PlayTime::Add (sub_82110928): the frames played since the last add (the
// play clock less the stats' mark; the mark moved on) split into hours,
// minutes, seconds and frames (30 a second) and added to the stats' play
// time with the carries. The total into `total`.
void AddPlayTime(SaveStats& stats, PlayTime& total) {
  constexpr uint32_t kFramesPerSecond = 30;
  constexpr uint32_t kFramesPerMinute = 1800;
  constexpr uint32_t kFramesPerHour = 108000;
  const uint32_t now = *At<u32_be>(kPlayClock);
  PlayTime& time = stats.play_time;
  const uint32_t played = now - uint32_t(stats.play_clock_mark);
  stats.play_clock_mark = now;
  uint32_t hours = played / kFramesPerHour + uint32_t(time.hours);
  uint32_t minutes = played % kFramesPerHour / kFramesPerMinute + time.minutes;
  uint32_t seconds = played % kFramesPerMinute / kFramesPerSecond + time.seconds;
  uint32_t frames = played % kFramesPerSecond + time.frames;
  seconds += frames / kFramesPerSecond;
  frames %= kFramesPerSecond;
  minutes += seconds / 60;
  seconds %= 60;
  hours += minutes / 60;
  minutes %= 60;
  time.seconds = uint8_t(seconds);
  time.minutes = uint8_t(minutes);
  time.frames = uint8_t(frames);
  time.hours = hours;
  total.hours = hours;
  total.minutes = time.minutes;
  total.seconds = time.seconds;
  total.frames = time.frames;
  total.unknown_7 = time.unknown_7;
}

// PlayTime::Update (sub_82112988): the save's play time brought up to date
// (PlayTime::Add, its total not kept).
void UpdatePlayTime(game::SaveData& save) {
  PlayTime total;
  AddPlayTime(save.stats, total);
}

}  // namespace

// SaveBuffers::Destroy: the replays' records, the three buffers and the
// header freed (the header isn't checked).
void game::DestroySaveBuffers(Guest& g) {
  FreeReplayList(g);
  FreeSaveBuffer(g, Buffers()->game_data);
  FreeSaveBuffer(g, Buffers()->replay);
  FreeSaveBuffer(g, Buffers()->record);
  g.Call(sub_82292D70, *At<u32_be>(kSaveBuffers), 0u, kMemRelease);
  *At<u32_be>(kSaveBuffers) = 0;
}

#define SBX_SAVE_INTERFACE_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                    \
  if (!On()) {                            \
    __imp__##name(ctx, base);             \
    return;                               \
  }                                       \
  Guest g(ctx, base);

// App::SaveDevices and App::Users: r3 the app; its services' (+8) slots 10
// and 9.
extern "C" REX_FUNC(sub_820949C8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820949C8)
  g.CallVirtual(At<u32_be>(At<App>(ctx.r3.u32)->services), 10);
}

extern "C" REX_FUNC(sub_820949E0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820949E0)
  g.CallVirtual(At<u32_be>(At<App>(ctx.r3.u32)->services), 9);
}

// Users::SignedIn: r3 the users, r4 the user (below their count): its
// controller when that controller's record is signed in, else -1.
extern "C" REX_FUNC(sub_82099E00) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_82099E00)
  Users* users = At<Users>(ctx.r3.u32);
  const uint32_t user = ctx.r4.u32;
  if (user < users->count) {
    const int32_t controller = *guest::Element<guest::i32_be>(guest::AddressOf(users->controllers), user);
    if (guest::Element<game::UserRecord>(guest::AddressOf(users->records), uint32_t(controller))->signed_in != 0) {
      ctx.r3.u64 = uint32_t(controller);
      return;
    }
  }
  ctx.r3.s64 = -1;
}

// The save slots' getters: r3 the slots, r4 the slot.
extern "C" REX_FUNC(sub_820A5970) {  // SaveSlot::RequestDevice
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5970)
  Slot(ctx)->requests[0] = 1;
}

extern "C" REX_FUNC(sub_820A5988) {  // SaveSlot::Device
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5988)
  ctx.r3.u64 = uint32_t(Slot(ctx)->device);
}

extern "C" REX_FUNC(sub_820A59B8) {  // SaveSlot::SaveCount
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A59B8)
  ctx.r3.u64 = uint32_t(Slot(ctx)->save_count);
}

extern "C" REX_FUNC(sub_820A5A10) {  // SaveSlot::State
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5A10)
  ctx.r3.u64 = uint32_t(Slot(ctx)->state);
}

extern "C" REX_FUNC(sub_820A59A0) {  // SaveSlot::FreeBytes (a doubleword)
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A59A0)
  ctx.r3.u64 = uint64_t(Slot(ctx)->free_bytes);
}

// SaveSlot::SaveAt: r5's save (308 bytes each).
extern "C" REX_FUNC(sub_820A59D0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A59D0)
  ctx.r3.u64 = guest::AddressOf(guest::Element<ContentData>(Slot(ctx)->saves, ctx.r5.u32));
}

// SaveSlot::Pending: the six requests' sum.
extern "C" REX_FUNC(sub_820A5A28) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5A28)
  uint32_t sum = 0;
  for (const u32_be& request : Slot(ctx)->requests) {
    sum += request;
  }
  ctx.r3.u64 = sum;
}

// SaveDevices::SetRootName: r3 the devices, r4 the name copied in (with its
// terminator).
extern "C" REX_FUNC(sub_820A57A0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A57A0)
  char* to = At<SaveDevices>(ctx.r3.u32)->root_name;
  for (const char* from = At<char>(ctx.r4.u32); (*to++ = *from++) != 0;) {
  }
}

// PlayTime::Update: r3 the save.
extern "C" REX_FUNC(sub_82112988) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_82112988)
  UpdatePlayTime(*At<game::SaveData>(ctx.r3.u32));
}

// SaveInterface's calls into the save devices: r3 (and r4) passed on as r4
// (and r5) to the devices' slot; r5 otherwise as the first call leaves it.
#define SBX_DEVICES_CALL(address, slot)                                         \
  extern "C" REX_FUNC(sub_##address) {                                          \
    SBX_SAVE_INTERFACE_FALLBACK(sub_##address)                                  \
    const uint32_t a = ctx.r3.u32;                                              \
    const uint32_t devices = AppPart(g, App::kSaveDevicesSlot);                 \
    g.CallVirtual(At<u32_be>(devices), slot, a);                                \
  }

SBX_DEVICES_CALL(820A03F0, 1)  // SaveInterface::RequestDevice
SBX_DEVICES_CALL(820A04B0, 4)  // SaveInterface::SaveCount
SBX_DEVICES_CALL(820A0560, 8)  // SaveInterface::Pending
SBX_DEVICES_CALL(820A05B8, 2)  // SaveInterface::Device

#undef SBX_DEVICES_CALL

// SaveInterface::FreeBytes: the low word of the devices' slot 3.
extern "C" REX_FUNC(sub_820A0508) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0508)
  ctx.r3.u64 = FreeBytes(g, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_820A0610) {  // SaveInterface::SaveAt
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0610)
  const uint32_t a = ctx.r3.u32, b = ctx.r4.u32;
  const uint32_t devices = AppPart(g, App::kSaveDevicesSlot);
  g.CallVirtual(At<u32_be>(devices), 5, a, b);
}

// SaveInterface::UserCount and UserAt: the users' slots 1 and 2.
extern "C" REX_FUNC(sub_820A07C0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A07C0)
  UserCount(g);
}

extern "C" REX_FUNC(sub_820A0808) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0808)
  UserAt(g, ctx.r3.u32);
}

// SaveSlots::OfController: r3 the controller: the first user it is, or the
// user count (also when that's 0 or below).
extern "C" REX_FUNC(sub_8228CB68) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CB68)
  ctx.r3.s64 = SlotOfController(g, ctx.r3.u32);
}

// Two plain getters: the word at +92 and the word at +216 (the users'
// count, for a Users).
extern "C" REX_FUNC(sub_8236FD60) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8236FD60)
  struct Object92 {
    uint8_t unknown_0[92];
    u32_be word_92;
  };
  ctx.r3.u64 = uint32_t(At<Object92>(ctx.r3.u32)->word_92);
}

extern "C" REX_FUNC(sub_82473DE8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_82473DE8)
  ctx.r3.u64 = uint32_t(At<Users>(ctx.r3.u32)->count);
}

// SaveInterface::Init: r3 the app's interface, kept in [0x826AD970]; the
// save devices' root name set to "TXR360". r3 as the set leaves it.
extern "C" REX_FUNC(sub_820A01A0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A01A0)
  *At<u32_be>(kApp) = ctx.r3.u32;
  g.CallVirtual(Devices(g), SaveDevices::kSetRootNameSlot, kRootName);
}

// SaveInterface::Clear: the app's interface forgotten.
extern "C" REX_FUNC(sub_820A01F0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A01F0)
  *At<u32_be>(kApp) = 0;
}

// SaveInterface::SlotState: r3 the slot.
extern "C" REX_FUNC(sub_820A0200) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0200)
  ctx.r3.u64 = SlotState(g, ctx.r3.u32);
}

// SaveInterface::Failure: r3 the slot. Its failure; past 5 (the delete's)
// 0.
extern "C" REX_FUNC(sub_820A02E0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A02E0)
  const uint32_t slot = ctx.r3.u32;
  const uint32_t failure = g.CallVirtual(Devices(g), SaveDevices::kFailureSlot, slot);
  ctx.r3.u64 = failure <= SaveSlot::kDeleteFailed ? failure : 0;
}

// SaveInterface::AcknowledgeFailure: r3 the slot.
extern "C" REX_FUNC(sub_820A0398) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0398)
  const uint32_t slot = ctx.r3.u32;
  g.CallVirtual(Devices(g), SaveDevices::kAcknowledgeFailureSlot, slot);
}

// SaveInterface::SaveFlag: r3 the slot, r4 an enumerated save.
extern "C" REX_FUNC(sub_820A0448) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0448)
  const uint32_t slot = ctx.r3.u32, item = ctx.r4.u32;
  g.CallVirtual(Devices(g), SaveDevices::kSaveFlagSlot, slot, item);
}

// SaveInterface::Save: r3 the slot, r4 the display name, r5 the file name,
// r6 the path, r7 the data, r8 its size, r9 a thumbnail, r10 its size.
extern "C" REX_FUNC(sub_820A0678) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0678)
  ctx.r3.u64 = RequestSave(g, ctx.r3.u32, At<guest::u16_be>(ctx.r4.u32), At<char>(ctx.r5.u32),
                           At<char>(ctx.r6.u32), ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32);
}

// SaveInterface::Load: r3 the slot, r4 the display name, r5 the file name,
// r6 the path, r7 the buffer, r8 its size: the save devices' RequestLoad.
extern "C" REX_FUNC(sub_820A06F8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A06F8)
  const uint32_t slot = ctx.r3.u32, name = ctx.r4.u32, file = ctx.r5.u32, path = ctx.r6.u32,
                 buffer = ctx.r7.u32, size = ctx.r8.u32;
  g.CallVirtual(Devices(g), SaveDevices::kRequestLoadSlot, slot, name, file, path, buffer, size);
}

// SaveInterface::Delete: r3 the slot, r4 the display name, r5 the file
// name: the save devices' RequestDelete.
extern "C" REX_FUNC(sub_820A0768) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A0768)
  const uint32_t slot = ctx.r3.u32, name = ctx.r4.u32, file = ctx.r5.u32;
  g.CallVirtual(Devices(g), SaveDevices::kRequestDeleteSlot, slot, name, file);
}

// The save devices' own slots: r3 the devices, r4 the slot.

// SaveDevices::SaveFlag (slot 6): r5 an enumerated save; its flag.
extern "C" REX_FUNC(sub_820A59F0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A59F0)
  ctx.r3.u64 = uint32_t(SaveFlag(Slot(ctx), ctx.r5.u32));
}

// SaveDevices::Failure (slot 11) and AcknowledgeFailure (slot 12).
extern "C" REX_FUNC(sub_820A5A70) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5A70)
  ctx.r3.u64 = uint32_t(Slot(ctx)->failure);
}

extern "C" REX_FUNC(sub_820A5A88) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5A88)
  Slot(ctx)->requests[SaveSlot::kFailureAcknowledged] = 1;
}

// SaveSlot::RequestSave (slot 15): r5 the display name, r6 the file name,
// r7 the path, r8 the data, r9 its size, r10 a thumbnail, its size on the
// caller's stack (+84).
extern "C" REX_FUNC(sub_820A5500) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5500)
  const SaveRequest request{ctx.r5.u32,
                            At<char>(ctx.r6.u32),
                            At<char>(ctx.r7.u32),
                            ctx.r8.u32,
                            ctx.r9.u32,
                            ctx.r10.u32,
                            // The caller's stack argument.
                            At<SaveRequestStack>(ctx.r1.u32)->thumbnail_size};
  ctx.r3.u64 = RequestSlot(g, At<SaveDevices>(ctx.r3.u32), ctx.r4.u32, request,
                           SaveSlot::kSaveRequest);
}

// SaveSlot::RequestLoad (slot 16): r5 the display name, r6 the file name,
// r7 the path, r8 the buffer, r9 its size; no thumbnail.
extern "C" REX_FUNC(sub_820A5608) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5608)
  const SaveRequest request{ctx.r5.u32, At<char>(ctx.r6.u32), At<char>(ctx.r7.u32),
                            ctx.r8.u32, ctx.r9.u32,           0,
                            0};
  ctx.r3.u64 = RequestSlot(g, At<SaveDevices>(ctx.r3.u32), ctx.r4.u32, request,
                           SaveSlot::kLoadRequest);
}

// SaveSlot::RequestDelete (slot 17): r5 the display name, r6 the file name.
extern "C" REX_FUNC(sub_820A5708) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5708)
  const SaveRequest request{ctx.r5.u32, At<char>(ctx.r6.u32), nullptr, 0, 0, 0, 0};
  ctx.r3.u64 = RequestSlot(g, At<SaveDevices>(ctx.r3.u32), ctx.r4.u32, request,
                           SaveSlot::kDeleteRequest);
}

// The slot state handlers (the save devices' handlers): r3 the devices,
// r4 the slot. Each returns the slot's next state.
#define SBX_SLOT_HANDLER(address, call)                       \
  extern "C" REX_FUNC(sub_##address) {                        \
    SBX_SAVE_INTERFACE_FALLBACK(sub_##address)                \
    [[maybe_unused]] SaveDevices* devices = At<SaveDevices>(ctx.r3.u32); \
    SaveSlot* slot = At<SaveSlot>(ctx.r4.u32);                \
    ctx.r3.u64 = call;                                        \
  }

SBX_SLOT_HANDLER(820A4A98, WaitForUser(slot))                  // state 0
SBX_SLOT_HANDLER(820A5AA0, WaitForDevice(g, devices, slot))    // state 1
SBX_SLOT_HANDLER(820A4AB8, Failed(slot))                       // state 2
SBX_SLOT_HANDLER(820A5B00, Ready(g, devices, slot))            // state 3
SBX_SLOT_HANDLER(820A5BE8, EnumerateStep(g, devices, slot))    // state 5
SBX_SLOT_HANDLER(820A5CB0, CheckStep(g, devices, slot))        // state 6
SBX_SLOT_HANDLER(820A6238, DeleteStep(g, devices, slot))       // state 9

#undef SBX_SLOT_HANDLER

// The steps' waits: r4 the slot. Each answers 1 done, 0 pending or its
// error.
#define SBX_SLOT_WAIT(address, call)                \
  extern "C" REX_FUNC(sub_##address) {              \
    SBX_SAVE_INTERFACE_FALLBACK(sub_##address)      \
    SaveSlot* slot = At<SaveSlot>(ctx.r4.u32);      \
    ctx.r3.s64 = call;                              \
  }

SBX_SLOT_WAIT(820A4E28, WaitContent(g, slot->open_overlapped, -8))      // WaitCloseItem
SBX_SLOT_WAIT(820A54B0, WaitContent(g, slot->content_overlapped, -12))  // WaitDelete
SBX_SLOT_WAIT(820A53B0, WaitContent(g, slot->content_overlapped, -9))   // WaitThumbnail
SBX_SLOT_WAIT(820A5088, WaitFile(g, slot, -9, -10))                     // WaitWrite
SBX_SLOT_WAIT(820A5218, WaitFile(g, slot, -11, -11))                    // WaitRead
SBX_SLOT_WAIT(820A5290, WaitClose(g, slot))                             // WaitClose

#undef SBX_SLOT_WAIT

// SaveDevices::OnEvent: r3 the devices, r4 the run loop, r5 the event: on
// the shut-down, its update and event handler removed from the run loop.
extern "C" REX_FUNC(sub_820A4A28) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A4A28)
  SaveDevices* devices = At<SaveDevices>(ctx.r3.u32);
  RunLoop* run_loop = At<RunLoop>(ctx.r4.u32);
  if (At<RunLoopEvent>(ctx.r5.u32)->kind == RunLoopEvent::kShutDown) {
    g.CallVirtual(run_loop, RunLoop::kRemoveTickHandlerSlot, &devices->tick_key);
    g.CallVirtual(run_loop, RunLoop::kRemoveListenerSlot, &devices->listener_key);
  }
}

// SaveSlot::AllocBuffers: r3 the slot, r4 how many saves.
extern "C" REX_FUNC(sub_820A4788) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A4788)
  AllocBuffers(g, At<SaveSlot>(ctx.r3.u32), ctx.r4.u32);
}

// SaveDevices::SaveDevices: r3 the devices, r4 what they're made with, r5
// how many saves a slot enumerates. Returns the devices.
extern "C" REX_FUNC(sub_820A6388) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A6388)
  const uint32_t devices = ctx.r3.u32;
  ConstructDevices(g, At<SaveDevices>(devices), At<game::SaveDevicesSetUp>(ctx.r4.u32),
                   ctx.r5.u32);
  ctx.r3.u64 = devices;
}

// SaveDevices' deleting destructor (its vtable slot 0): r3 the devices, r4
// the flags: destroyed, then freed (operator delete) with bit 0. Returns
// the devices.
extern "C" REX_FUNC(sub_820A6338) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A6338)
  SaveDevices* devices = At<SaveDevices>(ctx.r3.u32);
  const uint32_t flags = ctx.r4.u32;
  DestroyDevices(g, devices);
  if ((flags & 1) != 0) {
    g.Call(sub_8258C868, devices);  // operator delete
  }
  ctx.r3.u64 = guest::AddressOf(devices);
}

// SaveDevices::Destroy: r3 the devices.
extern "C" REX_FUNC(sub_820A5908) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5908)
  DestroyDevices(g, At<SaveDevices>(ctx.r3.u32));
}

// SlotArray::Reset: r3 the word holding the slots.
extern "C" REX_FUNC(sub_820A5810) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_820A5810)
  ResetSlotArray(g, *At<u32_be>(ctx.r3.u32));
}

// --- The save layer ---

// SaveSlots::Device and SaveSlots::Pending: r3 a player; their slot's.
extern "C" REX_FUNC(sub_8228CE78) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CE78)
  ctx.r3.u64 = game::PlayerSaveDevice(g, ctx.r3.u32);
}

extern "C" REX_FUNC(sub_8228CEB0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CEB0)
  const uint32_t slot = PlayerSlot(g, ctx.r3.u32);
  AskDevices(g, SaveDevices::kPendingSlot, slot);
}

// Users::Name (probably): r3 a player, r4 a buffer: XUserGetName
// (sub_82292A60) of their controller into it, 16 bytes.
extern "C" REX_FUNC(sub_8228CBB8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CBB8)
  const uint32_t controller = *guest::Element<u32_be>(kSaveController, ctx.r3.u32);
  g.Call(sub_82292A60, controller, ctx.r4.u32, 16u);
}

// GameData::Find: r3 a player.
extern "C" REX_FUNC(sub_8228D138) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D138)
  ctx.r3.u64 = game::FindGameData(g, ctx.r3.u32);
}

// Replays::CheckRoom: r3 a player.
extern "C" REX_FUNC(sub_8228D428) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D428)
  ctx.r3.u64 = CheckReplayRoom(g, ctx.r3.u32);
}

// Replays::Count: r3 the count (out).
extern "C" REX_FUNC(sub_8228D688) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D688)
  ctx.r3.u64 = CountReplays(g, *At<u32_be>(ctx.r3.u32));
}

// Replays::ReadEntry: r3 an enumerated save, r4 a record, r5 a list entry.
extern "C" REX_FUNC(sub_8228D4C8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D4C8)
  ctx.r3.u64 =
      ReadReplayEntry(g, ctx.r3.u32, At<ReplayRecord>(ctx.r4.u32), At<ReplayEntry>(ctx.r5.u32));
}

// Replays::Save: r3 a record, r4 the replay's data.
extern "C" REX_FUNC(sub_8228D228) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D228)
  ctx.r3.u64 = SaveReplay(g, At<ReplayRecord>(ctx.r3.u32), ctx.r4.u32);
}

// Replays::BuildList: r3 nonzero for an empty first entry.
extern "C" REX_FUNC(sub_8228D760) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D760)
  ctx.r3.u64 = BuildReplayList(g, ctx.r3.u32 != 0);
}

// Replays::Sort: r3 the records.
extern "C" REX_FUNC(sub_8228CC28) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CC28)
  SortReplays(g, ctx.r3.u32);
}

// Replays::Load: r3 a record's index.
extern "C" REX_FUNC(sub_8228D370) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D370)
  ctx.r3.u64 = LoadReplay(g, ctx.r3.u32);
}

// Replays::SaveNew: no arguments.
extern "C" REX_FUNC(sub_8228D918) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D918)
  SaveNewReplay(g);
}

// Replays::SaveAs: r3 a number, given to the record being saved, which is
// then saved from the replay buffer (Replays::Save).
extern "C" REX_FUNC(sub_8228D988) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D988)
  At<ReplayRecord>(Buffers()->record)->number = int32_t(ctx.r3.u32);
  ctx.r3.u64 = SaveReplay(g, At<ReplayRecord>(Buffers()->record), Buffers()->replay);
}

// Replays::Delete: r3 a replay's number.
extern "C" REX_FUNC(sub_8228D9A8) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228D9A8)
  ctx.r3.u64 = DeleteReplay(g, int32_t(ctx.r3.u32));
}

// Replays::SaveFlag (probably): r3 a record's index: its save's flag on
// player 0's slot (SaveInterface::SaveFlag), by the record's number.
extern "C" REX_FUNC(sub_8228DA28) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228DA28)
  const int32_t number = Buffers()->Record(ctx.r3.u32)->number;
  const uint32_t slot = PlayerSlot(g, 0);
  AskDevices(g, SaveDevices::kSaveFlagSlot, slot, number);
}

// SaveBuffers::Create, FreeReplayList and Destroy: no arguments.
extern "C" REX_FUNC(sub_8228CA98) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CA98)
  CreateSaveBuffers(g);
}

extern "C" REX_FUNC(sub_8228CBD0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CBD0)
  FreeReplayList(g);
}

extern "C" REX_FUNC(sub_8228CCE0) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_8228CCE0)
  game::DestroySaveBuffers(g);
}

// PlayTime::Add: r3 the total (out), r4 the save's stats.
extern "C" REX_FUNC(sub_82110928) {
  SBX_SAVE_INTERFACE_FALLBACK(sub_82110928)
  AddPlayTime(*At<SaveStats>(ctx.r4.u32), *At<PlayTime>(ctx.r3.u32));
}

// SaveGame::Write: no arguments. The save's play time brought up to date,
// its display name formatted (swprintf, sub_8210A5C0, into 256 cleared
// bytes of the original's 368-byte frame, with the local time,
// sub_82294300), then the write asked of the save devices (RequestSave)
// for the saving controller's slot. Called from autosave's override
// (autosave.cpp), which logs it.
void hooks::NativeSaveInterface_Write(PPCContext& ctx, uint8_t* base) {
  if (!On()) {
    __imp__sub_8228CEE8(ctx, base);
    return;
  }
  Guest g(ctx, base);
  struct WriteFrame {
    uint8_t linkage[80];
    LocalTime time;
    guest::u16_be name[128];  // UTF-16
  };
  guest::StackFrame frame(ctx, 368);
  WriteFrame* locals = At<WriteFrame>(frame.Local(0));
  LocalTime* time = &locals->time;
  guest::u16_be* name = locals->name;
  g.Call(sub_8258C950, name, 0u, uint32_t(sizeof(locals->name)));  // memset
  UpdatePlayTime(*At<game::SaveData>(g.Call(sub_82105790, 0u)));  // GameData::Get
  g.Call(sub_82294300, time);  // the local time
  g.Call(sub_8210A5C0, name, kSaveNameFormat, kSaveNameText, uint32_t(time->year),
         uint32_t(time->month), uint32_t(time->day), uint32_t(time->hour),
         uint32_t(time->minute));  // swprintf
  const uint32_t save = g.Call(sub_82105790, 0u);  // GameData::Get
  const uint32_t slot = uint32_t(SlotOfController(g, *At<u32_be>(kSaveController)));
  RequestSave(g, slot, name, At<char>(kGameDataFileName), At<char>(kSavePath), save, kSaveBytes,
              0, 0);
}
