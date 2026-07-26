#ifndef FONT_H
#define FONT_H

typedef struct {
    unsigned short w, h;
    short          left;
    short          top;
    unsigned short advance;
    unsigned int   pixel_offset;
} glyph_t;

typedef struct {
    unsigned short        size;
    unsigned short        ascent;
    unsigned short        descent;
    unsigned short        line_height;
    unsigned short        first;
    unsigned short        last;
    const glyph_t        *glyphs;
    const unsigned char  *pixels;
    unsigned int          packed_size;
    unsigned int          unpacked_size;
} font_t;

extern const font_t jetbrains_font;

#endif
