
#include "gui.h"
#include "efi_helpers.h"
#include <efi.h>
#include <efilib.h>

extern EFI_BOOT_SERVICES *BS;

typedef struct {
    UINT8 *input;
    UINTN input_size;
    UINTN bit_pos;
} bit_reader_t;

#define MAX_CODE_LENGTH 16
#define LITERALS  288
#define DISTANCES  32

static const UINT16 length_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

static const UINT8 length_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

static const UINT16 dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};

static const UINT8 dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

static UINT32 read_bits(bit_reader_t *br, UINTN count) {
    UINT32 result = 0;
    for (UINTN i = 0; i < count; i++) {
        UINTN byte_idx = br->bit_pos / 8;
        UINTN bit_idx  = br->bit_pos % 8;
        if (byte_idx < br->input_size) {
            result |= ((br->input[byte_idx] >> bit_idx) & 1) << i;
        }
        br->bit_pos++;
    }
    return result;
}

#define MAXBITS 15

typedef struct {
    INT16 count[MAXBITS + 1];
    INT16 symbol[288];
} huff_t;

static INTN huff_construct(huff_t *h, const UINT8 *length, INTN n) {
    INTN symbol, len, left;
    INT16 offs[MAXBITS + 1];

    for (len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
    if (h->count[0] == n) return 0;

    left = 1;
    for (len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }

    offs[1] = 0;
    for (len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (symbol = 0; symbol < n; symbol++)
        if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (INT16)symbol;

    return left;
}

static INTN huff_decode(bit_reader_t *br, const huff_t *h) {
    INTN code = 0, first = 0, index = 0, len;
    for (len = 1; len <= MAXBITS; len++) {
        code |= (INTN)read_bits(br, 1);
        INTN count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static INTN br_exhausted(bit_reader_t *br) {
    return br->bit_pos >= br->input_size * 8;
}

static EFI_STATUS png_decompress(UINT8 *input, UINTN input_size,
                                  UINT8 *output, UINTN *output_size) {
    bit_reader_t br = {input, input_size, 0};
    UINTN out_pos = 0;
    UINT8 bfinal  = 0;

    UINT8 *window = efi_allocate_pool(32768);
    if (!window) return EFI_OUT_OF_RESOURCES;
    UINTN window_pos = 0;
    UINTN max_output = *output_size;
    EFI_STATUS status = EFI_SUCCESS;

    do {

        if (br_exhausted(&br)) { status = EFI_INVALID_PARAMETER; break; }

        bfinal = read_bits(&br, 1);
        UINT8 btype = read_bits(&br, 2);

        if (btype == 0) {

            br.bit_pos = (br.bit_pos + 7) & ~7u;
            UINTN byte = br.bit_pos / 8;
            if (byte + 4 > input_size) { status = EFI_INVALID_PARAMETER; break; }
            UINTN len = input[byte] | (input[byte + 1] << 8);
            br.bit_pos += 32;
            byte += 4;
            if (byte + len > input_size) { status = EFI_INVALID_PARAMETER; break; }

            for (UINTN i = 0; i < len && out_pos < max_output; i++) {
                UINT8 b = input[byte + i];
                output[out_pos++] = b;
                window[window_pos++] = b;
                window_pos &= 0x7FFF;
            }
            br.bit_pos += len * 8;
        }
        else if (btype == 1 || btype == 2) {
            huff_t lit_h, dist_h;

            if (btype == 1) {

                UINT8 lit_len[288];
                UINT8 dist_len[30];
                for (INTN i = 0; i < 288; i++) {
                    if      (i <= 143) lit_len[i] = 8;
                    else if (i <= 255) lit_len[i] = 9;
                    else if (i <= 279) lit_len[i] = 7;
                    else               lit_len[i] = 8;
                }
                for (INTN i = 0; i < 30; i++) dist_len[i] = 5;
                huff_construct(&lit_h,  lit_len, 288);
                huff_construct(&dist_h, dist_len, 30);
            } else {

                UINTN hlit  = read_bits(&br, 5) + 257;
                UINTN hdist = read_bits(&br, 5) + 1;
                UINTN hclen = read_bits(&br, 4) + 4;

                static const UINT8 code_length_order[19] = {
                    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
                };

                UINT8 cl_lens[19] = {0};
                for (UINTN i = 0; i < hclen; i++)
                    cl_lens[code_length_order[i]] = (UINT8)read_bits(&br, 3);

                huff_t cl_h;
                if (huff_construct(&cl_h, cl_lens, 19) < 0) { status = EFI_INVALID_PARAMETER; break; }

                UINT8 all_lens[288 + 32] = {0};
                UINTN total = hlit + hdist;
                UINTN i = 0;
                while (i < total) {
                    if (br_exhausted(&br)) { status = EFI_INVALID_PARAMETER; break; }
                    INTN sym = huff_decode(&br, &cl_h);
                    if (sym < 0) { status = EFI_INVALID_PARAMETER; break; }

                    if (sym < 16) {
                        all_lens[i++] = (UINT8)sym;
                    } else if (sym == 16) {
                        UINTN rep = read_bits(&br, 2) + 3;
                        UINT8 last = i > 0 ? all_lens[i-1] : 0;
                        for (UINTN r = 0; r < rep && i < total; r++) all_lens[i++] = last;
                    } else if (sym == 17) {
                        UINTN rep = read_bits(&br, 3) + 3;
                        for (UINTN r = 0; r < rep && i < total; r++) all_lens[i++] = 0;
                    } else {
                        UINTN rep = read_bits(&br, 7) + 11;
                        for (UINTN r = 0; r < rep && i < total; r++) all_lens[i++] = 0;
                    }
                }
                if (status != EFI_SUCCESS) break;

                if (huff_construct(&lit_h, all_lens, (INTN)hlit) < 0) { status = EFI_INVALID_PARAMETER; break; }

                if (huff_construct(&dist_h, all_lens + hlit, (INTN)hdist) < 0) { status = EFI_INVALID_PARAMETER; break; }
            }

            while (1) {
                INTN symbol = huff_decode(&br, &lit_h);

                if (symbol < 0) { status = EFI_INVALID_PARAMETER; break; }
                if (symbol < 256) {
                    if (out_pos < max_output) {
                        output[out_pos++] = (UINT8)symbol;
                        window[window_pos++] = (UINT8)symbol;
                        window_pos &= 0x7FFF;
                    } else break;
                } else if (symbol == 256) {
                    break;
                } else if (symbol > 285) {
                    status = EFI_INVALID_PARAMETER; break;
                } else {
                    INTN  length_idx = symbol - 257;
                    UINTN length     = length_base[length_idx];
                    length += read_bits(&br, length_extra[length_idx]);

                    INTN  dist_symbol = huff_decode(&br, &dist_h);
                    if (dist_symbol < 0 || dist_symbol > 29) { status = EFI_INVALID_PARAMETER; break; }
                    UINTN dist        = dist_base[dist_symbol];
                    dist += read_bits(&br, dist_extra[dist_symbol]);

                    for (UINTN k = 0; k < length && out_pos < max_output; k++) {
                        UINTN src_pos = (window_pos - dist) & 0x7FFF;
                        UINT8 b = window[src_pos];
                        output[out_pos++] = b;
                        window[window_pos++] = b;
                        window_pos &= 0x7FFF;
                    }
                }
                if (br_exhausted(&br) && out_pos < max_output) { status = EFI_INVALID_PARAMETER; break; }
            }
            if (status != EFI_SUCCESS) break;
        } else {
            status = EFI_INVALID_PARAMETER;
            break;
        }
    } while (!bfinal && status == EFI_SUCCESS);

    efi_free_pool(window);
    *output_size = out_pos;
    return status;
}

static UINT32 crc32_table[256];
static INTN   crc32_init = 0;

static void init_crc32(void) {
    if (crc32_init) return;
    for (UINTN i = 0; i < 256; i++) {
        UINT32 crc = (UINT32)i;
        for (UINTN j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
        }
        crc32_table[i] = crc;
    }
    crc32_init = 1;
}

static UINT32 png_crc(UINT8 *data, UINTN len) {
    init_crc32();
    UINT32 crc = 0xFFFFFFFFu;
    for (UINTN i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static void paeth_predictor(INTN a, INTN b, INTN c, INTN *p) {
    INTN pp = a + b - c;
    INTN pa = (pp > a) ? (pp - a) : (a - pp);
    INTN pb = (pp > b) ? (pp - b) : (b - pp);
    INTN pc = (pp > c) ? (pp - c) : (c - pp);
    if (pa <= pb && pa <= pc) *p = a;
    else if (pb <= pc)        *p = b;
    else                      *p = c;
}

static void apply_filter(UINT8 filter, UINT8 *row, UINT8 *prev, UINTN bpp, UINTN width) {
    for (UINTN x = 0; x < width; x++) {
        for (UINTN b = 0; b < bpp; b++) {
            UINTN idx     = x * bpp + b;
            UINT8 raw     = row[idx];
            UINT8 left    = (x > 0)               ? row[(x - 1) * bpp + b]       : 0;
            UINT8 up      = prev                   ? prev[idx]                     : 0;
            UINT8 up_left = (x > 0 && prev)        ? prev[(x - 1) * bpp + b]      : 0;

            switch (filter) {
                case 0: row[idx] = raw;            break;
                case 1: row[idx] = raw + left;     break;
                case 2: row[idx] = raw + up;       break;
                case 3: row[idx] = raw + (left + up) / 2; break;
                case 4: {
                    INTN pred;
                    paeth_predictor(left, up, up_left, &pred);
                    row[idx] = raw + (UINT8)pred;
                    break;
                }
            }
        }
    }
}

icon_t* png_load(UINT8 *data, UINTN size) {
    efi_log(L"  png: decoding");
    UINT8 png_sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    for (INTN i = 0; i < 8; i++) {
        if (i >= (INTN)size || data[i] != png_sig[i]) {
            efi_log(L"  ERROR: bad PNG signature");
            return NULL;
        }
    }

    UINTN  offset     = 8;
    UINT32 width      = 0, height = 0;
    UINT8  bit_depth  = 0, color_type = 0;
    UINT8 *idat_data  = NULL;
    UINTN  idat_size  = 0;

    while (offset + 12 <= size) {
        UINT32 chunk_len = ((UINT32)data[offset]     << 24) |
                           ((UINT32)data[offset + 1] << 16) |
                           ((UINT32)data[offset + 2] <<  8) |
                                    data[offset + 3];

        if (offset + 12 + chunk_len > size) break;

        UINT32 stored_crc = ((UINT32)data[offset + 8 + chunk_len]     << 24) |
                            ((UINT32)data[offset + 8 + chunk_len + 1] << 16) |
                            ((UINT32)data[offset + 8 + chunk_len + 2] <<  8) |
                                     data[offset + 8 + chunk_len + 3];
        UINT32 calc_crc = png_crc(data + offset + 4, 4 + chunk_len);
        if (stored_crc != calc_crc) {
            efi_log(L"  ERROR: PNG chunk CRC mismatch - file is corrupt");
            if (idat_data) efi_free_pool(idat_data);
            return NULL;
        }

        char type[5] = {0};
        for (INTN i = 0; i < 4; i++) type[i] = (char)data[offset + 4 + i];

        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            width      = ((UINT32)data[offset + 8]  << 24) | ((UINT32)data[offset + 9]  << 16) |
                         ((UINT32)data[offset + 10] <<  8) |          data[offset + 11];
            height     = ((UINT32)data[offset + 12] << 24) | ((UINT32)data[offset + 13] << 16) |
                         ((UINT32)data[offset + 14] <<  8) |          data[offset + 15];
            bit_depth  = data[offset + 16];
            color_type = data[offset + 17];
        }
        else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            UINT8 *new_idat = efi_allocate_pool(idat_size + chunk_len);
            if (!new_idat) break;
            if (idat_data && idat_size > 0) {
                for (UINTN i = 0; i < idat_size; i++) new_idat[i] = idat_data[i];
            }
            for (UINTN i = 0; i < chunk_len; i++) {
                new_idat[idat_size + i] = data[offset + 8 + i];
            }
            if (idat_data) efi_free_pool(idat_data);
            idat_data  = new_idat;
            idat_size += chunk_len;
        }
        else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }

        offset += 12 + chunk_len;
    }

    { CHAR16 d[96]; SPrint(d, sizeof(d),
        L"  png: %dx%d colortype=%d depth=%d idat=%d bytes",
        (int)width, (int)height, (int)color_type, (int)bit_depth, (int)idat_size);
      efi_log(d); }

    if (width == 0 || height == 0 || !idat_data || idat_size < 2) {
        efi_log(L"  ERROR: PNG has no IHDR/IDAT or zero dimensions");
        if (idat_data) efi_free_pool(idat_data);
        return NULL;
    }

    UINTN bpp;
    switch (color_type) {
        case 0: bpp = 1; break;
        case 2: bpp = 3; break;
        case 3: bpp = 1; break;
        case 4: bpp = 2; break;
        case 6: bpp = 4; break;
        default: bpp = 4;
    }

    UINTN  uncomp_size = height * (1 + width * bpp);
    UINT8 *uncomp      = efi_allocate_pool(uncomp_size);
    if (!uncomp) {
        { CHAR16 d[96]; SPrint(d, sizeof(d),
            L"  ERROR: out of memory for %d KB scratch buffer",
            (int)(uncomp_size / 1024)); efi_log(d); }
        return NULL;
    }
    UINTN  final_size  = uncomp_size;

    EFI_STATUS status = png_decompress(idat_data + 2, idat_size - 2,
                                       uncomp, &final_size);
    efi_free_pool(idat_data);

    if (EFI_ERROR(status)) {
        efi_log(L"  ERROR: DEFLATE decompression failed (corrupt/unsupported)");
        efi_free_pool(uncomp);
        return NULL;
    }
    efi_log(L"  png: decoded ok");

    icon_t *icon = efi_allocate_pool(sizeof(icon_t));
    if (!icon) { efi_free_pool(uncomp); return NULL; }
    icon->width    = width;
    icon->height   = height;
    icon->scaled_size = 0;
    icon->scaled   = NULL;
    icon->pixels   = efi_allocate_pool(width * height * sizeof(UINT32));
    if (!icon->pixels) { efi_free_pool(icon); efi_free_pool(uncomp); return NULL; }

    UINTN row_bytes;
    if      (color_type == 2) row_bytes = width * 3;
    else if (color_type == 6) row_bytes = width * 4;
    else if (color_type == 4) row_bytes = width * 2;
    else                      row_bytes = (width * bit_depth * bpp + 7) / 8;
    row_bytes += 1;

    UINT8 *prev_row = NULL;

    for (UINTN y = 0; y < height; y++) {
        UINT8 *row        = uncomp + y * row_bytes;
        UINT8  filter     = row[0];
        UINT8 *pixel_data = row + 1;

        if (filter != 0) {
            apply_filter(filter, pixel_data, prev_row, bpp, width);
        }

        for (UINTN x = 0; x < width; x++) {
            UINT8 r, g, b, a = 255;
            UINTN idx = x * bpp;

            if (color_type == 2) {
                r = pixel_data[idx]; g = pixel_data[idx+1]; b = pixel_data[idx+2];
            } else if (color_type == 6) {
                r = pixel_data[idx]; g = pixel_data[idx+1];
                b = pixel_data[idx+2]; a = pixel_data[idx+3];
            } else if (color_type == 4) {
                UINT8 gray = pixel_data[idx]; a = pixel_data[idx+1];
                r = g = b = gray;
            } else if (color_type == 0) {
                r = (bit_depth == 16) ? pixel_data[idx * 2] : pixel_data[idx];
                g = b = r;
            } else {
                r = g = b = pixel_data[idx];
            }

            icon->pixels[y * width + x] = ((UINT32)a << 24) | ((UINT32)r << 16) |
                                           ((UINT32)g <<  8) | b;
        }

        prev_row = pixel_data;
    }

    efi_free_pool(uncomp);
    return icon;
}
