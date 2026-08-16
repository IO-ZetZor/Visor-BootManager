#ifndef ANIM_H
#define ANIM_H


typedef struct {
    UINTN    width;
    UINTN    height;
    UINTN    frame_count;
    UINTN    loops;

    UINT8   *data;
    UINTN    size;
    UINTN    first_frame_off;

    UINT32  *canvas;
    UINT32  *restore;
    UINT8   *plane;

    UINT16  *lzw_prefix;
    UINT8   *lzw_suffix;
    UINT8   *lzw_stack;

    UINT8    gct[256 * 3];
    UINTN    gct_n;

    UINTN    next_off;
    UINTN    cur;
    UINTN    loops_done;
    UINT64   next_ms;
    UINT16   cur_delay;

    int      pending_disposal;
    UINTN    pending_x, pending_y, pending_w, pending_h;

    UINTN   *xmap;
    UINTN   *ymap;
    UINTN    map_w;
    UINTN    map_h;
} anim_t;

#endif

