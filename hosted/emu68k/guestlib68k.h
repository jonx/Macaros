/* guestlib68k.h - reusable construction core for disk-loaded 68k libraries. */
#ifndef GUESTLIB68K_H
#define GUESTLIB68K_H

#include "j4_hunk.h"

#include <stdint.h>

#define GL68_RTF_EXTENDED 0x40u
#define GL68_RTF_AUTOINIT 0x80u

typedef struct gl68_resident {
    uint32_t tag;
    uint32_t end_skip;
    uint32_t name_ptr;
    uint32_t id_ptr;
    uint32_t init;
    uint16_t revision;
    uint8_t flags;
    uint8_t version;
    uint8_t type;
    int8_t priority;
    char name[64];
} gl68_resident;

typedef struct gl68_init {
    uint32_t base;       /* nonzero after AUTOINIT construction */
    uint32_t init_pc;    /* direct rt_Init or AUTOINIT final callback */
    uint32_t seglist;    /* A0 value for the initializer */
    uint32_t neg_size;
    uint32_t pos_size;
    uint32_t vectors;
} gl68_init;

/* Select the named NT_LIBRARY resident across every loaded CODE/DATA hunk and
 * validate all of its pointers against the complete loaded segment set. */
int gl68_find_resident(const j4_sandbox *sb, const j4_seglist *seg,
                       const char *requested_name, gl68_resident *out,
                       char *err, unsigned errlen);

/* Prepare the correct initializer form. Direct residents produce an init_pc
 * and no base. AUTOINIT residents allocate/build the complete negative vector
 * area + positive Library, apply InitStruct, and copy Resident identity fields;
 * the caller then runs init_pc with D0=base, A0=seglist, A6=SysBase. */
int gl68_prepare_init(j4_sandbox *sb, const j4_seglist *seg,
                      const gl68_resident *resident, gl68_init *out,
                      char *err, unsigned errlen);

#endif
