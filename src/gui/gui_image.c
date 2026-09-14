/* gui_image.c - image and animation loading by file extension */
#include "gui_internal.h"

icon_t* gui_load_image(CHAR16 *path) {
    efi_log(L"  image: opening file");
    efi_log(path);
    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) { efi_log(L"  ERROR: image file not found or unreadable"); return NULL; }
    { CHAR16 d[64]; SPrint(d, sizeof(d), L"  image: read %d bytes", (int)buf->size); efi_log(d); }

    UINT8 *data = (UINT8*)buf->data;
    if (buf->size < 2) {
        efi_log(L"  ERROR: image file is too small");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT8 png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    INTN is_png = 1;
    if (buf->size < sizeof(png_sig)) is_png = 0;
    for (int i = 0; is_png && i < 8; i++) {
        if (data[i] != png_sig[i]) {
            is_png = 0;
            break;
        }
    }

    if (is_png) {
        icon_t *icon = png_load(data, buf->size);
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        if (!icon) efi_log(L"  ERROR: PNG decode failed");
        return icon;
    }

    if (data[0] != 'B' || data[1] != 'M') {
        efi_log(L"  ERROR: image is neither PNG nor BMP");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    if (buf->size < 54) {
        efi_log(L"  ERROR: BMP header is truncated");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT32 width = rd32le(data + 18);
    UINT32 height = rd32le(data + 22);
    UINT16 bpp = rd16le(data + 28);
    UINT32 compression = rd32le(data + 30);
    UINT32 data_offset = rd32le(data + 10);

    if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
        (UINT64)width * height > 16u * 1024u * 1024u ||
        compression != 0 || (bpp != 24 && bpp != 32) || data_offset >= buf->size) {
        efi_log(L"  ERROR: unsupported or invalid BMP");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINTN bytes_per_px = bpp / 8;
    UINTN row_raw = 0, row_stride = 0, pixel_array_bytes = 0;
    UINTN pixel_array_end = 0, pixel_count = 0, pixel_bytes = 0;
    if (mul_overflow_uintn((UINTN)width, bytes_per_px, &row_raw) ||
        add_overflow_uintn(row_raw, 3, &row_stride) ||
        mul_overflow_uintn((UINTN)height, row_stride & ~(UINTN)3, &pixel_array_bytes) ||
        add_overflow_uintn((UINTN)data_offset, pixel_array_bytes, &pixel_array_end) ||
        pixel_array_end > buf->size ||
        mul_overflow_uintn((UINTN)width, (UINTN)height, &pixel_count) ||
        mul_overflow_uintn(pixel_count, sizeof(UINT32), &pixel_bytes)) {
        efi_log(L"  ERROR: BMP dimensions or pixel data are invalid");
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }
    row_stride &= ~(UINTN)3;

    icon_t *icon = efi_allocate_pool(sizeof(icon_t));
    if (!icon) {
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }
    icon->width = width;
    icon->height = height;
    icon->scaled_size = 0;
    icon->scaled = NULL;
    icon->pixels = efi_allocate_pool(pixel_bytes);
    if (!icon->pixels) {
        efi_free_pool(icon);
        efi_free_pool(buf->data);
        efi_free_pool(buf);
        return NULL;
    }

    UINT8 *src = data + data_offset;

    for (UINTN y = 0; y < height; y++) {
        UINT8 *row = src + ((UINTN)height - 1 - y) * row_stride;
        for (UINTN x = 0; x < width; x++) {
            UINT8 *pixel = row + x * bytes_per_px;

            icon->pixels[y * width + x] = (0xFF << 24) | (pixel[2] << 16) | (pixel[1] << 8) | pixel[0];
        }
    }

    efi_free_pool(buf->data);
    efi_free_pool(buf);
    return icon;
}

icon_t* gui_load_icon(CHAR16 *path) {
    return gui_load_image(path);
}

static int path_ext_is(CHAR16 *path, const char *ext) {
    if (!path) return 0;
    UINTN n = 0;
    while (path[n]) n++;
    UINTN el = 0;
    while (ext[el]) el++;
    if (n < el + 1) return 0;
    for (UINTN i = 0; i < el; i++) {
        CHAR16 c = path[n - el + i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != (CHAR16)ext[i]) return 0;
    }
    return path[n - el - 1] == '.';
}

int path_anim_kind(CHAR16 *path) {
    if (path_ext_is(path, "gif")) return 1;
    if (path_ext_is(path, "mp4") || path_ext_is(path, "mov") ||
        path_ext_is(path, "m4v")) return 2;
    if (path_ext_is(path, "vbg")) return 3;
    return 0;
}

anim_t* gui_load_anim(CHAR16 *path, icon_t **first_out, UINTN sw, UINTN sh) {
    if (first_out) *first_out = NULL;

    efi_file_buffer_t *buf = efi_load_file(path);
    if (!buf) return NULL;

    UINT8 *data = (UINT8*)buf->data;
    UINTN size = buf->size;
    if (!data || !size) {
        if (data) efi_free_pool(data);
        efi_free_pool(buf);
        return NULL;
    }

    anim_t *a = NULL;
    int taken = 0;
    if (size >= 8 && data[0] == 'V' && data[1] == 'I' &&
        data[2] == 'S' && data[3] == 'O' && data[4] == 'R' &&
        data[5] == 'V' && data[6] == 'B' && data[7] == 'G') {
        a = vbg_load(data, size);
        taken = 1;
    } else if (size >= 8 && data[4] == 'f' && data[5] == 't' &&
               data[6] == 'y' && data[7] == 'p') {
        a = mp4_load(data, size, sw, sh);
        taken = 1;
    } else if (size >= 6 && data[0] == 'G' && data[1] == 'I' &&
               data[2] == 'F') {
        a = gif_load(data, size);
        taken = 1;
    }
    if (!taken) efi_free_pool(data);
    efi_free_pool(buf);
    if (!a) return NULL;

    if (first_out) {
        UINTN px = a->width * a->height;
        icon_t *ic = efi_allocate_pool(sizeof(icon_t));
        if (ic) {
            ic->width = a->width;
            ic->height = a->height;
            ic->scaled_size = 0;
            ic->scaled = NULL;
            ic->pixels = efi_allocate_pool(px * sizeof(UINT32));
            if (ic->pixels) {
                for (UINTN i = 0; i < px; i++)
                    ic->pixels[i] = a->canvas[i];
                *first_out = ic;
            } else {
                efi_free_pool(ic);
            }
        }
    }

    return a;
}
