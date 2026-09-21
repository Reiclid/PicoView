// Letters on the screen.
//
// DirectWrite does this on Windows and there is nothing like it here, so the
// pieces are assembled by hand: fontconfig finds a font file, FreeType turns a
// character into a small grey bitmap, and those bitmaps are cached and stamped
// onto the canvas.
//
// What is deliberately missing is shaping. HarfBuzz is what turns a string into
// positioned glyphs when a script needs it - Arabic, Devanagari, ligatures.
// Latin and Cyrillic at a file name's worth of length do not, so this walks the
// string a character at a time and advances. When the day comes that this has
// to show an Arabic file name, HarfBuzz goes in here and nothing else changes.
#pragma once

#include "paint.h"

namespace pv {

struct Font;

// `family` is a fontconfig pattern - "Inter", "DejaVu Sans", or nullptr for
// whatever the system calls sans-serif. Never returns null: with no font at
// all it returns a handle that measures as zero and draws nothing, because an
// interface with no text is better than no interface.
Font* fontOpen(const char* family, float pixelSize, bool bold);
void  fontClose(Font*);

float textWidth(Font*, const char* utf8);
float lineHeight(Font*);
float ascent(Font*);

// `x`, `y` is the left end of the baseline.
void drawText(Canvas&, Font*, const char* utf8, float x, float y, Color);

enum class Align { Left, Centre, Right };
// Inside `box`, vertically centred. Cut with an ellipsis when it will not fit.
void drawTextIn(Canvas&, Font*, const char* utf8, Rect box, Color, Align = Align::Left);

}  // namespace pv
