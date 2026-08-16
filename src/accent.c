#include "accent.h"
#include "efi_helpers.h"

#ifdef GUI_ACCENT_ROLES
_Static_assert(GUI_ACCENT_ROLES == ACCENT_ROLE_COUNT,
               "gui.h GUI_ACCENT_ROLES must match accent.h ACCENT_ROLE_COUNT");
#endif

static const INT32 SRGB2LIN[256] = {
         0,     20,     40,     60,     80,     99,    119,    139,
       159,    179,    199,    219,    241,    264,    288,    313,
       340,    367,    396,    427,    458,    491,    526,    562,
       599,    637,    677,    718,    761,    805,    851,    898,
       947,    997,   1048,   1101,   1156,   1212,   1270,   1330,
      1391,   1453,   1517,   1583,   1651,   1720,   1791,   1863,
      1937,   2013,   2090,   2170,   2250,   2333,   2418,   2504,
      2592,   2681,   2773,   2866,   2961,   3058,   3157,   3258,
      3360,   3464,   3570,   3678,   3788,   3900,   4014,   4129,
      4247,   4366,   4488,   4611,   4736,   4864,   4993,   5124,
      5257,   5392,   5530,   5669,   5810,   5953,   6099,   6246,
      6395,   6547,   6701,   6856,   7014,   7174,   7336,   7500,
      7666,   7834,   8004,   8177,   8352,   8529,   8708,   8889,
      9072,   9258,   9446,   9636,   9828,  10022,  10219,  10418,
     10619,  10822,  11028,  11236,  11446,  11658,  11873,  12090,
     12309,  12531,  12754,  12981,  13209,  13440,  13673,  13909,
     14147,  14387,  14629,  14874,  15122,  15372,  15624,  15878,
     16135,  16394,  16656,  16920,  17187,  17456,  17727,  18001,
     18278,  18556,  18838,  19121,  19408,  19696,  19988,  20281,
     20578,  20876,  21178,  21481,  21788,  22096,  22408,  22722,
     23038,  23357,  23679,  24003,  24329,  24659,  24991,  25325,
     25662,  26002,  26344,  26689,  27036,  27387,  27739,  28095,
     28453,  28813,  29177,  29543,  29911,  30283,  30657,  31033,
     31413,  31795,  32180,  32567,  32957,  33350,  33746,  34144,
     34545,  34949,  35355,  35765,  36177,  36591,  37009,  37429,
     37852,  38278,  38707,  39138,  39572,  40009,  40449,  40892,
     41337,  41786,  42237,  42691,  43147,  43607,  44069,  44534,
     45003,  45474,  45947,  46424,  46904,  47386,  47871,  48360,
     48851,  49345,  49842,  50342,  50844,  51350,  51859,  52370,
     52884,  53402,  53922,  54445,  54972,  55501,  56033,  56568,
     57106,  57647,  58191,  58738,  59288,  59841,  60397,  60956,
     61518,  62083,  62651,  63222,  63796,  64373,  64953,  65536,
};

static const INT16 SIN_Q15[91] = {
        0,   572,  1144,  1715,  2286,  2856,  3425,
     3993,  4560,  5126,  5690,  6252,  6813,  7371,
     7927,  8481,  9032,  9580, 10126, 10668, 11207,
    11743, 12275, 12803, 13328, 13848, 14365, 14876,
    15384, 15886, 16384, 16877, 17364, 17847, 18324,
    18795, 19261, 19720, 20174, 20622, 21063, 21498,
    21926, 22348, 22763, 23170, 23571, 23965, 24351,
    24730, 25102, 25466, 25822, 26170, 26510, 26842,
    27166, 27482, 27789, 28088, 28378, 28660, 28932,
    29197, 29452, 29698, 29935, 30163, 30382, 30592,
    30792, 30983, 31164, 31336, 31499, 31651, 31795,
    31928, 32052, 32166, 32270, 32365, 32449, 32524,
    32588, 32643, 32688, 32723, 32748, 32763, 32767,
};

static const INT32 TAN_Q16[46] = {
        0,  1144,  2289,  3435,  4583,  5734,  6888,  8047,
     9210, 10380, 11556, 12739, 13930, 15130, 16340, 17560,
    18792, 20036, 21294, 22566, 23853, 25157, 26478, 27818,
    29179, 30560, 31964, 33392, 34846, 36327, 37837, 39378,
    40951, 42560, 44205, 45889, 47615, 49385, 51202, 53070,
    54991, 56970, 59009, 61113, 63287, 65536,
};

static const INT32 OK_INV2[9] = { 65536, 25974, 14143, 65536, -6918, -4185, 65536, -5864, -84639 };
static const INT32 OK_INV1[9] = { 267173, -216774, 15137, -83128, 171033, -22369, -275, -46099, 111910 };

static const INT8 HCT_HUE_OFF[36] = {
     0, -1, -3, -2, -2, -1, -1,  0,  1,  2,  2,  2,
     2,  2,  2,  3,  3,  3,  2,  2,  1,  1,  1,  1,
     2,  3,  8,  7,  5,  5,  5,  5,  5,  4,  3,  1,
};
static const INT16 HCT_C_RATIO[36][11] = {
    {451, 436, 422, 409, 398, 385, 365, 355, 349, 347, 347},
    {454, 442, 431, 419, 408, 391, 360, 345, 337, 325, 325},
    {459, 451, 446, 435, 426, 403, 354, 336, 323, 310, 310},
    {447, 439, 429, 420, 410, 395, 350, 327, 313, 297, 297},
    {426, 405, 399, 390, 383, 374, 353, 319, 303, 284, 284},
    {423, 392, 383, 376, 364, 359, 349, 317, 298, 288, 288},
    {391, 389, 381, 370, 360, 353, 342, 322, 297, 286, 286},
    {399, 387, 370, 368, 354, 348, 341, 332, 301, 291, 291},
    {382, 385, 379, 367, 356, 349, 343, 338, 317, 300, 300},
    {406, 394, 386, 372, 366, 357, 349, 343, 334, 313, 313},
    {416, 410, 394, 382, 373, 367, 360, 352, 346, 336, 336},
    {438, 413, 405, 395, 385, 377, 367, 362, 355, 348, 348},
    {442, 429, 418, 404, 395, 386, 377, 370, 363, 356, 356},
    {450, 436, 422, 411, 400, 390, 382, 374, 367, 360, 360},
    {449, 434, 421, 409, 397, 388, 379, 371, 364, 370, 370},
    {463, 444, 425, 411, 399, 389, 380, 371, 364, 384, 384},
    {473, 453, 440, 424, 412, 401, 391, 382, 374, 401, 401},
    {512, 468, 449, 436, 426, 412, 401, 392, 386, 411, 411},
    {502, 477, 462, 448, 434, 420, 411, 401, 393, 412, 412},
    {520, 484, 465, 452, 438, 424, 414, 405, 397, 417, 417},
    {495, 502, 468, 448, 437, 427, 416, 406, 407, 474, 474},
    {513, 479, 467, 446, 431, 419, 411, 401, 424, 518, 518},
    {482, 491, 455, 440, 422, 412, 404, 398, 463, 526, 526},
    {475, 467, 443, 423, 406, 397, 388, 407, 465, 558, 558},
    {458, 436, 416, 400, 388, 376, 377, 419, 473, 530, 530},
    {414, 402, 383, 367, 355, 355, 385, 425, 471, 559, 559},
    {350, 324, 307, 308, 327, 356, 391, 425, 469, 534, 534},
    {353, 332, 316, 310, 335, 362, 387, 418, 451, 516, 516},
    {371, 344, 329, 317, 336, 357, 382, 408, 441, 493, 493},
    {381, 356, 340, 326, 330, 354, 372, 397, 430, 477, 477},
    {389, 365, 348, 336, 329, 345, 366, 385, 414, 445, 445},
    {398, 376, 359, 347, 336, 339, 355, 373, 397, 426, 426},
    {407, 387, 373, 359, 349, 341, 347, 363, 380, 409, 409},
    {420, 404, 387, 374, 363, 354, 355, 363, 376, 391, 391},
    {434, 415, 401, 388, 377, 367, 363, 365, 370, 377, 377},
    {443, 425, 412, 398, 388, 377, 367, 361, 360, 360, 360},
};

static INT32 c_ratio(INT32 h_ok, INT32 tone) {
    if (tone < 0) tone = 0;
    if (tone > 100) tone = 100;
    return HCT_C_RATIO[h_ok / 10][tone / 10];
}

static INT32 isin_q15(INT32 deg) {
    deg %= 360; if (deg < 0) deg += 360;
    if (deg <= 90)  return SIN_Q15[deg];
    if (deg <= 180) return SIN_Q15[180 - deg];
    if (deg <= 270) return -SIN_Q15[deg - 180];
    return -SIN_Q15[360 - deg];
}
static INT32 icos_q15(INT32 deg) { return isin_q15(deg + 90); }

static INT32 sanitize_deg(INT32 deg) {
    deg %= 360; if (deg < 0) deg += 360;
    return deg;
}

static INT64 isqrt64(INT64 x) {
    if (x <= 0) return 0;
    INT64 g = x;
    int shift = 1;
    while (g > 1) { g >>= 2; shift++; }
    INT64 r = 1LL << shift, last;
    do { last = r; r = (r + x / r) / 2; } while (r < last);
    return last;
}

static INT64 cbrt_q16(INT64 x) {
    if (x <= 0) return 0;
    INT64 K = x << 32;
    INT64 y = 1 << 16;
    for (int i = 0; i < 48; i++) {
        INT64 y2 = y * y;
        if (y2 == 0) { y = 1; continue; }
        INT64 yn = (2 * y + K / y2) / 3;
        if (yn == y) break;
        if (yn > y ? yn - y : y - yn) { y = yn; } else break;
    }
    return y;
}

static INT64 cube_q16(INT64 v) {
    INT64 s = v < 0 ? -1 : 1; if (v < 0) v = -v;
    INT64 v2 = (v * v) >> 16;
    return s * ((v2 * v) >> 16);
}

static INT32 iatan2_deg(INT32 y, INT32 x) {
    if (x == 0 && y == 0) return 0;
    INT32 ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    INT32 lo_v = ax < ay ? ax : ay, hi_v = ax < ay ? ay : ax;
    INT32 t = (INT32)(((INT64)lo_v << 16) / hi_v);
    INT32 lo = 0, hi = 45;
    while (lo < hi) {
        INT32 mid = (lo + hi) / 2;
        if (TAN_Q16[mid] < t) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && (t - TAN_Q16[lo - 1]) < (TAN_Q16[lo] - t)) lo--;
    INT32 base = (ax >= ay) ? lo : 90 - lo;
    if (x >= 0 && y >= 0) return base;
    if (x <  0 && y >= 0) return 180 - base;
    if (x <  0 && y <  0) return 180 + base;
    return 360 - base;
}

static const INT32 LOG2_TBL[65] = {
    0, 1466, 2909, 4331, 5732, 7112, 8473, 9814,
    11136, 12440, 13727, 14996, 16248, 17484, 18704, 19909,
    21098, 22272, 23433, 24579, 25711, 26830, 27936, 29029,
    30109, 31178, 32234, 33279, 34312, 35334, 36346, 37346,
    38336, 39316, 40286, 41246, 42196, 43137, 44068, 44990,
    45904, 46809, 47705, 48593, 49472, 50344, 51207, 52063,
    52911, 53751, 54584, 55410, 56229, 57040, 57845, 58643,
    59434, 60219, 60997, 61769, 62534, 63294, 64047, 64794,
    65536
};
static const INT32 EXP2_TBL[65] = {
    65536, 66250, 66971, 67700, 68438, 69183, 69936, 70698,
    71468, 72246, 73032, 73828, 74632, 75444, 76266, 77096,
    77936, 78785, 79642, 80510, 81386, 82273, 83169, 84074,
    84990, 85915, 86851, 87796, 88752, 89719, 90696, 91684,
    92682, 93691, 94711, 95743, 96785, 97839, 98905, 99982,
    101070, 102171, 103283, 104408, 105545, 106694, 107856, 109031,
    110218, 111418, 112631, 113858, 115098, 116351, 117618, 118899,
    120194, 121502, 122825, 124163, 125515, 126882, 128263, 129660,
    131072
};

static INT64 log2_q16(INT64 x) {
    INT64 e = 0;
    while (x >= 131072) { x >>= 1; e++; }
    while (x < 65536)   { x <<= 1; e--; }
    INT64 i = (x - 65536) >> 10, f = (x - 65536) & 1023;
    return (e << 16) + LOG2_TBL[i] + (((INT64)(LOG2_TBL[i + 1] - LOG2_TBL[i]) * f) >> 10);
}
static INT64 exp2_q16(INT64 x) {
    INT64 ip = x >> 16;
    INT64 fp = x - (ip << 16);
    INT64 i = fp >> 10, f = fp & 1023;
    INT64 m = EXP2_TBL[i] + (((INT64)(EXP2_TBL[i + 1] - EXP2_TBL[i]) * f) >> 10);
    if (ip >= 0) return ip > 40 ? ((INT64)1 << 56) : (m << ip);
    return -ip > 60 ? 0 : (m >> (-ip));
}
static INT64 pow_q16(INT64 x, INT64 e) {
    if (x <= 0) return 0;
    return exp2_q16((e * log2_q16(x)) >> 16);
}

static INT64 cam16_adapt(INT64 comp) {
    INT64 s = comp < 0 ? -1 : 1;
    if (comp < 0) comp = -comp;
    INT64 t = (25460 * comp) >> 16;
    if (t <= 0) return 0;
    INT64 af = pow_q16(t, 27525);
    return s * ((400 * af << 16) / (af + 1777992));
}

static void cam16_hc(INT32 r8, INT32 g8, INT32 b8, INT32 *hue, INT32 *chroma_q16) {
    INT64 lr = SRGB2LIN[r8 & 0xFF], lg = SRGB2LIN[g8 & 0xFF], lb = SRGB2LIN[b8 & 0xFF];
    INT64 rd = (20258 * lr + 40314 * lg +  4716 * lb) >> 16;
    INT64 gd = ( 9938 * lr + 50248 * lg +  5518 * lb) >> 16;
    INT64 bd = ( 1712 * lr +  9049 * lg + 55636 * lb) >> 16;
    INT64 ra = cam16_adapt(rd), ga = cam16_adapt(gd), ba = cam16_adapt(bd);
    INT64 a = ra + (ba - 12 * ga) / 11;
    INT64 b = (ra + ga - 2 * ba) / 9;
    *hue = sanitize_deg(iatan2_deg((INT32)b, (INT32)a));
    INT64 u  = (20 * ra + 20 * ga + 21 * ba) / 20;
    INT64 p2 = (40 * ra + 20 * ga + ba) / 20;
    INT64 ac = (p2 * 66645) >> 16;
    if (ac <= 0) { *chroma_q16 = 0; return; }
    INT32 hp = (*hue < 21) ? *hue + 360 : *hue;
    INT64 c114 = icos_q15(hp + 114), c115 = icos_q15(hp + 115);
    INT64 ecos = (c114 * 418 + c115 * 606) / 1024;
    INT64 ehue = ((ecos << 1) + 249037) / 4;
    INT64 p1 = (ehue * 256326213) >> 16;
    INT64 hyp = isqrt64(a * a + b * b);
    INT64 t = p1 * hyp / (u + 19988);
    INT64 ratio = (ac << 16) / 1964835;
    INT64 c = (57898 * pow_q16(t, 58982)) >> 16;
    *chroma_q16 = (INT32)((c * pow_q16(ratio, 43166)) >> 16);
}

static INT32 lin_to_srgb8(INT64 lin) {
    if (lin <= 0) return 0;
    if (lin >= 65536) return 255;
    int lo = 0, hi = 255;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (SRGB2LIN[mid] < lin) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && (lin - SRGB2LIN[lo-1]) < (SRGB2LIN[lo] - lin)) return lo - 1;
    return lo;
}

static void oklab_to_linear(INT32 L, INT32 A, INT32 B, INT64 *lr, INT64 *lg, INT64 *lb) {
    INT64 lp = ((INT64)OK_INV2[0]*L + (INT64)OK_INV2[1]*A + (INT64)OK_INV2[2]*B) >> 16;
    INT64 mp = ((INT64)OK_INV2[3]*L + (INT64)OK_INV2[4]*A + (INT64)OK_INV2[5]*B) >> 16;
    INT64 sp = ((INT64)OK_INV2[6]*L + (INT64)OK_INV2[7]*A + (INT64)OK_INV2[8]*B) >> 16;
    INT64 l = cube_q16(lp), m = cube_q16(mp), s = cube_q16(sp);
    *lr = (OK_INV1[0]*l + OK_INV1[1]*m + OK_INV1[2]*s) >> 16;
    *lg = (OK_INV1[3]*l + OK_INV1[4]*m + OK_INV1[5]*s) >> 16;
    *lb = (OK_INV1[6]*l + OK_INV1[7]*m + OK_INV1[8]*s) >> 16;
}

static color_t oklab_to_color(INT32 L, INT32 A, INT32 B) {
    INT64 lr, lg, lb;
    oklab_to_linear(L, A, B, &lr, &lg, &lb);
    color_t o = { (UINT8)lin_to_srgb8(lr), (UINT8)lin_to_srgb8(lg), (UINT8)lin_to_srgb8(lb) };
    return o;
}

static int oklab_in_gamut(INT32 L, INT32 A, INT32 B) {
    INT64 lr, lg, lb;
    oklab_to_linear(L, A, B, &lr, &lg, &lb);
    const INT64 m = 400;
    return lr >= -m && lr <= 65536 + m &&
           lg >= -m && lg <= 65536 + m &&
           lb >= -m && lb <= 65536 + m;
}

static color_t hct_render(INT32 hue_deg, INT32 chroma100, INT32 tone) {
    INT32 L = (tone + 16) * 65536 / 116;
    INT32 h = sanitize_deg(hue_deg - HCT_HUE_OFF[sanitize_deg(hue_deg) / 10]);
    INT32 C = (INT32)(((INT64)chroma100 << 16) / (c_ratio(h, tone) * 100));
    INT32 ca = icos_q15(h), sa = isin_q15(h);
    INT32 A = (INT32)(((INT64)C * ca) >> 15);
    INT32 B = (INT32)(((INT64)C * sa) >> 15);
    if (!oklab_in_gamut(L, A, B)) {
        INT32 lo = 0, hi = 65536, fit = 0;
        for (int i = 0; i < 12; i++) {
            INT32 mid = (lo + hi) / 2;
            INT32 ta = (INT32)(((INT64)A * mid) >> 16);
            INT32 tb = (INT32)(((INT64)B * mid) >> 16);
            if (oklab_in_gamut(L, ta, tb)) { fit = mid; lo = mid + 1; }
            else hi = mid - 1;
        }
        A = (INT32)(((INT64)A * fit) >> 16);
        B = (INT32)(((INT64)B * fit) >> 16);
    }
    return oklab_to_color(L, A, B);
}

#define WU_SIDE   33
#define WU_MAX    128
#define WU_IDX(r, g, b) (((r) * WU_SIDE + (g)) * WU_SIDE + (b))

typedef struct { int r0, r1, g0, g1, b0, b1; INT64 vol; } wu_box_t;

typedef struct {
    INT64 *w, *mr, *mg, *mb, *m2;
} wu_t;

static INT64 wu_vol(const wu_box_t *c, const INT64 *m) {
    return m[WU_IDX(c->r1, c->g1, c->b1)] - m[WU_IDX(c->r1, c->g1, c->b0)]
         - m[WU_IDX(c->r1, c->g0, c->b1)] + m[WU_IDX(c->r1, c->g0, c->b0)]
         - m[WU_IDX(c->r0, c->g1, c->b1)] + m[WU_IDX(c->r0, c->g1, c->b0)]
         + m[WU_IDX(c->r0, c->g0, c->b1)] - m[WU_IDX(c->r0, c->g0, c->b0)];
}

static INT64 wu_bottom(const wu_box_t *c, int dir, const INT64 *m) {
    switch (dir) {
    case 0: return -m[WU_IDX(c->r0, c->g1, c->b1)] + m[WU_IDX(c->r0, c->g1, c->b0)]
                 + m[WU_IDX(c->r0, c->g0, c->b1)] - m[WU_IDX(c->r0, c->g0, c->b0)];
    case 1: return -m[WU_IDX(c->r1, c->g0, c->b1)] + m[WU_IDX(c->r1, c->g0, c->b0)]
                 + m[WU_IDX(c->r0, c->g0, c->b1)] - m[WU_IDX(c->r0, c->g0, c->b0)];
    default: return -m[WU_IDX(c->r1, c->g1, c->b0)] + m[WU_IDX(c->r1, c->g0, c->b0)]
                  + m[WU_IDX(c->r0, c->g1, c->b0)] - m[WU_IDX(c->r0, c->g0, c->b0)];
    }
}

static INT64 wu_top(const wu_box_t *c, int dir, int pos, const INT64 *m) {
    switch (dir) {
    case 0: return m[WU_IDX(pos, c->g1, c->b1)] - m[WU_IDX(pos, c->g1, c->b0)]
                 - m[WU_IDX(pos, c->g0, c->b1)] + m[WU_IDX(pos, c->g0, c->b0)];
    case 1: return m[WU_IDX(c->r1, pos, c->b1)] - m[WU_IDX(c->r1, pos, c->b0)]
                 - m[WU_IDX(c->r0, pos, c->b1)] + m[WU_IDX(c->r0, pos, c->b0)];
    default: return m[WU_IDX(c->r1, c->g1, pos)] - m[WU_IDX(c->r1, c->g0, pos)]
                  - m[WU_IDX(c->r0, c->g1, pos)] + m[WU_IDX(c->r0, c->g0, pos)];
    }
}

static INT64 wu_variance(const wu_t *q, const wu_box_t *c) {
    INT64 dw = wu_vol(c, q->w);
    if (dw <= 0) return 0;
    INT64 dr = wu_vol(c, q->mr), dg = wu_vol(c, q->mg), db = wu_vol(c, q->mb);
    INT64 dm2 = wu_vol(c, q->m2);
    return dm2 - ((dr * dr + dg * dg + db * db) / dw);
}

static int wu_maximize(const wu_t *q, const wu_box_t *c, int dir, int first, int last,
                       INT64 whole_w, INT64 whole_r, INT64 whole_g, INT64 whole_b,
                       INT64 *max_score) {
    INT64 base_w = wu_bottom(c, dir, q->w);
    INT64 base_r = wu_bottom(c, dir, q->mr);
    INT64 base_g = wu_bottom(c, dir, q->mg);
    INT64 base_b = wu_bottom(c, dir, q->mb);
    INT64 best = -1;
    int cut = -1;
    for (int i = first; i < last; i++) {
        INT64 half_w = base_w + wu_top(c, dir, i, q->w);
        if (half_w == 0 || half_w == whole_w) continue;
        INT64 half_r = base_r + wu_top(c, dir, i, q->mr);
        INT64 half_g = base_g + wu_top(c, dir, i, q->mg);
        INT64 half_b = base_b + wu_top(c, dir, i, q->mb);
        INT64 s = (half_r * half_r + half_g * half_g + half_b * half_b) / half_w;
        INT64 ow = whole_w - half_w;
        INT64 or_ = whole_r - half_r, og = whole_g - half_g, ob = whole_b - half_b;
        s += (or_ * or_ + og * og + ob * ob) / ow;
        if (s > best) { best = s; cut = i; }
    }
    *max_score = best;
    return cut;
}

static int wu_cut(const wu_t *q, wu_box_t *set1, wu_box_t *set2) {
    INT64 whole_w = wu_vol(set1, q->w);
    INT64 whole_r = wu_vol(set1, q->mr);
    INT64 whole_g = wu_vol(set1, q->mg);
    INT64 whole_b = wu_vol(set1, q->mb);
    INT64 sr, sg, sb;
    int cr = wu_maximize(q, set1, 0, set1->r0 + 1, set1->r1, whole_w, whole_r, whole_g, whole_b, &sr);
    int cg = wu_maximize(q, set1, 1, set1->g0 + 1, set1->g1, whole_w, whole_r, whole_g, whole_b, &sg);
    int cb = wu_maximize(q, set1, 2, set1->b0 + 1, set1->b1, whole_w, whole_r, whole_g, whole_b, &sb);
    int dir;
    if (sr >= sg && sr >= sb) { if (cr < 0) return 0; dir = 0; }
    else if (sg >= sr && sg >= sb) dir = 1;
    else dir = 2;

    *set2 = *set1;
    if (dir == 0)      { set1->r1 = cr; set2->r0 = cr; }
    else if (dir == 1) { set1->g1 = cg; set2->g0 = cg; }
    else               { set1->b1 = cb; set2->b0 = cb; }
    return (wu_vol(set1, q->w) > 0) && (wu_vol(set2, q->w) > 0);
}

static void lab_from_rgb(INT32 r8, INT32 g8, INT32 b8,
                         INT32 *L, INT32 *a, INT32 *b) {
    INT64 lr = SRGB2LIN[r8 & 0xFF], lg = SRGB2LIN[g8 & 0xFF], lb = SRGB2LIN[b8 & 0xFF];
    INT64 xn = (28435 * lr + 24659 * lg + 12447 * lb) >> 16;
    INT64 yn = (13933 * lr + 46871 * lg +  4732 * lb) >> 16;
    INT64 zn = ( 1163 * lr +  7172 * lg + 57201 * lb) >> 16;
    INT64 fx = xn > 580 ? cbrt_q16(xn) : (xn * 24389 / 27 + (16 << 16)) / 116;
    INT64 fy = yn > 580 ? cbrt_q16(yn) : (yn * 24389 / 27 + (16 << 16)) / 116;
    INT64 fz = zn > 580 ? cbrt_q16(zn) : (zn * 24389 / 27 + (16 << 16)) / 116;
    *L = (INT32)(116 * fy - (16 << 16));
    *a = (INT32)(500 * (fx - fy));
    *b = (INT32)(200 * (fy - fz));
}

static INT32 delin(INT64 v) {
    if (v <= 0) return 0;
    if (v >= SRGB2LIN[255]) return 255;
    INT32 lo = 0, hi = 255;
    while (lo < hi) {
        INT32 mid = (lo + hi) / 2;
        if (SRGB2LIN[mid] < v) lo = mid + 1; else hi = mid;
    }
    if (lo > 0 && (v - SRGB2LIN[lo - 1]) < (SRGB2LIN[lo] - v)) lo--;
    return lo;
}

static UINT32 rgb_from_lab(INT32 L, INT32 a, INT32 b) {
    INT64 fy = ((INT64)L + (16 << 16)) / 116;
    INT64 fx = fy + a / 500;
    INT64 fz = fy - b / 200;
    INT64 fx3 = cube_q16(fx), fz3 = cube_q16(fz);
    INT64 xn = fx3 > 580 ? fx3 : (116 * fx - (16 << 16)) * 27 / 24389;
    INT64 yn = L > (8 << 16) ? cube_q16(fy) : (INT64)L * 27 / 24389;
    INT64 zn = fz3 > 580 ? fz3 : (116 * fz - (16 << 16)) * 27 / 24389;
    INT64 X = (xn * 62290) >> 16;
    INT64 Y = yn;
    INT64 Z = (zn * 71358) >> 16;
    INT64 rl = ( 212397 * X - 100742 * Y - 32676 * Z) >> 16;
    INT64 gl = ( -63503 * X + 122942 * Y +  2720 * Z) >> 16;
    INT64 bl = (   3650 * X -  13369 * Y + 69274 * Z) >> 16;
    return ((UINT32)delin(rl) << 16) | ((UINT32)delin(gl) << 8) | (UINT32)delin(bl);
}

static UINT32 g_rand_tbl[31];
static int g_rand_f, g_rand_r;
static UINT32 grand_next(void) {
    g_rand_tbl[g_rand_f] += g_rand_tbl[g_rand_r];
    UINT32 out = g_rand_tbl[g_rand_f] >> 1;
    if (++g_rand_f == 31) g_rand_f = 0;
    if (++g_rand_r == 31) g_rand_r = 0;
    return out;
}
static void grand_seed(UINT32 seed) {
    if (!seed) seed = 1;
    g_rand_tbl[0] = seed;
    for (int i = 1; i < 31; i++) {
        INT32 prev = (INT32)g_rand_tbl[i - 1];
        INT64 word = 16807 * (INT64)(prev % 127773) - 2836 * (INT64)(prev / 127773);
        if (word < 0) word += 2147483647;
        g_rand_tbl[i] = (UINT32)word;
    }
    g_rand_f = 3; g_rand_r = 0;
    for (int i = 0; i < 310; i++) grand_next();
}

#define WS_MIN_MOVE (3 << 16)

typedef struct { UINT32 rgb; UINT32 pop; } swatch_t;

static int wsmeans(const UINT32 *thumb, UINTN npix,
                   const UINT32 *start_rgb, int nclus, swatch_t *swatch) {
    if (!npix || nclus <= 0) return 0;

    enum { HBITS = 15, HSIZE = 1 << HBITS };
    INT32  *hash   = efi_allocate_pool(HSIZE * sizeof(INT32));
    UINT32 *pixels = efi_allocate_pool(npix * sizeof(UINT32));
    UINT32 *counts = efi_allocate_pool(npix * sizeof(UINT32));
    if (!hash || !pixels || !counts) goto fail0;
    for (UINTN i = 0; i < HSIZE; i++) hash[i] = -1;
    UINTN npt = 0;
    for (UINTN i = 0; i < npix; i++) {
        UINT32 c = thumb[i] & 0xFFFFFF;
        UINTN h = (c * 2654435761u) >> (32 - HBITS);
        while (hash[h] >= 0 && pixels[hash[h]] != c) h = (h + 1) & (HSIZE - 1);
        if (hash[h] < 0) {
            hash[h] = (INT32)npt;
            pixels[npt] = c; counts[npt] = 1; npt++;
        } else counts[hash[h]]++;
    }
    efi_free_pool(hash); hash = NULL;

    if ((UINTN)nclus > npt) nclus = (int)npt;

    INT32  *pl   = efi_allocate_pool(npt * 3 * sizeof(INT32));
    INT32  *ci   = efi_allocate_pool(npt * sizeof(INT32));
    INT32  *cl   = efi_allocate_pool((UINTN)nclus * 3 * sizeof(INT32));
    INT64  *cc   = efi_allocate_pool((UINTN)nclus * nclus * sizeof(INT64));
    INT64  *sums = efi_allocate_pool((UINTN)nclus * 4 * sizeof(INT64));
    if (!pl || !ci || !cl || !cc || !sums) goto fail;

    for (UINTN i = 0; i < npt; i++)
        lab_from_rgb((INT32)(pixels[i] >> 16) & 0xFF, (INT32)(pixels[i] >> 8) & 0xFF,
                     (INT32)pixels[i] & 0xFF, &pl[i * 3], &pl[i * 3 + 1], &pl[i * 3 + 2]);
    for (int i = 0; i < nclus; i++)
        lab_from_rgb((INT32)(start_rgb[i] >> 16) & 0xFF, (INT32)(start_rgb[i] >> 8) & 0xFF,
                     (INT32)start_rgb[i] & 0xFF, &cl[i * 3], &cl[i * 3 + 1], &cl[i * 3 + 2]);

    grand_seed(42688);
    for (UINTN i = 0; i < npt; i++) ci[i] = (INT32)(grand_next() % (UINT32)nclus);

    for (int iter = 0; iter < 100; iter++) {
        for (int i = 0; i < nclus; i++) {
            cc[i * nclus + i] = 0;
            for (int j = i + 1; j < nclus; j++) {
                INT64 dl = cl[i * 3] - cl[j * 3];
                INT64 da = cl[i * 3 + 1] - cl[j * 3 + 1];
                INT64 db = cl[i * 3 + 2] - cl[j * 3 + 2];
                INT64 d = dl * dl + da * da + db * db;
                cc[i * nclus + j] = d;
                cc[j * nclus + i] = d;
            }
        }
        int moved = 0;
        for (UINTN i = 0; i < npt; i++) {
            INT32 L = pl[i * 3], A = pl[i * 3 + 1], B = pl[i * 3 + 2];
            int prev = ci[i];
            INT64 dl = L - cl[prev * 3], da = A - cl[prev * 3 + 1], db = B - cl[prev * 3 + 2];
            INT64 prev_d = dl * dl + da * da + db * db;
            INT64 min_d = prev_d;
            int best = -1;
            const INT64 *ccrow = cc + (UINTN)prev * nclus;
            for (int j = 0; j < nclus; j++) {
                if (ccrow[j] >= 4 * prev_d) continue;
                dl = L - cl[j * 3]; da = A - cl[j * 3 + 1]; db = B - cl[j * 3 + 2];
                INT64 d = dl * dl + da * da + db * db;
                if (d < min_d) { min_d = d; best = j; }
            }
            if (best >= 0) {
                INT64 chg = isqrt64(prev_d) - isqrt64(min_d);
                if (chg > WS_MIN_MOVE) { moved = 1; ci[i] = best; }
            }
        }
#ifdef ACCENT_HOST_DEBUG
        {
            int nm = 0;
            for (UINTN i = 0; i < npt && i < 20; i++)
                fprintf(stderr, "%d ", ci[i]);
            fprintf(stderr, "| iter %d moved=%d\n", iter, moved);
            (void)nm;
        }
#endif
        if (!moved && iter != 0) break;
        for (int i = 0; i < nclus * 4; i++) sums[i] = 0;
        for (UINTN i = 0; i < npt; i++) {
            INT64 *s = sums + (UINTN)ci[i] * 4;
            INT64 w = counts[i];
            s[0] += w;
            s[1] += pl[i * 3] * w; s[2] += pl[i * 3 + 1] * w; s[3] += pl[i * 3 + 2] * w;
        }
        for (int i = 0; i < nclus; i++) {
            INT64 w = sums[i * 4];
            if (!w) { cl[i * 3] = cl[i * 3 + 1] = cl[i * 3 + 2] = 0; continue; }
            cl[i * 3]     = (INT32)(sums[i * 4 + 1] / w);
            cl[i * 3 + 1] = (INT32)(sums[i * 4 + 2] / w);
            cl[i * 3 + 2] = (INT32)(sums[i * 4 + 3] / w);
        }
    }

    int ns = 0;
    for (int i = 0; i < nclus; i++) {
        INT64 w = sums[i * 4];
        if (!w) continue;
        UINT32 rgb = rgb_from_lab(cl[i * 3], cl[i * 3 + 1], cl[i * 3 + 2]);
        int j;
        for (j = 0; j < ns; j++)
            if (swatch[j].rgb == rgb) { swatch[j].pop += (UINT32)w; break; }
        if (j == ns) { swatch[ns].rgb = rgb; swatch[ns].pop = (UINT32)w; ns++; }
    }

    efi_free_pool(pl); efi_free_pool(ci); efi_free_pool(cl);
    efi_free_pool(cc); efi_free_pool(sums);
    efi_free_pool(pixels); efi_free_pool(counts);
    return ns;

fail:
    if (pl) efi_free_pool(pl);
    if (ci) efi_free_pool(ci);
    if (cl) efi_free_pool(cl);
    if (cc) efi_free_pool(cc);
    if (sums) efi_free_pool(sums);
fail0:
    if (hash) efi_free_pool(hash);
    if (pixels) efi_free_pool(pixels);
    if (counts) efi_free_pool(counts);
    return 0;
}

typedef struct {
    INT32  hue;
    INT32  chroma_q16;
    INT32  tone_q16;
    UINT32 pop;
} cand_t;

static INT32 tone_from_rgb(INT32 r8, INT32 g8, INT32 b8) {
    INT64 Y = (13933LL * SRGB2LIN[r8 & 0xFF] +
               46871LL * SRGB2LIN[g8 & 0xFF] +
                4732LL * SRGB2LIN[b8 & 0xFF]) >> 16;
    if (Y <= 580)
        return (INT32)(Y * 24389 / 27);
    return (INT32)(116 * cbrt_q16(Y) - (16 << 16));
}

#define SCORE_TARGET_CHROMA   (48 << 16)
#define SCORE_CUTOFF_CHROMA   (5 << 16)
#define SCORE_CUTOFF_PROP     655

static int score_pick(cand_t *cand, UINTN n, UINT64 total_pop) {
    if (!n || !total_pop) return -1;

    static INT32  excited[360];
    static UINT64 hue_pop[360];
    for (int i = 0; i < 360; i++) { excited[i] = 0; hue_pop[i] = 0; }
    for (UINTN i = 0; i < n; i++) hue_pop[cand[i].hue] += cand[i].pop;
    for (int h = 0; h < 360; h++) {
        if (!hue_pop[h]) continue;
        INT32 prop = (INT32)((hue_pop[h] << 16) / total_pop);
        for (int i = h - 14; i < h + 16; i++)
            excited[sanitize_deg(i)] += prop;
    }

    int best = -1;
    INT64 best_score = 0;
    for (UINTN i = 0; i < n; i++) {
        INT32 prop = excited[cand[i].hue];
        if (cand[i].chroma_q16 < SCORE_CUTOFF_CHROMA || prop <= SCORE_CUTOFF_PROP)
            continue;
        INT64 cs = (INT64)cand[i].chroma_q16 - SCORE_TARGET_CHROMA;
        cs = cs * (cand[i].chroma_q16 < SCORE_TARGET_CHROMA ? 10 : 30) / 100;
        INT64 score = (INT64)prop * 70 + cs;
        if (best < 0 || score > best_score) { best_score = score; best = (int)i; }
    }
    return best;
}

static int extract_seed(icon_t *bg, INT32 *seed_hue, INT32 *seed_chroma100, INT32 *seed_tone) {
    if (!bg || !bg->pixels || bg->width == 0 || bg->height == 0) return 0;

    UINTN W = bg->width, H = bg->height, tw, th;
    if (W <= 128 && H <= 128) { tw = W; th = H; }
    else if (H >= W) { th = 128; tw = (256 * W + H) / (2 * H); if (!tw) tw = 1; }
    else            { tw = 128; th = (256 * H + W) / (2 * W); if (!th) th = 1; }

    UINT32 *thumb = efi_allocate_pool(tw * th * sizeof(UINT32));
    if (!thumb) return 0;
    UINTN npix = 0;
    for (UINTN y = 0; y < th; y++) {
        UINTN sy = (2 * y + 1) * H / (2 * th);
        for (UINTN x = 0; x < tw; x++) {
            UINTN sx = (2 * x + 1) * W / (2 * tw);
            UINT32 p = bg->pixels[sy * W + sx];
            if (((p >> 24) & 0xFF) < 128) continue;
            thumb[npix++] = p & 0xFFFFFF;
        }
    }
    if (!npix) { efi_free_pool(thumb); return 0; }

    enum { TSIZE = WU_SIDE * WU_SIDE * WU_SIDE };
    wu_t q;
    q.w  = efi_allocate_pool(TSIZE * sizeof(INT64) * 5);
    if (!q.w) { efi_free_pool(thumb); return 0; }
    q.mr = q.w  + TSIZE;
    q.mg = q.mr + TSIZE;
    q.mb = q.mg + TSIZE;
    q.m2 = q.mb + TSIZE;
    for (UINTN i = 0; i < TSIZE * 5; i++) q.w[i] = 0;

    UINT64 total_pop = 0;
    for (UINTN idx = 0; idx < npix; idx++) {
        UINT32 p = thumb[idx];
        INT64 r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        UINTN k = WU_IDX((r >> 3) + 1, (g >> 3) + 1, (b >> 3) + 1);
        q.w[k]++;
        q.mr[k] += r; q.mg[k] += g; q.mb[k] += b;
        q.m2[k] += r * r + g * g + b * b;
        total_pop++;
    }

    for (int t = 0; t < 5; t++) {
        INT64 *m = q.w + (UINTN)t * TSIZE;
        for (int r = 1; r < WU_SIDE; r++)
            for (int g = 1; g < WU_SIDE; g++)
                for (int b = 1; b < WU_SIDE; b++)
                    m[WU_IDX(r, g, b)] += m[WU_IDX(r - 1, g, b)] + m[WU_IDX(r, g - 1, b)]
                                        + m[WU_IDX(r, g, b - 1)] - m[WU_IDX(r - 1, g - 1, b)]
                                        - m[WU_IDX(r - 1, g, b - 1)] - m[WU_IDX(r, g - 1, b - 1)]
                                        + m[WU_IDX(r - 1, g - 1, b - 1)];
    }

    wu_box_t *box = efi_allocate_pool(WU_MAX * sizeof(wu_box_t));
    if (!box) { efi_free_pool(q.w); return 0; }
    box[0].r0 = box[0].g0 = box[0].b0 = 0;
    box[0].r1 = box[0].g1 = box[0].b1 = WU_SIDE - 1;
    box[0].vol = 0;
    int nbox = 1;
    while (nbox < WU_MAX) {
        int split = -1; INT64 best = 0;
        for (int i = 0; i < nbox; i++) {
            if (box[i].vol < 0) continue;
            INT64 v = wu_variance(&q, &box[i]);
            if (v > best) { best = v; split = i; }
        }
        if (split < 0) break;
        wu_box_t s1 = box[split], s2;
        if (!wu_cut(&q, &s1, &s2)) { box[split].vol = -1; continue; }
        s1.vol = 0; s2.vol = 0;
        box[split] = s1;
        box[nbox++] = s2;
    }

    UINT32 start_rgb[WU_MAX];
    int nstart = 0;
    for (int i = 0; i < nbox; i++) {
        INT64 w = wu_vol(&box[i], q.w);
        if (w <= 0) continue;
        UINT32 r = (UINT32)(wu_vol(&box[i], q.mr) / w);
        UINT32 g = (UINT32)(wu_vol(&box[i], q.mg) / w);
        UINT32 b = (UINT32)(wu_vol(&box[i], q.mb) / w);
        start_rgb[nstart++] = (r << 16) | (g << 8) | b;
    }
    efi_free_pool(box);
    efi_free_pool(q.w);
    if (!nstart) { efi_free_pool(thumb); return 0; }

    static swatch_t swatch[WU_MAX];
    int ns = wsmeans(thumb, npix, start_rgb, nstart, swatch);
    efi_free_pool(thumb);
    if (!ns) return 0;
#ifdef ACCENT_HOST_DEBUG
    fprintf(stderr, "thumb %ux%u npix=%u nstart=%d ns=%d\n",
            (unsigned)tw, (unsigned)th, (unsigned)npix, nstart, ns);
    {
        FILE *tf = fopen("/tmp/c_thumb.bin", "wb");
        for (UINTN y = 0; y < th; y++) {
            UINTN sy = (2 * y + 1) * H / (2 * th);
            for (UINTN x = 0; x < tw; x++) {
                UINTN sx = (2 * x + 1) * W / (2 * tw);
                UINT32 p = bg->pixels[sy * W + sx] & 0xFFFFFF;
                fwrite(&p, 4, 1, tf);
            }
        }
        fclose(tf);
    }
    for (int i = 0; i < nstart; i++)
        fprintf(stderr, "start %06x\n", start_rgb[i]);
    for (int i = 0; i < ns; i++)
        fprintf(stderr, "swatch %06x %u\n", swatch[i].rgb, swatch[i].pop);
#endif

    cand_t *cand = efi_allocate_pool((UINTN)ns * sizeof(cand_t));
    if (!cand) return 0;
    UINTN n = 0;
    for (int i = 0; i < ns; i++) {
        INT32 r = (INT32)(swatch[i].rgb >> 16) & 0xFF;
        INT32 g = (INT32)(swatch[i].rgb >> 8) & 0xFF;
        INT32 b = (INT32)swatch[i].rgb & 0xFF;
        INT32 h, cq;
        cam16_hc(r, g, b, &h, &cq);
        cand[n].hue        = h;
        cand[n].tone_q16   = tone_from_rgb(r, g, b);
        cand[n].chroma_q16 = cq;
        cand[n].pop        = swatch[i].pop;
        n++;
    }
    if (!n) { efi_free_pool(cand); return 0; }

    int pick = score_pick(cand, n, total_pop);
    if (pick < 0) {
        *seed_hue = 266; *seed_chroma100 = 6227; *seed_tone = 57;
        efi_free_pool(cand);
        return 1;
    }

    *seed_hue       = cand[pick].hue;
    *seed_chroma100 = (INT32)(((INT64)cand[pick].chroma_q16 * 100) >> 16);
    *seed_tone      = cand[pick].tone_q16 >> 16;
    if (*seed_hue >= 90 && *seed_hue <= 111 &&
        cand[pick].chroma_q16 > (16 << 16) && *seed_tone < 65)
        *seed_tone = 70;
#ifdef ACCENT_HOST_DEBUG
    fprintf(stderr, "seed hue=%d chroma=%d.%02d tone=%d pop=%u\n",
            (int)cand[pick].hue,
            (int)(cand[pick].chroma_q16 >> 16),
            (int)(((cand[pick].chroma_q16 & 0xFFFF) * 100) >> 16),
            (int)(cand[pick].tone_q16 >> 16), cand[pick].pop);
#endif
    efi_free_pool(cand);
    return 1;
}

typedef struct { INT32 hue; INT32 chroma100; } palette_t;
enum { PAL_PRIM = 0, PAL_SEC, PAL_TER, PAL_NEUT, PAL_NVAR, PAL_COUNT };

static INT32 rotated_hue(INT32 hue, const INT16 *bp, const INT16 *rot, int n) {
    for (int i = 0; i < n - 1; i++)
        if (bp[i] <= hue && hue < bp[i + 1])
            return sanitize_deg(hue + rot[i]);
    return hue;
}

static const INT16 TS_TER_BP[]  = {0, 20, 71, 161, 333, 360};
static const INT16 TS_TER_ROT[] = {-40, 48, -32, 40, -32};
static const INT16 NEU_TER_BP[]  = {0, 38, 105, 161, 204, 278, 333, 360};
static const INT16 NEU_TER_ROT[] = {-32, 26, 10, -39, 24, -15, -32};
static const INT16 VIB_SEC_BP[]  = {0, 38, 105, 140, 333, 360};
static const INT16 VIB_SEC_ROT[] = {-14, 10, -14, 10, -14};
static const INT16 VIB_TER_BP[]  = {0, 38, 71, 105, 140, 161, 253, 333, 360};
static const INT16 VIB_TER_ROT[] = {-72, 35, 24, -24, 62, 50, 62, -72};
static const INT16 EXP_SECTER_BP[]  = {0, 105, 140, 204, 253, 278, 300, 333, 360};
static const INT16 EXP_SEC_ROT[]    = {-160, 155, -100, 96, -96, -156, -165, -160};
static const INT16 EXP_TER_ROT[]    = {-165, 160, -105, 101, -101, -160, -170, -165};
static const INT16 EXP_NEU_BP[]  = {0, 71, 124, 253, 278, 300, 360};
static const INT16 EXP_NEU_ROT[] = {10, 0, 10, 0, 10, 0};
#define IS_BLUE(h)   ((h) >= 250 && (h) < 270)
#define IS_YELLOW(h) ((h) >= 105 && (h) < 125)
#define NELEM(a)     ((int)(sizeof(a) / sizeof((a)[0])))

static int tc_between(INT32 angle, INT32 a, INT32 b) {
    if (a < b) return a <= angle && angle <= b;
    return a <= angle || angle <= b;
}

static void tc_tertiary(int fidelity, INT32 seed_hue, INT32 seed_chroma100,
                        INT32 seed_tone, INT32 *out_hue, INT32 *out_chroma100) {
    static INT64 temp[360];
    for (int h = 0; h < 360; h++) {
        color_t c = hct_render(h, seed_chroma100, seed_tone);
        INT32 L, a, b;
        lab_from_rgb(c.r, c.g, c.b, &L, &a, &b);
        INT32 hl = sanitize_deg(iatan2_deg(b, a));
        INT64 cl = isqrt64((INT64)a * a + (INT64)b * b);
        INT64 c107 = pow_q16(cl, 70124);
        INT64 cs = icos_q15(sanitize_deg(hl - 50));
        temp[h] = -32768 + ((c107 * cs) >> 15) / 50;
    }
    int hcold = 0, hwarm = 0;
    for (int h = 1; h < 360; h++) {
        if (temp[h] < temp[hcold]) hcold = h;
        if (temp[h] >= temp[hwarm]) hwarm = h;
    }
    if (temp[seed_hue] >= temp[hwarm]) hwarm = seed_hue;
    INT64 range = temp[hwarm] - temp[hcold];
    if (range <= 0) range = 1;

    INT32 chosen;
    if (fidelity) {
        INT32 start = tc_between(seed_hue, hcold, hwarm) ? hwarm : hcold;
        INT32 end   = tc_between(seed_hue, hcold, hwarm) ? hcold : hwarm;
        INT64 rel_in    = ((temp[seed_hue] - temp[hcold]) << 16) / range;
        INT64 comp_rel  = 65536 - rel_in;
        INT64 best_err  = -1;
        chosen = seed_hue;
        for (int add = 0; add <= 360; add++) {
            INT32 h = sanitize_deg(start + add);
            if (!tc_between(h, start, end)) continue;
            INT64 rel = ((temp[h] - temp[hcold]) << 16) / range;
            INT64 err = comp_rel - rel; if (err < 0) err = -err;
            if (best_err < 0 || err < best_err) { best_err = err; chosen = h; }
        }
    } else {
        enum { DIV = 6 };
        INT64 abs_total = 0, last = temp[seed_hue];
        for (int i = 0; i < 360; i++) {
            INT64 t = temp[sanitize_deg(seed_hue + i)];
            INT64 d = t - last; if (d < 0) d = -d;
            abs_total += d;
            last = t;
        }
        INT64 step = abs_total / DIV;
        INT32 all[DIV];
        int n = 1;
        all[0] = seed_hue;
        INT64 total = 0;
        last = temp[seed_hue];
        for (int add = 1; n < DIV; add++) {
            INT32 h = sanitize_deg(seed_hue + add);
            if (add > 360) { while (n < DIV) { all[n] = all[n - 1]; n++; } break; }
            INT64 t = temp[h];
            INT64 d = t - last; if (d < 0) d = -d;
            total += d;
            INT64 desired = (INT64)n * step;
            int addk = 1;
            while (total >= desired && n < DIV) {
                all[n++] = h;
                desired = ((INT64)n + addk) * step;
                addk++;
            }
            last = t;
        }
        chosen = all[1];
    }

    INT32 tone = seed_tone;
    color_t c = hct_render(chosen, seed_chroma100, tone);
    INT32 hh, cc;
    cam16_hc(c.r, c.g, c.b, &hh, &cc);
    if (hh >= 90 && hh <= 111 && cc > (16 << 16) && tone < 65) {
        c = hct_render(chosen, seed_chroma100, 70);
        cam16_hc(c.r, c.g, c.b, &hh, &cc);
    }
    *out_hue = hh;
    *out_chroma100 = (INT32)(((INT64)cc * 100) >> 16);
}

static void build_palettes(int variant, INT32 hue, INT32 seedC100, INT32 seed_tone,
                           palette_t pal[PAL_COUNT]) {
    for (int i = 0; i < PAL_COUNT; i++) { pal[i].hue = hue; pal[i].chroma100 = 0; }

    switch (variant) {
    default:
    case ACCENT_TONAL:
        pal[PAL_PRIM].chroma100 = 2600;
        pal[PAL_SEC].chroma100  = 1600;
        pal[PAL_TER].hue        = rotated_hue(hue, TS_TER_BP, TS_TER_ROT, NELEM(TS_TER_BP));
        pal[PAL_TER].chroma100  = 2800;
        pal[PAL_NEUT].chroma100 = 500;
        pal[PAL_NVAR].chroma100 = 850;
        break;
    case ACCENT_NEUTRAL:
        pal[PAL_PRIM].chroma100 = IS_BLUE(hue) ? 1200 : 800;
        pal[PAL_SEC].chroma100  = IS_BLUE(hue) ? 600 : 400;
        pal[PAL_TER].hue        = rotated_hue(hue, NEU_TER_BP, NEU_TER_ROT, NELEM(NEU_TER_BP));
        pal[PAL_TER].chroma100  = 2000;
        pal[PAL_NEUT].chroma100 = 140;
        pal[PAL_NVAR].chroma100 = 308;
        break;
    case ACCENT_VIBRANT: {
        INT32 nh = rotated_hue(hue, VIB_SEC_BP, VIB_SEC_ROT, NELEM(VIB_SEC_BP));
        pal[PAL_PRIM].chroma100 = 7400;
        pal[PAL_SEC].hue        = nh;
        pal[PAL_SEC].chroma100  = 5600;
        pal[PAL_TER].hue        = rotated_hue(hue, VIB_TER_BP, VIB_TER_ROT, NELEM(VIB_TER_BP));
        pal[PAL_TER].chroma100  = 5600;
        pal[PAL_NEUT].hue       = nh;
        pal[PAL_NEUT].chroma100 = 2800;
        pal[PAL_NVAR].hue       = nh;
        pal[PAL_NVAR].chroma100 = 3612;
        break;
    }
    case ACCENT_EXPRESSIVE: {
        INT32 nh = rotated_hue(hue, EXP_NEU_BP, EXP_NEU_ROT, NELEM(EXP_NEU_BP));
        INT32 nc = IS_YELLOW(nh) ? 600 : 1400;
        pal[PAL_PRIM].chroma100 = 3600;
        pal[PAL_SEC].hue        = rotated_hue(hue, EXP_SECTER_BP, EXP_SEC_ROT, NELEM(EXP_SECTER_BP));
        pal[PAL_SEC].chroma100  = 1600;
        pal[PAL_TER].hue        = rotated_hue(hue, EXP_SECTER_BP, EXP_TER_ROT, NELEM(EXP_SECTER_BP));
        pal[PAL_TER].chroma100  = 4800;
        pal[PAL_NEUT].hue       = nh;
        pal[PAL_NEUT].chroma100 = nc;
        pal[PAL_NVAR].hue       = nh;
        pal[PAL_NVAR].chroma100 = nc * (IS_YELLOW(nh) ? 160 : 230) / 100;
        break;
    }
    case ACCENT_CONTENT:
    case ACCENT_FIDELITY: {
        INT32 c = seedC100;
        INT32 sec = c - 3200 > c / 2 ? c - 3200 : c / 2;
        pal[PAL_PRIM].chroma100 = c;
        pal[PAL_SEC].chroma100  = sec;
        tc_tertiary(variant == ACCENT_FIDELITY, hue, c, seed_tone,
                    &pal[PAL_TER].hue, &pal[PAL_TER].chroma100);
        pal[PAL_NEUT].chroma100 = c / 8;
        pal[PAL_NVAR].chroma100 = c / 8 + 400;
        break;
    }
    case ACCENT_MONOCHROME:
        break;
    }
}

static INT32 max_chroma_at(INT32 h_ok, INT32 tone) {
    INT32 L = (tone + 16) * 65536 / 116;
    INT32 ca = icos_q15(h_ok), sa = isin_q15(h_ok);
    INT32 lo = 0, hi = 24000, fit = 0;
    while (lo <= hi) {
        INT32 mid = (lo + hi) / 2;
        INT32 A = (INT32)(((INT64)mid * ca) >> 15);
        INT32 B = (INT32)(((INT64)mid * sa) >> 15);
        if (oklab_in_gamut(L, A, B)) { fit = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return fit;
}

static INT32 t_max_c(INT32 hue_deg, INT32 chroma100, INT32 upper) {
    INT32 h = sanitize_deg(hue_deg - HCT_HUE_OFF[sanitize_deg(hue_deg) / 10]);
    INT32 best_tone = upper;
    INT64 best_c = -1;
    for (INT32 t = upper; t >= 0; t--) {
        INT32 target = (INT32)(((INT64)chroma100 << 16) / (c_ratio(h, t) * 100));
        INT32 c = max_chroma_at(h, t);
        if (c >= target) return t;
        INT64 hc = (INT64)c * c_ratio(h, t);
        if (hc > best_c) { best_c = hc; best_tone = t; }
    }
    return best_tone;
}

static INT32 t_min_c(INT32 hue_deg, INT32 chroma100, INT32 lo, INT32 hi) {
    INT32 h = sanitize_deg(hue_deg - HCT_HUE_OFF[sanitize_deg(hue_deg) / 10]);
    INT32 best_tone = 0;
    INT64 best_c = -1;
    for (INT32 t = 0; t <= 100; t++) {
        INT32 target = (INT32)(((INT64)chroma100 << 16) / (c_ratio(h, t) * 100));
        INT32 c = max_chroma_at(h, t);
        if (c >= target) { best_tone = t; break; }
        INT64 hc = (INT64)c * c_ratio(h, t);
        if (hc > best_c) { best_c = hc; best_tone = t; }
    }
    if (best_tone < lo) best_tone = lo;
    if (best_tone > hi) best_tone = hi;
    return best_tone;
}

static INT32 y_from_tone(INT32 tone) {
    INT32 L = (tone + 16) * 65536 / 116;
    return (INT32)(cube_q16(L) * 100);
}
static INT32 tone_from_y(INT64 y100) {
    if (y100 <= 0) return 0;
    INT64 L = cbrt_q16(y100 / 100);
    INT32 t = (INT32)((116 * L) >> 16) - 16;
    if (t < 0) t = 0;
    if (t > 100) t = 100;
    return t;
}

static INT32 foreground_tone(INT32 tone, INT32 ratio_x10) {
    INT64 yt = y_from_tone(tone);
    INT64 five = 5LL << 16;
    if (tone < 60) {
        INT64 yl = ratio_x10 * (yt + five) / 10 - five;
        if (yl <= 100LL << 16) return tone_from_y(yl);
        return 100;
    }
    INT64 yd = 10 * (yt + five) / ratio_x10 - five;
    if (yd < 0) yd = 0;
    return tone_from_y(yd);
}
static INT32 foreground_tone_45(INT32 tone) { return foreground_tone(tone, 45); }

typedef struct { UINT8 pal; UINT8 tone; UINT8 cmul_pct; } role_def_t;
static const role_def_t ROLES_2021[ACCENT_ROLE_COUNT] = {
     { PAL_PRIM, 80, 100 },
     { PAL_PRIM, 20, 100 },
     { PAL_PRIM, 30, 100 },
     { PAL_PRIM, 90, 100 },
     { PAL_SEC,  80, 100 },
     { PAL_TER,  80, 100 },
     { PAL_TER,  30, 100 },
     { PAL_NEUT,  6, 100 },
     { PAL_NEUT, 12, 100 },
     { PAL_NEUT, 17, 100 },
     { PAL_NEUT, 90, 100 },
     { PAL_NVAR, 80, 100 },
     { PAL_NVAR, 60, 100 },
};
static const UINT8 MONO_TONES[ACCENT_ROLE_COUNT] = {
    100, 10, 85, 0, 80, 90, 60, 6, 12, 17, 90, 80, 60,
};

int accent_role_from_str(const CHAR16 *s) {
    static const struct { const CHAR16 *name; int role; } names[] = {
        { L"primary",             ROLE_PRIMARY },
        { L"on_primary",          ROLE_ON_PRIMARY },
        { L"primary_container",   ROLE_PRIMARY_CONTAINER },
        { L"on_primary_container",ROLE_ON_PRIMARY_CONTAINER },
        { L"secondary",           ROLE_SECONDARY },
        { L"tertiary",            ROLE_TERTIARY },
        { L"tertiary_container",  ROLE_TERTIARY_CONTAINER },
        { L"surface",             ROLE_SURFACE },
        { L"surface_container",   ROLE_SURFACE_CONTAINER },
        { L"on_surface",          ROLE_ON_SURFACE },
        { L"on_surface_variant",  ROLE_ON_SURFACE_VARIANT },
        { L"outline",             ROLE_OUTLINE },
        { NULL, 0 }
    };
    if (!s || !s[0]) return -1;
    for (int i = 0; names[i].name; i++) {
        UINTN j = 0;
        while (s[j] && names[i].name[j]) {
            CHAR16 a = s[j], b = names[i].name[j];
            if (a >= L'A' && a <= L'Z') a = (CHAR16)(a - L'A' + L'a');
            if (a == L'-') a = L'_';
            if (a != b) break;
            j++;
        }
        if (!s[j] && !names[i].name[j]) return names[i].role;
    }
    return -1;
}

int accent_variant_from_str(const CHAR16 *s) {
    if (!s || !s[0]) return ACCENT_TONAL;
    switch (s[0]) {
        case L'n': case L'N': return ACCENT_NEUTRAL;
        case L'c': case L'C': return ACCENT_CONTENT;
        case L'f': case L'F': return ACCENT_FIDELITY;
        case L'v': case L'V': return ACCENT_VIBRANT;
        case L'e': case L'E': return ACCENT_EXPRESSIVE;
        case L'm': case L'M': return ACCENT_MONOCHROME;
        default:              return ACCENT_TONAL;
    }
}

#define IS_CYAN(h)   ((h) >= 170 && (h) < 207)

int accent_generate(icon_t *bg, int variant, color_t out[ACCENT_ROLE_COUNT]) {
    INT32 hue, chroma100, seed_tone;
    if (!extract_seed(bg, &hue, &chroma100, &seed_tone)) return 0;
    if (variant < 0 || variant > ACCENT_MONOCHROME) variant = ACCENT_TONAL;

    palette_t pal[PAL_COUNT];
    build_palettes(variant, hue, chroma100, seed_tone, pal);

    int spec2021 = (variant == ACCENT_CONTENT || variant == ACCENT_FIDELITY ||
                    variant == ACCENT_MONOCHROME);

    if (spec2021) {
        int fid = (variant != ACCENT_MONOCHROME);
        for (int i = 0; i < ACCENT_ROLE_COUNT; i++) {
            const role_def_t *r = &ROLES_2021[i];
            INT32 tone = (variant == ACCENT_MONOCHROME) ? MONO_TONES[i] : r->tone;
            if (fid && i == ROLE_PRIMARY_CONTAINER)
                tone = seed_tone;
            else if (fid && i == ROLE_ON_PRIMARY_CONTAINER)
                tone = foreground_tone_45(seed_tone);
            else if (fid && i == ROLE_TERTIARY_CONTAINER) {
                tone = seed_tone;
                if (pal[PAL_TER].hue >= 90 && pal[PAL_TER].hue <= 111 &&
                    pal[PAL_TER].chroma100 > 1600 && tone < 65)
                    tone = 70;
            }
            out[i] = hct_render(pal[r->pal].hue, pal[r->pal].chroma100, tone);
        }
        return 1;
    }

    INT32 pT, pcT, secT;
    int yellow = IS_YELLOW(pal[PAL_PRIM].hue), cyan = IS_CYAN(pal[PAL_PRIM].hue);
    switch (variant) {
    case ACCENT_NEUTRAL:
        pT   = 80;
        pcT  = 30;
        secT = t_min_c(pal[PAL_SEC].hue, pal[PAL_SEC].chroma100, 0, 98);
        break;
    case ACCENT_VIBRANT:
        pT   = t_max_c(pal[PAL_PRIM].hue, pal[PAL_PRIM].chroma100, cyan ? 88 : 98);
        pcT  = t_min_c(pal[PAL_PRIM].hue, pal[PAL_PRIM].chroma100, 66, 93);
        secT = t_max_c(pal[PAL_SEC].hue, pal[PAL_SEC].chroma100, 90);
        break;
    case ACCENT_EXPRESSIVE:
        pT   = t_max_c(pal[PAL_PRIM].hue, pal[PAL_PRIM].chroma100,
                       yellow ? 25 : (cyan ? 88 : 98));
        pcT  = t_max_c(pal[PAL_PRIM].hue, pal[PAL_PRIM].chroma100, 93);
        if (pcT < 30) pcT = 30;
        secT = 80;
        break;
    default:
        pT   = 80;
        pcT  = t_min_c(pal[PAL_PRIM].hue, pal[PAL_PRIM].chroma100, 35, 93);
        secT = 80;
        break;
    }

    INT32 terT, tcT;
    {
        INT32 cap = (variant == ACCENT_VIBRANT || variant == ACCENT_EXPRESSIVE) &&
                    IS_CYAN(pal[PAL_TER].hue) ? 88 : 98;
        terT = t_max_c(pal[PAL_TER].hue, pal[PAL_TER].chroma100, cap) + 5;
        if (terT > cap) terT = cap;

        INT32 tc_cap = (variant == ACCENT_EXPRESSIVE &&
                        IS_CYAN(pal[PAL_TER].hue)) ? 88 : 93;
        tcT = t_max_c(pal[PAL_TER].hue, pal[PAL_TER].chroma100, tc_cap);
        if (variant == ACCENT_EXPRESSIVE && tcT < 75) tcT = 75;
        if (terT < tcT + 5) tcT = terT - 5;
    }

    INT32 min_accent = foreground_tone(18, 45);
    if (pT   < min_accent) pT   = min_accent;
    if (secT < min_accent) secT = min_accent;
    if (terT < min_accent) terT = min_accent;
    if (pT < pcT + 5) pT = pcT + 5 > 100 ? 100 : pcT + 5;

    INT32 onSurfT = foreground_tone(18, 110);
    INT32 textmul;
    switch (variant) {
    case ACCENT_NEUTRAL:    textmul = 220; break;
    case ACCENT_VIBRANT:    textmul = 100; break;
    case ACCENT_EXPRESSIVE: textmul = IS_YELLOW(pal[PAL_NEUT].hue) ? 300 : 160; break;
    default:                textmul = 170; break;
    }
    INT32 scmul, schmul;
    switch (variant) {
    case ACCENT_NEUTRAL:    scmul = 160; schmul = 190; break;
    case ACCENT_VIBRANT:    scmul = 115; schmul = 122; break;
    case ACCENT_EXPRESSIVE: scmul = IS_YELLOW(pal[PAL_NEUT].hue) ? 160 : 130;
                            schmul = IS_YELLOW(pal[PAL_NEUT].hue) ? 195 : 145; break;
    default:                scmul = 140; schmul = 150; break;
    }

    struct { UINT8 pal; INT32 tone; INT32 cmul; } spec[ACCENT_ROLE_COUNT] = {
         { PAL_PRIM, pT,                        100 },
         { PAL_PRIM, foreground_tone(pT, 60),   100 },
         { PAL_PRIM, pcT,                       100 },
         { PAL_PRIM, foreground_tone(pcT, 60),  100 },
         { PAL_SEC,  secT,                      100 },
         { PAL_TER,  terT,                      100 },
         { PAL_TER,  tcT,                       100 },
         { PAL_NEUT, 4,                         100 },
         { PAL_NEUT, 9,                       scmul },
         { PAL_NEUT, 12,                     schmul },
         { PAL_NEUT, onSurfT,               textmul },
         { PAL_NEUT, foreground_tone(18, 60), textmul },
         { PAL_NEUT, foreground_tone(18, 30), textmul },
    };
    for (int i = 0; i < ACCENT_ROLE_COUNT; i++)
        out[i] = hct_render(pal[spec[i].pal].hue,
                            pal[spec[i].pal].chroma100 * spec[i].cmul / 100,
                            spec[i].tone);
    return 1;
}

