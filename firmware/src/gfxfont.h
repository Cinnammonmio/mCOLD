// The Adafruit GFX font format, so fonts generated for the bench test
// (bench/epd29-s3/tools/genfont.py) compile here unchanged.
//
// A glyph is a run of bits, MSB first, `width` x `height`, placed at
// (cursor + xOffset, baseline + yOffset), after which the cursor moves
// on by xAdvance. Field order and types must stay exactly as Adafruit
// has them: the generated tables are positional initialisers.
#pragma once

#include <stdint.h>

#ifndef PROGMEM
#define PROGMEM   // flash is directly addressable on the ESP32
#endif

typedef struct {
  uint16_t bitmapOffset;
  uint8_t width;
  uint8_t height;
  uint8_t xAdvance;
  int8_t xOffset;
  int8_t yOffset;
} GFXglyph;

typedef struct {
  uint8_t *bitmap;
  GFXglyph *glyph;
  uint16_t first;
  uint16_t last;
  uint8_t yAdvance;
} GFXfont;
