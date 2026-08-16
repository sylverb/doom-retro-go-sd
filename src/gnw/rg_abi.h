//
// rg_abi.h — firmware ABI for gnw-doom (same header as every other core).
//
#ifndef RG_ABI_H
#define RG_ABI_H

#include "gw_firmware_abi.h"

/* Historical doom names → template accessors. */
#define gnw_abi()              gw_firmware_abi()
#define GNW_FIRMWARE_ABI_VERSION GW_FIRMWARE_ABI_VERSION
typedef gw_firmware_abi_t      gnw_firmware_abi_t;

/* SDK enum is GW_MEM_DTC; doom wrappers historically said DTCM. */
#ifndef GW_MEM_DTCM
#define GW_MEM_DTCM GW_MEM_DTC
#endif

static inline int gnw_abi_ok(void)
{
    const gw_firmware_abi_t *a = gw_firmware_abi();
    return a && a->version == GW_FIRMWARE_ABI_VERSION &&
           a->size >= (uint32_t)sizeof(gw_firmware_abi_t);
}

#endif /* RG_ABI_H */
