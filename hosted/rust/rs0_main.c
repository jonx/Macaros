/* rs0_main.c — the AROS C program that owns startup and calls into Rust.
 *
 * The [RS0]/[RS1] harness from docs/features/rust-aros/README.md. C owns AROS
 * startup (DOSBase/SysBase, argv) and calls the no_std Rust staticlib; a Rust-owned
 * fn main() waits for std (RS3). Links (relocatable, via collect-aros):
 *
 *     rs0_main.o + aros_rt_glue.o + libaros_rt.a  ->  an ET_REL AROS program
 *
 * Header-clean (PutStr + a libc-free hex, no <stdio.h>) so it builds against the
 * darwin-aarch64 backend without dragging in broken host SDK headers (see
 * aros_rt_glue.c). One PASS/FAIL line per milestone for the unattended loop;
 * returns 0 on full PASS, 20 (FAILAT trips) otherwise.
 *
 *   [RS0]  aros_rust_selftest()       == AROS_RS0_MAGIC   (codegen+link+startup)
 *   [RS1]  aros_rust_alloc_checksum() == AROS_RS1_EXPECTED (allocator + collections)
 */
#include <proto/dos.h>     /* PutStr — no <stdio.h> */

/* Must match lib.rs. "RS0 " in ASCII. */
#define AROS_RS0_MAGIC     0x52533020u
/* FNV-1a digest of the [RS1] Vec<u32>+String workload. Reproduce on the host:
   see ../README.md "Reproducing the [RS1] digest" (target-independent integer
   arithmetic, so the host value is authoritative). */
#define AROS_RS1_EXPECTED  0xE5889F2Du

extern unsigned int aros_rust_selftest(void);
extern unsigned int aros_rust_alloc_checksum(void);

/* libc-free 8-digit hex into buf[0..8] (buf must hold 9 bytes). */
static void hex8(unsigned int v, char *buf)
{
    static const char d[] = "0123456789abcdef";
    int i;
    for (i = 7; i >= 0; --i) { buf[i] = d[v & 0xF]; v >>= 4; }
    buf[8] = '\0';
}

int main(void)
{
    int ok = 1;
    char hb[9];
    unsigned int rs0, rs1;

    rs0 = aros_rust_selftest();
    PutStr("[RS0] rust selftest magic 0x"); hex8(rs0, hb); PutStr(hb);
    if (rs0 == AROS_RS0_MAGIC) PutStr(" PASS\n");
    else { PutStr(" FAIL\n"); ok = 0; }

    rs1 = aros_rust_alloc_checksum();
    PutStr("[RS1] alloc checksum 0x"); hex8(rs1, hb); PutStr(hb);
    if (rs1 == AROS_RS1_EXPECTED) PutStr(" PASS (Vec<u32>+String round-trip)\n");
    else { PutStr(" FAIL\n"); ok = 0; }

    PutStr(ok ? "RUST-AROS: ALL PASS\n" : "RUST-AROS: FAIL\n");
    return ok ? 0 : 20;
}
