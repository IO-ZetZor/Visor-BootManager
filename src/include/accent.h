#ifndef ACCENT_H
#define ACCENT_H

#include <efi.h>
#include "gui.h"

enum {
    ACCENT_TONAL = 0,
    ACCENT_NEUTRAL,
    ACCENT_CONTENT,
    ACCENT_FIDELITY,
    ACCENT_VIBRANT,
    ACCENT_EXPRESSIVE,
    ACCENT_MONOCHROME,
};

enum {
    ROLE_PRIMARY = 0,
    ROLE_ON_PRIMARY,
    ROLE_PRIMARY_CONTAINER,
    ROLE_ON_PRIMARY_CONTAINER,
    ROLE_SECONDARY,
    ROLE_TERTIARY,
    ROLE_TERTIARY_CONTAINER,
    ROLE_SURFACE,
    ROLE_SURFACE_CONTAINER,
    ROLE_SURFACE_CONTAINER_HIGH,
    ROLE_ON_SURFACE,
    ROLE_ON_SURFACE_VARIANT,
    ROLE_OUTLINE,
    ACCENT_ROLE_COUNT
};

int accent_variant_from_str(const CHAR16 *s);

int accent_role_from_str(const CHAR16 *s);

int accent_generate(icon_t *bg, int variant, color_t out[ACCENT_ROLE_COUNT]);

#endif
