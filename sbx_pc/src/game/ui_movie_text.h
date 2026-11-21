// A UI movie's queued text draw: the font style a queued text is drawn in,
// and ui_movie_text.cpp's functions the other movie files call. The texts'
// records and nodes are in ui_movie_nodes.h, the movie in ui_movies.h.

#pragma once

#include "game/guest_types.h"
#include "game/strings.h"
#include "game/ui_movies.h"

namespace game {

using guest::u32_be;

// A queued text's draw as UiMovie::DrawQueuedText builds it: the font's
// style, then the text's id (TickerText::Offset reads it).
struct UiMovieTextDraw {
  TextStyle style;
  u32_be text_id;
};
GUEST_OFFSET(UiMovieTextDraw, text_id, 120);

// UiMovie::DrawQueuedText (sub_8211CF58): the movie's queued texts drawn
// and taken off, last first.
void DrawQueuedText(guest::Guest& g, UiMovie* movie, MovieTextList* list, uint32_t draw_only);

}  // namespace game
