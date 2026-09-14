/* stub_selfheal.c - stub when UEFI NVRAM self-healing is compiled out */

#include "efi_selfheal.h"

nvsh_report_t nvram_self_heal(nvsh_policy_t *policy) {
    (void)policy;
    nvsh_report_t ret = {0};
    return ret;
}

const CHAR16 * nvsh_err_text(int err) {
    (void)err;
    return L"(self-heal not compiled in)";
}

const CHAR16 * nvsh_order_name(int mode) {
    (void)mode;
    return L"(none)";
}

void nvsh_policy_defaults(nvsh_policy_t *policy) {
    (void)policy;
}
