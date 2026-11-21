// The options screen's load and save pages as our own C++
// (--native_options_load_save, on by default): each page's sub-pages (the
// device check, the question, the note, the save's wait, the close, the
// cancel, the load's device answer), their dialogs, and the steps that
// leave the pages.
//
// Each page's sub-page is its own ScreenPages block; the options screen's
// pages are another. The menu, the sounds, the UI movies and the save
// layer's requests are called as they are; three of the save layer's
// (save_interface.cpp) are called as C++.

#include "base/settings.h"
#include "game/menu.h"
#include "game/options_load_save.h"
#include "game/race_screens.h"
#include "game/save_interface.h"

SBX_SETTING_BOOL(native_options_load_save, true, "Overrides", "Data and Saves",
                 "Our own C++ for the options screen's load and save pages (their "
                 "sub-pages, dialogs and leaving steps) instead of the recompiled functions");

namespace {

using game::MenuCursor;
using game::OptionsScreen;
using game::ScreenPages;
using guest::At;
using guest::Guest;
using guest::i32_be;
using guest::u32_be;

bool On() { return SBX_GET(native_options_load_save); }

// The options screen's globals (writable): [each] the object.
constexpr uint32_t kScreenPages = 0x826C7F18;  // The options screen's pages.
constexpr uint32_t kOptionsScreen = 0x826C7F20;
constexpr uint32_t kLoadSubPages = 0x826C7F24;
constexpr uint32_t kDialogMenu = 0x826C7F2C;  // The dialog's menu cursor.
constexpr uint32_t kSaveSubPages = 0x826C7F30;
constexpr uint32_t kMovieTable = 0x826B59AC;   // The UI movies, by key (writable).
constexpr uint32_t kOptionsMovie = 7;

constexpr uint32_t kPlayer = 0;  // The pages work on player 0's slot.

// The sub-pages.
enum SubPage : int32_t {
  kCheckDevice = 0,
  kQuestion = 1,  // A dialog's sub-page is its dialog + 1.
  kNote = 2,
  kWait = 3,
  kClose = 4,
  kDeviceAnswer = 6,
};

// The dialogs (OptionsScreen's load_dialog and save_dialog).
enum Dialog : uint32_t {
  kQuestionDialog = 0,
  kNoteDialog = 1,
  kWaitDialog = 2,
};

// Menu sounds (MenuSound::Play).
constexpr uint32_t kAcceptSound = 0;
constexpr uint32_t kMoveSound = 4;

// The save slot's states the pages look for (SaveSlots::State).
constexpr uint32_t kSlotNoDevice = 1;
constexpr uint32_t kSlotFailed = 2;
constexpr uint32_t kSlotReady = 3;
constexpr uint32_t kSlotSelectingDevice = 4;
constexpr uint32_t kSlotSaving = 7;

// What each page differs in.
struct LoadSavePage {
  int32_t page;  // The options screen's page.
  uint32_t sub_pages;  // The global holding the page's sub-pages.
  int32_t dialog_step, dialog_step_after;  // The options steps that show a dialog.
  int32_t move_step;   // The step that moves the dialog's cursor.
  int32_t close_step;  // The step that leaves the page (Leave, below).
  int32_t no_device_kind;  // The question asked when there's no device.
  i32_be OptionsScreen::*dialog;
  i32_be OptionsScreen::*kind;
  i32_be OptionsScreen::*ticks;
  uint32_t first_movie_label, cursor_movie_labels;  // The movie's labels finished on leaving.
  int32_t leave_animation;
};

constexpr LoadSavePage kLoadPage = {9,  kLoadSubPages, 44, 43, 45, 41, 7,
                                    &OptionsScreen::load_dialog, &OptionsScreen::load_kind,
                                    &OptionsScreen::load_ticks, 841, 843, 10};
constexpr LoadSavePage kSavePage = {10, kSaveSubPages, 47, 46, 48, 42, 11,
                                    &OptionsScreen::save_dialog, &OptionsScreen::save_kind,
                                    &OptionsScreen::save_ticks, 857, 859, 11};

ScreenPages* Pages(uint32_t global) { return At<ScreenPages>(*At<u32_be>(global)); }
OptionsScreen* Options() { return At<OptionsScreen>(*At<u32_be>(kOptionsScreen)); }
MenuCursor* DialogMenu() { return At<MenuCursor>(*At<u32_be>(kDialogMenu)); }

// The options screen's pages to `next` by the steps `pending` then
// `pending_after` (the page before kept when it changes).
void ShowPage(ScreenPages* pages, int32_t next, int32_t pending, int32_t pending_after) {
  if (pages->page != next) {
    pages->unknown_8 = pages->page;
  }
  pages->pending = pending;
  pages->pending_after = pending_after;
  pages->next = next;
  pages->page = -1;
}

// A page's sub-page set (the one before kept when it changes).
void SetSubPage(const LoadSavePage& page, int32_t sub_page) {
  ScreenPages* pages = Pages(page.sub_pages);
  if (pages->page != sub_page) {
    pages->unknown_8 = pages->page;
  }
  pages->page = sub_page;
  pages->pending = -1;
  pages->pending_after = -1;
}

// LoadPage::SetDialog and SavePage::SetDialog (sub_821B9D88,
// sub_821B9C38): with a dialog (below 3), the page shown again with the
// dialog's steps and its sub-page set to the dialog's; the kind kept
// either way.
void SetDialog(const LoadSavePage& page, int32_t kind, uint32_t dialog) {
  if (dialog <= kWaitDialog) {
    ShowPage(Pages(kScreenPages), page.page, page.dialog_step, page.dialog_step_after);
    SetSubPage(page, int32_t(dialog) + 1);
    Options()->*page.dialog = int32_t(dialog);
  }
  Options()->*page.kind = kind;
}

void PlaySound(Guest& g, uint32_t sound) { g.Call(sub_82110318, sound); }  // MenuSound::Play
void RequestDevice(Guest& g) { g.Call(sub_8228CD98, kPlayer); }  // SaveSlots::RequestDevice

// The dialog's menu stepped (MenuCursor::Update). True when its cursor
// moved: the page shown again with the move step, and the move's sound.
bool StepMenu(Guest& g, const LoadSavePage& page) {
  g.Call(sub_82184468, DialogMenu());
  if (DialogMenu()->moved != 0) {
    ShowPage(Pages(kScreenPages), page.page, page.move_step, -1);
    PlaySound(g, kMoveSound);
    return true;
  }
  return false;
}

bool Pressed(Guest& g, uint32_t action) {
  return g.Call(sub_821843C8, DialogMenu(), action) != 0;  // MenuCursor::Pressed
}

// LoadPage::CheckDevice and SavePage::CheckDevice (sub-page 0): unless
// the device selector is up, the wait with a device, else the question
// with none.
void CheckDevice(Guest& g, const LoadSavePage& page) {
  if (g.Call(sub_8228CDD0, kPlayer) == kSlotSelectingDevice) {  // SaveSlots::State
    return;
  }
  if (game::PlayerSaveDevice(g, kPlayer) != 0) {
    SetDialog(page, 0, kWaitDialog);
  } else {
    SetDialog(page, page.no_device_kind, kQuestionDialog);
  }
}

// LoadPage::Answer (sub-page 1): the question's answer (the cursor's row:
// 0 yes), by its kind: 0 and 1 load the game data; 4 asks for a device;
// 5 and 6 close, or go back to the wait; 7 closes, or asks for a device.
// "No" to a load or a device: the note of kind 6.
void LoadAnswer(Guest& g) {
  const LoadSavePage& page = kLoadPage;
  if (StepMenu(g, page)) {
    return;
  }
  const bool yes = DialogMenu()->row == 0;
  if (!Pressed(g, game::kMenuAccept)) {
    Pressed(g, game::kMenuCancel);  // Its answer isn't used.
    return;
  }
  PlaySound(g, kAcceptSound);
  switch (int32_t(Options()->load_kind)) {
    case 0:
    case 1:
      if (yes) {
        g.Call(sub_8228CFA8, kPlayer);  // GameData::Load
        SetDialog(page, 8, kWaitDialog);
      } else {
        SetDialog(page, 6, kQuestionDialog);
      }
      break;
    case 4:
      if (yes) {
        RequestDevice(g);
        SetSubPage(page, kCheckDevice);
      } else {
        SetDialog(page, 6, kQuestionDialog);
      }
      break;
    case 5:
    case 6:
      if (yes) {
        SetSubPage(page, kClose);
      } else {
        SetDialog(page, 0, kWaitDialog);
      }
      break;
    case 7:
      if (yes) {
        SetSubPage(page, kClose);
      } else {
        RequestDevice(g);
        SetSubPage(page, kCheckDevice);
      }
      break;
  }
}

// SavePage::Answer (sub-page 1): as the load's: 0, 1 and 5 save the game
// data; 4 asks for a device; 6 closes, or goes back to the wait; 11
// closes, or asks for a device.
void SaveAnswer(Guest& g) {
  const LoadSavePage& page = kSavePage;
  if (StepMenu(g, page)) {
    return;
  }
  if (!Pressed(g, game::kMenuAccept)) {
    Pressed(g, game::kMenuCancel);  // Its answer isn't used.
    return;
  }
  const bool yes = DialogMenu()->row == 0;
  PlaySound(g, kAcceptSound);
  switch (uint32_t(Options()->save_kind)) {
    case 0:
    case 1:
    case 5:
      if (yes) {
        SetDialog(page, 8, kWaitDialog);
        g.Call(sub_8228CEE8);  // GameData::Save
      } else {
        SetDialog(page, 6, kQuestionDialog);
      }
      break;
    case 4:
      if (yes) {
        SetSubPage(page, kCheckDevice);
        RequestDevice(g);
      } else {
        SetDialog(page, 6, kQuestionDialog);
      }
      break;
    case 6:
      if (yes) {
        SetSubPage(page, kClose);
      } else {
        SetDialog(page, 0, kWaitDialog);
      }
      break;
    case 11:
      if (yes) {
        SetSubPage(page, kClose);
      } else {
        SetSubPage(page, kCheckDevice);
        RequestDevice(g);
      }
      break;
  }
}

// The notes (sub-page 2): counted in ticks; the note of kind 3 (no device
// at the start) asks for a device by itself after 45. On accept, by the
// kind.
bool NoteTimedOut(Guest& g, const LoadSavePage& page) {
  g.Call(sub_82184468, DialogMenu());  // MenuCursor::Update
  OptionsScreen* options = Options();
  options->*page.ticks += 1;
  if (options->*page.ticks >= 45 && options->*page.kind == 3) {
    RequestDevice(g);
    SetSubPage(page, kCheckDevice);
    return true;
  }
  return false;
}

// LoadPage::Note: 3 asks for a device; 4, 5 and 10 show the note of kind
// 6; 11 the note of kind 10; the others close.
void LoadNote(Guest& g) {
  const LoadSavePage& page = kLoadPage;
  if (NoteTimedOut(g, page) || !Pressed(g, game::kMenuAccept)) {
    return;
  }
  PlaySound(g, kAcceptSound);
  switch (uint32_t(Options()->load_kind)) {
    case 3:
      RequestDevice(g);
      SetSubPage(page, kCheckDevice);
      break;
    case 4:
    case 5:
    case 10:
      SetDialog(page, 6, kQuestionDialog);
      break;
    case 11:
      SetDialog(page, 10, kNoteDialog);
      break;
    case 0:
    case 1:
    case 2:
    case 6:
    case 7:
    case 8:
    case 9:
    case 12:
      SetSubPage(page, kClose);
      break;
  }
}

// SavePage::Note: 2 and 9 close; 3 asks for a device; 7 and 10 show the
// question of kind 6.
void SaveNote(Guest& g) {
  const LoadSavePage& page = kSavePage;
  if (NoteTimedOut(g, page) || !Pressed(g, game::kMenuAccept)) {
    return;
  }
  PlaySound(g, kAcceptSound);
  switch (int32_t(Options()->save_kind)) {
    case 2:
    case 9:
      SetSubPage(page, kClose);
      break;
    case 3:
      RequestDevice(g);
      SetSubPage(page, kCheckDevice);
      break;
    case 7:
    case 10:
      SetDialog(page, 6, kQuestionDialog);
      break;
  }
}

// SavePage::Wait (sub-page 3): with no device, the question of kind 4.
// Else after 90 ticks: the check's (kind 0) answer from GameData::Check
// (questions 1 and 5, the note 7); the save's (kind 8) end once the slot
// isn't saving: a failed save's note (10, or 7 when the failure is 3) with
// the failure acknowledged (sub_8228CE40), a good one's note 9.
void SaveWait(Guest& g) {
  const LoadSavePage& page = kSavePage;
  const uint32_t state = g.Call(sub_8228CDD0, kPlayer);  // SaveSlots::State
  if (state == kSlotNoDevice) {
    SetDialog(page, 4, kQuestionDialog);
    return;
  }
  OptionsScreen* options = Options();
  options->save_ticks += 1;
  if (options->save_ticks < 90) {
    return;
  }
  const int32_t kind = options->save_kind;
  if (kind == 0) {
    switch (g.Call(sub_8228D038, kPlayer)) {  // GameData::Check
      case 0:
        SetDialog(page, 1, kQuestionDialog);
        break;
      case 1:
        SetDialog(page, 5, kQuestionDialog);
        break;
      case 2:
        SetDialog(page, 7, kNoteDialog);
        break;
    }
  } else if (kind == 8 && state != kSlotSaving) {
    if (state == kSlotFailed) {
      const int32_t failure = int32_t(g.Call(sub_8228CE08, kPlayer));  // SaveSlots::Failure
      if (failure == 2) {
        SetDialog(page, 10, kNoteDialog);
      } else if (failure == 3) {
        SetDialog(page, 7, kNoteDialog);
      }
      g.Call(sub_8228CE40, kPlayer);  // SaveSlots::AcknowledgeFailure
    } else if (state == kSlotReady) {
      SetDialog(page, 9, kNoteDialog);
    }
  }
}

// LoadPage::DeviceAnswer (sub-page 6): by GameData::Find: no game data (3)
// or a good one (7) acknowledges the failure and shows the note of kind
// 10; a damaged one (8) the note of kind 11.
void LoadDeviceAnswer(Guest& g) {
  const LoadSavePage& page = kLoadPage;
  const int32_t found = int32_t(game::FindGameData(g, kPlayer));
  if (found == 3 || found == 7) {
    g.Call(sub_8228CE40, kPlayer);  // SaveSlots::AcknowledgeFailure
    SetDialog(page, 10, kNoteDialog);
  } else if (found == 8) {
    SetDialog(page, 11, kNoteDialog);
  }
}

// LoadPage::Close and SavePage::Close (sub-page 4): the page left by its
// close step; the options screen told.
void ClosePage(const LoadSavePage& page) {
  ShowPage(Pages(kScreenPages), -1, page.close_step, -1);
  Options()->page_closing = 1;
}

// LoadPage::Leave and SavePage::Leave (steps 41 and 42): r3 the started
// word. The options movie (by its key, passed by address from the
// original's 128-byte frame): the first time, the page's labels finished
// (the second by the dialog's cursor), its leave animation played and the
// category page set up (sub_821C3180). Each time it runs; once it's done,
// the options screen's pages back to page 0 (step 5) and the save buffers
// freed. Returns 0.
uint32_t LeavePage(Guest& g, const LoadSavePage& page, u32_be* started) {
  struct LeaveFrame {
    uint8_t linkage[80];
    u32_be movie_key;
  };
  guest::StackFrame frame(g.ctx, 128);
  u32_be* key = &At<LeaveFrame>(frame.Local(0))->movie_key;
  *key = kOptionsMovie;
  const uint32_t movie = *At<u32_be>(g.Call(sub_8211B5C0, kMovieTable, key));  // UiMovies::Find
  if (*started == 0) {
    const int32_t row = DialogMenu()->row;
    g.Call(sub_8211E0E0, movie, page.first_movie_label);  // UiMovie::Finish
    g.Call(sub_8211E0E0, movie, page.cursor_movie_labels + uint32_t(row));
    g.Call(sub_8211DB38, movie, -1, page.leave_animation, 0u, 0u, 0u, 0u);  // UiMovie::Play
    g.Call(sub_821C3180);  // Options::SetUpCategoryPage
    *started = 1;
  }
  if (g.Call(sub_8211E378, movie, 0u) == 0) {  // UiMovie::Run
    ShowPage(Pages(kScreenPages), 0, 5, -1);
    Pages(kScreenPages)->unknown_24 = 0;
    game::DestroySaveBuffers(g);
  }
  return 0;
}

}  // namespace

#define SBX_OPTIONS_LOAD_SAVE_FALLBACK(name) \
  REX_FUNC_PROLOGUE();                       \
  if (!On()) {                               \
    __imp__##name(ctx, base);                \
    return;                                  \
  }                                          \
  Guest g(ctx, base);

// LoadPage::SetDialog and SavePage::SetDialog: r3 the kind, r4 the dialog.
extern "C" REX_FUNC(sub_821B9D88) {
  SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_821B9D88)
  SetDialog(kLoadPage, int32_t(ctx.r3.u32), ctx.r4.u32);
}

extern "C" REX_FUNC(sub_821B9C38) {
  SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_821B9C38)
  SetDialog(kSavePage, int32_t(ctx.r3.u32), ctx.r4.u32);
}

// The sub-pages (each page's table entries; r3 isn't read).
#define SBX_SUB_PAGE(address, call)                \
  extern "C" REX_FUNC(sub_##address) {             \
    SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_##address)  \
    call;                                          \
  }

SBX_SUB_PAGE(821BA160, CheckDevice(g, kLoadPage))  // LoadPage::CheckDevice
SBX_SUB_PAGE(821BACF8, LoadAnswer(g))              // LoadPage::Answer
SBX_SUB_PAGE(821BAE88, LoadNote(g))                // LoadPage::Note
SBX_SUB_PAGE(821B8458, ClosePage(kLoadPage))       // LoadPage::Close
SBX_SUB_PAGE(821BA1B8, LoadDeviceAnswer(g))        // LoadPage::DeviceAnswer
SBX_SUB_PAGE(821BA210, CheckDevice(g, kSavePage))  // SavePage::CheckDevice
SBX_SUB_PAGE(821BAFB8, SaveAnswer(g))              // SavePage::Answer
SBX_SUB_PAGE(821BB168, SaveNote(g))                // SavePage::Note
SBX_SUB_PAGE(821BA268, SaveWait(g))                // SavePage::Wait
SBX_SUB_PAGE(821B8998, ClosePage(kSavePage))       // SavePage::Close

#undef SBX_SUB_PAGE

// OptionsSavePages::Cancel (sub-page 5 of both pages): the options screen
// told a page closes.
extern "C" REX_FUNC(sub_821B89E0) {
  SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_821B89E0)
  Options()->page_closing = 1;
}

// LoadPage::Leave and SavePage::Leave: r3 the started word.
extern "C" REX_FUNC(sub_821C4A10) {
  SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_821C4A10)
  ctx.r3.u64 = LeavePage(g, kLoadPage, At<u32_be>(ctx.r3.u32));
}

extern "C" REX_FUNC(sub_821C4AF8) {
  SBX_OPTIONS_LOAD_SAVE_FALLBACK(sub_821C4AF8)
  ctx.r3.u64 = LeavePage(g, kSavePage, At<u32_be>(ctx.r3.u32));
}
