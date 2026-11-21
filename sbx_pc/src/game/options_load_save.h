// The options screen's load and save pages (pages 9 and 10): the screen's
// own words they use and the dialogs they show.

#pragma once

#include "game/guest_types.h"

namespace game {

using guest::i32_be;
using guest::u32_be;

// The options screen ([0x826C7F20]), as far as its load and save pages use
// it: each page's dialog (0 the question, 1 a note, 2 the wait), its kind
// (which question or note) and the ticks it has been shown; and the flag
// set when a page closes.
struct OptionsScreen {
  uint8_t unknown_0[672];
  i32_be load_dialog;
  i32_be load_kind;
  i32_be load_ticks;
  uint8_t unknown_684[8];
  i32_be save_dialog;
  i32_be save_kind;
  i32_be save_ticks;
  uint8_t unknown_704[56];
  u32_be page_closing;
};
GUEST_OFFSET(OptionsScreen, load_dialog, 672);
GUEST_OFFSET(OptionsScreen, save_dialog, 692);
GUEST_OFFSET(OptionsScreen, page_closing, 760);

}  // namespace game
