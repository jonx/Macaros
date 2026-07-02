#!/usr/bin/env perl
# emu68_darwinize.pl — build-time portability shim for the vendored Emu68 decoder
# files (OURS, AROS-licensed tool; it does NOT contain Emu68 source).
#
# WHY THIS EXISTS (a [J5c] re-hosting finding, recorded in emu68/NOTICE):
# The vendored Emu68 per-opcode decoders use the GNU C function-alias extension
#   uint32_t *EMIT_ADD(...) __attribute__((alias("EMIT_ADD_reg")));
# to collapse a chain of opcode-variant names (EMIT_ADD / _reg / _mem / _ext) onto a
# single real definition. `__attribute__((alias(...)))` is an ELF/GNU feature that
# clang on Darwin/Mach-O REJECTS outright ("aliases are not supported on darwin").
# This is a genuine, separate coupling from the three runtime couplings [J5a] named:
# the decoder SOURCE is not byte-portable to the macOS toolchain.
#
# To re-host the REAL decoders WITHOUT editing the quarantined verbatim files, this
# script reads each vendored emu68/<file>.c and writes a Darwin-compatible copy to the
# build dir ($2) in which every alias declaration is rewritten into an equivalent
# plain C forwarding function:
#   uint32_t *EMIT_ADD(...) { return EMIT_ADD_reg(ptr, opcode, m68k_ptr); }
# plus a block of forward prototypes (so the forwarder can call a not-yet-defined
# target) injected right after the file's #include lines. The transform is purely
# mechanical and preserves semantics: an alias and a tail-call forwarder are
# behaviourally identical here (same fixed signature, no side effects). The original
# quarantined files stay BYTE-VERBATIM; only the build-dir copies are patched.
#
# ============================ [J5d] EA SANDBOX REWRITE ============================
# A SECOND, file-scoped transform applied ONLY to M68k_EA.c (the [J5d] HOOK 1, the
# disclosed [J5a] fix). The REAL Emu68 EA decoder emits (An)-class memory accesses as
#     *ptr++ = ldr_offset(reg_An, *arm_reg, 0);            // (An)
#     *ptr++ = ldr_offset_postindex(reg_An, *arm_reg, 4);  // (An)+
#     *ptr++ = str_offset_preindex(reg_An, *arm_reg, -4);  // -(An)   ... etc
# treating An as a RAW HOST POINTER (1:1 MMU) and assuming a big-endian CPU. On the
# little-endian hosted sandbox both are wrong. We rewrite each such site in the
# BUILD-DIR COPY to call OUR sandbox-translating emitter (j5d_ea_helpers.c):
#     ptr = j5d_ea_mem(ptr, KIND, reg_An, *arm_reg, IMM);
# which adds the sandbox base, byteswaps with REV, and applies the post/pre index. KIND
# is derived MECHANICALLY from the original encoder name (size/sign/index), so the
# transform is faithful to what the decoder asked for. The QUARANTINE M68k_EA.c is NOT
# touched; only the build-dir copy is patched, and the patch is a pure call-substitution
# (same operands, same order) — disclosed in emu68/NOTICE.
#
# This is applied to M68k_EA.c only (detected by the j5d_ea_mem-relevant encoder
# pattern on `reg_An`); other vendored files are unaffected.

# Map an Emu68 ld/st encoder name -> the j5d_ea_mem KIND code (see j5d_ea_helpers.c):
#   bit0..1 size (0=B 1=H 2=W) ; bit2 store ; bit3 signed ; bit4..5 index(0 1 2).
sub ea_kind {
    my ($name, $imm) = @_;
    my $store  = ($name =~ /^str/) ? 1 : 0;
    my $signed = ($name =~ /^ld[rs]s/) ? 1 : 0;     # ldrsh/ldrsb => signed load
    my $size;                                       # default longword
    if    ($name =~ /^(ldr|str)(s?)h_/ || $name =~ /^(ldr|str)(s?)h$/) { $size = 1; }
    elsif ($name =~ /^(ldr|str)(s?)b_/ || $name =~ /^(ldr|str)(s?)b$/) { $size = 0; }
    elsif ($name =~ /^ldur(s?)h/) { $size = 1; }
    elsif ($name =~ /^ldur(s?)b/) { $size = 0; }
    else  { $size = 2; }                            # ldr/str/ldur word
    my $index = 0;
    $index = 1 if $name =~ /_postindex$/;
    $index = 2 if $name =~ /_preindex$/;
    # ldur_offset with a negative immediate (the -(An) size-0 longword fast path) is a
    # pre-decremented access in disguise; treat a negative imm on a plain offset load as
    # pre-index so An is adjusted. (Only relevant to the rare same-reg path; the corpus
    # never hits it, but keep the transform faithful.)
    $index = 2 if ($index == 0 && $imm < 0 && $name =~ /^ld[ru]/);
    return ($size & 3) | ($store << 2) | ($signed << 3) | (($index & 3) << 4);
}

# Usage: emu68_darwinize.pl <src.c> <out.c> [--ea-sandbox] [--movem-sandbox]
#   --ea-sandbox     enable the [J5d] (An)-class sandbox-memory EA rewrite (M68k_EA.c).
#                    Without it, only the alias-forwarder transform runs (the [J5c] build,
#                    which is register-direct and links no j5d_ea_mem helper). The [J5d]/
#                    apps builds pass it so the (An)/(An)+ memory modes route through the
#                    sandbox-base + REV helper.
#   --movem-sandbox  enable the [J5l] EMIT_MOVEM sandbox-memory rewrite (M68k_LINE4.c).
#                    EMIT_MOVEM does NOT use M68k_EA.c's ldr_offset(reg_An,...) sites — it
#                    resolves the EA base ONCE (size 0 -> `base` holds the 68k ADDRESS) and
#                    emits its OWN inner loop of raw stp/str/strh/ldr/ldp/ldrsh straight off
#                    `base` (1:1 MMU + big-endian CPU assumption). This rewrite routes each
#                    such memory touch through OUR j5d_movem_* helpers (j5d_ea_helpers.c):
#                    host = base_adjust + base (UXTW), per-register REV byteswap, with pair
#                    decomposition + the pre/post-index An update preserved. The non-memory
#                    base arithmetic (the add_immed/sub_immed that bump the 68k An, and the
#                    sub_immed(tmp_base_reg,...) base snapshot) is left AS-IS. The QUARANTINE
#                    M68k_LINE4.c stays BYTE-VERBATIM; only the build-dir copy is patched.
use strict; use warnings;
my ($src, $out, @opts) = @ARGV;
die "usage: $0 <src.c> <out.c> [--ea-sandbox] [--movem-sandbox] [--move-no-merge] [--fpu-sandbox] [--libm-asm-fix] [--rmw-sandbox] [--cas-sandbox]\n" unless $src && $out;
my $ea_sandbox    = grep { $_ eq '--ea-sandbox' } @opts;
my $movem_sandbox = grep { $_ eq '--movem-sandbox' } @opts;
my $move_no_merge = grep { $_ eq '--move-no-merge' } @opts;
my $fpu_sandbox   = grep { $_ eq '--fpu-sandbox' } @opts;     # [J5o] M68k_LINEF.c
my $libm_asm_fix  = grep { $_ eq '--libm-asm-fix' } @opts;    # [J5o] math/libm.h
my $rmw_sandbox   = grep { $_ eq '--rmw-sandbox' } @opts;     # [J5u] LINE8/9/B/C/D RMW-to-mem
my $cas_sandbox   = grep { $_ eq '--cas-sandbox' } @opts;     # [STD68K] EMIT_CAS (M68k_LINE0.c)
local $/; open my $fh, '<', $src or die "open $src: $!"; my $code = <$fh>; close $fh;

# ===================== [STD68K] EMIT_CAS SANDBOX + BYTESWAP =======================
# Applied to M68k_LINE0.c (--cas-sandbox). Emu68's EMIT_CAS treats the effective
# address `ea` as a HOST pointer and does raw exclusive loads/stores with no
# byteswap — both wrong for the little-endian hosted sandbox (whose memory is 68k
# big-endian and whose data registers hold architectural values). We:
#   (1) allocate a byteswap scratch `swp` and a host-pointer reg `eah`;
#   (2) rebase `ea` (a 68k address, and for mode (An) the LIVE mapped An which must
#       NOT be clobbered) into `eah = base_adjust(x12) + zext32(ea)` right before the
#       size dispatch — leaving `ea` intact for the alignment checks;
#   (3) route the CAS macros' memory accesses through `eah`, and REV the .w/.l
#       loaded value (after) and stored value (into swp, before). Byte is swap-free.
# Pure call/operand substitution on the quarantined source; disclosed in emu68/NOTICE.
if ($cas_sandbox) {
    my $cn = 0;
    # (1) declare swp + eah next to tmp (the CAS else-branch alloc)
    $cn += ($code =~ s{(uint8_t tmp = RA_AllocARMRegister\(&ptr\);\n)(\s*/\* Load effective address \*/)}
        {$1        uint8_t swp = RA_AllocARMRegister(&ptr);   /* [STD68K] byteswap scratch */\n        uint8_t eah = RA_AllocARMRegister(&ptr);   /* [STD68K] host pointer (ea rebased) */\n$2}s);
    # (2) rebase into eah just before the size dispatch
    $cn += ($code =~ s{(\n)(        if \(size == 1\)\n        \{\n            CAS_ATOMIC\(\);)}
        {$1        /* [STD68K] rebase the 68k EA to a host pointer in a SEPARATE reg */\n        *ptr++ = add64_reg_ext(eah, 12, ea, UXTW, 0);\n\n$2}s);
    # (3a) byteswap the loaded value after the .w/.l exclusive+plain loads
    $code =~ s{(\*ptr\+\+ = ldxrh\(ea, tmp\);\\\n)}{$1                *ptr++ = rev16(tmp, tmp);\\\n}g and $cn++;
    $code =~ s{(\*ptr\+\+ = ldxr\(ea, tmp\);\\\n)}{$1                *ptr++ = rev(tmp, tmp);\\\n}g and $cn++;
    $code =~ s{(\*ptr\+\+ = ldrh_offset\(ea, tmp, 0\);\\\n)}{$1                *ptr++ = rev16(tmp, tmp);\\\n}g and $cn++;
    $code =~ s{(\*ptr\+\+ = ldr_offset\(ea, tmp, 0\);\\\n)}{$1                *ptr++ = rev(tmp, tmp);\\\n}g and $cn++;
    # (3b) byteswap the stored value into swp for .w/.l (byte stores unchanged)
    $code =~ s{\*ptr\+\+ = stlxrh\(ea, du, status\);}{*ptr++ = rev16(swp, du);\\\n                *ptr++ = stlxrh(eah, swp, status);}g and $cn++;
    $code =~ s{\*ptr\+\+ = stlxr\(ea, du, status\);}{*ptr++ = rev(swp, du);\\\n                *ptr++ = stlxr(eah, swp, status);}g and $cn++;
    $code =~ s{\*ptr\+\+ = strh_offset\(ea, du, 0\);}{*ptr++ = rev16(swp, du);\\\n                *ptr++ = strh_offset(eah, swp, 0);}g and $cn++;
    $code =~ s{\*ptr\+\+ = str_offset\(ea, du, 0\);}{*ptr++ = rev(swp, du);\\\n                *ptr++ = str_offset(eah, swp, 0);}g and $cn++;
    # (3c) route the remaining (byte) accesses + the already-rewritten loads through eah
    $code =~ s{\*ptr\+\+ = ldxrb\(ea, tmp\);}{*ptr++ = ldxrb(eah, tmp);}g and $cn++;
    $code =~ s{\*ptr\+\+ = ldxrh\(ea, tmp\);}{*ptr++ = ldxrh(eah, tmp);}g and $cn++;
    $code =~ s{\*ptr\+\+ = ldxr\(ea, tmp\);}{*ptr++ = ldxr(eah, tmp);}g and $cn++;
    $code =~ s{\*ptr\+\+ = stlxrb\(ea, du, status\);}{*ptr++ = stlxrb(eah, du, status);}g and $cn++;
    $code =~ s{\*ptr\+\+ = ldrb_offset\(ea, tmp, 0\);}{*ptr++ = ldrb_offset(eah, tmp, 0);}g and $cn++;
    $code =~ s{\*ptr\+\+ = ldrh_offset\(ea, tmp, 0\);}{*ptr++ = ldrh_offset(eah, tmp, 0);}g and $cn++;
    $code =~ s{\*ptr\+\+ = ldr_offset\(ea, tmp, 0\);}{*ptr++ = ldr_offset(eah, tmp, 0);}g and $cn++;
    $code =~ s{\*ptr\+\+ = strb_offset\(ea, du, 0\);}{*ptr++ = strb_offset(eah, du, 0);}g and $cn++;
    # (4) free the two new regs alongside the existing CAS frees
    $cn += ($code =~ s{(        RA_FreeARMRegister\(&ptr, ea\);\n        RA_FreeARMRegister\(&ptr, status\);\n        RA_FreeARMRegister\(&ptr, tmp\);\n)}
        {$1        RA_FreeARMRegister(&ptr, swp);\n        RA_FreeARMRegister(&ptr, eah);\n}s);
    warn "darwinize: --cas-sandbox applied $cn edits\n";
}

# ===================== [J5o] LIBM.H CLANG ASM-CONSTRAINT FIX =====================
# Applied to math/libm.h only (--libm-asm-fix). Emu68's libm.h provides two tiny inline
# helpers with GNU-as register-tie input constraints clang/Mach-O REJECTS:
#     asm volatile("fsqrt %d0, %d0":"+w"(x):"0"(x));   // "0" tied-input is invalid in clang
#     asm volatile("fabs  %d0, %d0":"+w"(x):"0"(x));
# The "+w"(x) read-write output already names x as both in and out, so the tied input "0"(x)
# is REDUNDANT. Drop it — semantics-preserving (same instruction, same operand). Only the
# build-dir copy is patched; the quarantine math/libm.h stays byte-verbatim. (These helpers
# are used only by the transcendental paths this increment does not drive; the fix exists so
# the verbatim file COMPILES under clang.)
if ($libm_asm_fix) {
    my $n = ($code =~ s!("\+w"\(x\)):"0"\(x\)!$1!g);
    die "!! --libm-asm-fix: expected 2 asm-constraint sites, matched $n\n" unless $n == 2;
}

# ===================== [J5o] FPU SANDBOX + BARE-METAL ASM NEUTRALISE =====================
# Applied to M68k_LINEF.c only (--fpu-sandbox). Two transforms on the verbatim FPU decoder:
#
# (A) FP-MEMORY sandbox rewrite. FPU_FetchData/FPU_StoreData resolve the EA base ONCE
#     (EMIT_LoadFromEffectiveAddress size 0 -> int_reg/off holds the 68k ADDRESS) and emit
#     their OWN fp load/store straight off it (fldd/flds/fstd/fsts + pre/post/pimm variants),
#     the 1:1-MMU + big-endian-CPU assumption. Each such site whose BASE operand is the
#     literal `int_reg` or `off` (the resolved 68k address — uniquely the memory-EA path;
#     the immediate/const paths use REG_PC or 0 and are left ALONE) is rewritten to OUR
#     j5d_fpu_* helper (j5d_ea_helpers.c): host = base_adjust(x12) + base (UXTW) + a per-
#     element REV64/REV byteswap. The DECODE + the fp ARITHMETIC stay 100% the REAL decoder.
#     Operand order is preserved. The QUARANTINE M68k_LINEF.c stays BYTE-VERBATIM.
#
# (B) BARE-METAL ASM neutralise. The verbatim file embeds AArch64 inline-asm that clang/
#     Mach-O cannot assemble and that this increment never drives:
#       - the GNU-as transcendental Poly thunks (stub_PolySine/Cosine[Single]) using
#         `ldr x0,=constants` literal pools + `.ltorg` + `.globl` (only FSIN/FCOS need them);
#       - the bare-metal cache-maintenance asm in EMIT_lineF's MOVE16/CINV/CPUSH branch
#         (CLIDR/CSSELR/CCSIDR loops, dc/ic, `msr tpidr_el1,...` — re-hosted as host
#         __clear_cache at integration; never on the FPU arithmetic path).
#     Their bodies are emptied in the BUILD-DIR COPY (the PolySine* global SYMBOLS are still
#     provided as no-op stubs in j5o_fpu_shims.c so the transcendental blr sites link). The
#     QUARANTINE file is untouched. This is a portability neutralise, identical in spirit to
#     the alias-forwarder rewrite — semantics of the DRIVEN ops are unaffected.
if ($fpu_sandbox) {
    my $fp = 0;
    # (A) FP-memory sites: base operand is int_reg or off; value is *reg / reg / vfp_reg.
    #   plain offset  -> j5d_fpu_f{ldd,std,lds->flds,sts}(reg, base, offset)
    #   _preindex     -> _pre   (helper does base+=pre_sz first)
    #   _postindex    -> _post  (helper does base+=post_sz after)
    #   _pimm         -> _pimm  (helper multiplies the scaled immediate back up)
    my %fpmap = (
        'fldd' => 'j5d_fpu_fldd', 'fldd_preindex' => 'j5d_fpu_fldd_pre',
        'fldd_postindex' => 'j5d_fpu_fldd_post', 'fldd_pimm' => 'j5d_fpu_fldd_pimm',
        'fstd' => 'j5d_fpu_fstd', 'fstd_preindex' => 'j5d_fpu_fstd_pre',
        'fstd_postindex' => 'j5d_fpu_fstd_post', 'fstd_pimm' => 'j5d_fpu_fstd_pimm',
        'flds' => 'j5d_fpu_flds', 'flds_preindex' => 'j5d_fpu_flds_pre',
        'flds_postindex' => 'j5d_fpu_flds_post', 'flds_pimm' => 'j5d_fpu_flds_pimm',
        'fsts' => 'j5d_fpu_fsts', 'fsts_preindex' => 'j5d_fpu_fsts_pre',
        'fsts_postindex' => 'j5d_fpu_fsts_post', 'fsts_pimm' => 'j5d_fpu_fsts_pimm',
    );
    $code =~ s{
        \*ptr\+\+\s*=\s*
        (fldd_pimm|fldd_preindex|fldd_postindex|fldd|
         fstd_pimm|fstd_preindex|fstd_postindex|fstd|
         flds_pimm|flds_preindex|flds_postindex|flds|
         fsts_pimm|fsts_preindex|fsts_postindex|fsts)
        \(\s*(\*reg|reg|vfp_reg)\s*,\s*(int_reg|off)\s*,\s*([^;()]+?)\s*\)\s*;
    }{
        my ($enc, $val, $base, $arg) = ($1, $2, $3, $4);
        my $h = $fpmap{$enc};
        $fp++;
        "ptr = $h(ptr, $val, $base, $arg); /* [J5o] FP-mem sandbox: was $enc */";
    }gex;

    # (A2) FP INTEGER-FORMAT memory sites (.l/.w/.b FMOVE <-> mem). Unlike the fldd/flds FP
    #   loads/stores, the integer-format FMOVE converts in a GP reg and emits a PLAIN integer
    #   ldr/str (+h/+b, +ur/preindex/postindex) STRAIGHT OFF int_reg/off (the resolved 68k
    #   address) — the SAME 1:1-MMU + big-endian assumption, but emitted INLINE in M68k_LINEF.c
    #   (FPU_FetchData/FPU_StoreData Case 3), NOT via the darwinized M68k_EA.c. Operand order is
    #   (BASE, val, offset) — base FIRST (the address), val second — so we match only sites whose
    #   FIRST operand is the literal int_reg/off (the memory-EA path; the Dn / immediate / PC paths
    #   never use int_reg as the base of these and are left ALONE). Route each to OUR j5d_fpu_i*
    #   helper (j5d_ea_helpers.c): base-adjust(x12) + per-element byteswap (REV / REV16 / none),
    #   sign-correct for the decoder's following scvtf_32toD. Sizes from the encoder: ldr/str=.l(4),
    #   ldrsh/strh=.w(2), ldrsb/strb=.b(1). The QUARANTINE M68k_LINEF.c stays BYTE-VERBATIM.
    my %imap = (
        # .l (longword): ldr/ldur load, str/stur store
        'ldr_offset'            => 'j5d_fpu_ildr',  'ldur_offset'   => 'j5d_fpu_ildr',
        'ldr_offset_preindex'   => 'j5d_fpu_ildr_pre',
        'ldr_offset_postindex'  => 'j5d_fpu_ildr_post',
        'str_offset'            => 'j5d_fpu_istr',  'stur_offset'   => 'j5d_fpu_istr',
        'str_offset_preindex'   => 'j5d_fpu_istr_pre',
        'str_offset_postindex'  => 'j5d_fpu_istr_post',
        # .w (halfword, sign-extending load): ldrsh/ldursh load, strh/sturh store
        'ldrsh_offset'          => 'j5d_fpu_ildrh', 'ldursh_offset' => 'j5d_fpu_ildrh',
        'ldrsh_offset_preindex' => 'j5d_fpu_ildrh_pre',
        'ldrsh_offset_postindex'=> 'j5d_fpu_ildrh_post',
        'strh_offset'           => 'j5d_fpu_istrh', 'sturh_offset'  => 'j5d_fpu_istrh',
        'strh_offset_preindex'  => 'j5d_fpu_istrh_pre',
        'strh_offset_postindex' => 'j5d_fpu_istrh_post',
        # .b (byte, sign-extending load): ldrsb/ldursb load, strb/sturb store
        'ldrsb_offset'          => 'j5d_fpu_ildrb', 'ldursb_offset' => 'j5d_fpu_ildrb',
        'ldrsb_offset_preindex' => 'j5d_fpu_ildrb_pre',
        'ldrsb_offset_postindex'=> 'j5d_fpu_ildrb_post',
        'strb_offset'           => 'j5d_fpu_istrb', 'sturb_offset'  => 'j5d_fpu_istrb',
        'strb_offset_preindex'  => 'j5d_fpu_istrb_pre',
        'strb_offset_postindex' => 'j5d_fpu_istrb_post',
    );
    my $fpi = 0;
    $code =~ s{
        \*ptr\+\+\s*=\s*
        (ldr_offset_preindex|ldr_offset_postindex|ldr_offset|ldur_offset|
         str_offset_preindex|str_offset_postindex|str_offset|stur_offset|
         ldrsh_offset_preindex|ldrsh_offset_postindex|ldrsh_offset|ldursh_offset|
         strh_offset_preindex|strh_offset_postindex|strh_offset|sturh_offset|
         ldrsb_offset_preindex|ldrsb_offset_postindex|ldrsb_offset|ldursb_offset|
         strb_offset_preindex|strb_offset_postindex|strb_offset|sturb_offset)
        \(\s*(int_reg|off)\s*,\s*(val_reg)\s*,\s*([^;()]+?)\s*\)\s*;
    }{
        my ($enc, $base, $val, $arg) = ($1, $2, $3, $4);
        my $h = $imap{$enc};
        $fpi++;
        "ptr = $h(ptr, $base, $val, $arg); /* [J5o] FP-int-mem sandbox: was $enc */";
    }gex;

    # (B1) Empty the GNU-as inline-asm thunk bodies that clang/Mach-O cannot assemble and the
    #   [J5o] FPU sub-decoder never drives: the four Poly transcendental thunks (stub_PolyXxx)
    #   AND the __trampoline_icache_invalidate thunk (its `.globl ... bl invalidate_instruction_
    #   cache` uses the BARE C name, which Mach-O underscore-mangling won't resolve; the MOVE16/
    #   CINV path that uses it is not driven). Match `void __attribute__((used)) NAME(void) { ... }`
    #   and replace the body with `{ }`. The global SYMBOLS (PolySine etc, trampoline_icache_
    #   invalidate) are provided as no-op stubs in j5o_fpu_shims.c so the (un-driven) blr sites link.
    my $poly = 0;
    while ($code =~ /(void\s+__attribute__\(\(used\)\)\s+(?:stub_Poly\w+|__trampoline_icache_invalidate)\s*\(void\)\s*)\{/g) {
        my $hdr_end = pos($code); my $open = $hdr_end - 1;
        my $depth = 1; my $i = $hdr_end;
        while ($i < length($code) && $depth) {
            my $ch = substr($code, $i, 1); $depth++ if $ch eq '{'; $depth-- if $ch eq '}'; $i++;
        }
        substr($code, $open, $i - $open) = "{ /* [J5o] darwinize: GNU-as thunk neutralised (transcendental/cache, not driven) */ }";
        pos($code) = $open + 5;
        $poly++;
    }

    # (B2) Empty the bare-metal cache-maintenance asm functions in the MOVE16/CINV path.
    #   These are file-static helpers wrapping `asm volatile(... dc/ic/msr tpidr_el1 ...)`.
    #   Match a `static ... NAME(...) { ... asm volatile(...) ... }` whose body contains
    #   `tpidr_el1` or a `dc ` / `ic ` cache op and empty it. Scoped narrowly by the asm
    #   marker so only the cache thunks are touched.
    my $cache = 0;
    while ($code =~ /\b(static\s+(?:void|uint32_t\s*\*?|int)\s+\w+\s*\([^;{]*\)\s*)\{/g) {
        my $hdr_end = pos($code); my $open = $hdr_end - 1;
        my $depth = 1; my $i = $hdr_end;
        while ($i < length($code) && $depth) {
            my $ch = substr($code, $i, 1); $depth++ if $ch eq '{'; $depth-- if $ch eq '}'; $i++;
        }
        my $body = substr($code, $open, $i - $open);
        if ($body =~ /tpidr_el1|"\s*msr\s|"\s*dc\s|"\s*ic\s|CLIDR|CSSELR|CCSIDR/) {
            substr($code, $open, $i - $open) = "{ /* [J5o] darwinize: bare-metal cache-maintenance asm neutralised (host __clear_cache at integration) */ }";
            pos($code) = $open + 5;
            $cache++;
        } else {
            pos($code) = $open + 1;
        }
    }

    # (C) [J5p] TRANSCENDENTAL CALL-SITE ADDRESS-MATERIALIZATION FIX. The verbatim FPU decoder
    #   bakes a runtime helper address (the host libm fn for a transcendental: &sin/&cos/&exp/...,
    #   and &sincos/&remquo/&exp10) into the emitted block as four immediate-moves, then `blr`s it:
    #       *ptr++ = mov64_immed_u16 (R, u.u16[3], 0);   // word3 (HIGH 16 bits) -> bits [0:16)
    #       *ptr++ = movk64_immed_u16(R, u.u16[2], 1);   // word2 -> bits [16:32)
    #       *ptr++ = movk64_immed_u16(R, u.u16[1], 2);   // word1 -> bits [32:48)
    #       *ptr++ = movk64_immed_u16(R, u.u16[0], 3);   // word0 (LOW 16 bits)  -> bits [48:64)
    #   On a LITTLE-ENDIAN host (AArch64/darwin) u.u16[0] is the LOW 16 bits, so this pairing emits
    #   the address WORD-REVERSED — the assembled x_R holds the byteswapped pointer and the blr
    #   jumps to garbage. (Upstream Emu68's bare-metal RasPi build resolves helpers in low memory
    #   where this never bit; our hosted libm addresses have nonzero high words, so it bites.) Fix
    #   in the BUILD-DIR COPY only: pair word N with shift N (the standard movz/movk lo->hi build).
    #   This is the sanctioned "adjust a vendored helper CALL in the darwinize transform, not the
    #   quarantine" path (CLEANROOM.md); the QUARANTINE M68k_LINEF.c stays BYTE-VERBATIM, and only
    #   the address BYTE-ORDER is corrected — the decode + the blr + the FP semantics are untouched.
    my $callsite = 0;
    $code =~ s{
        \*ptr\+\+\s*=\s*mov64_immed_u16\s*\(\s*(\w+)\s*,\s*u\.u16\[3\]\s*,\s*0\s*\)\s*;\s*
        \*ptr\+\+\s*=\s*movk64_immed_u16\s*\(\s*\1\s*,\s*u\.u16\[2\]\s*,\s*1\s*\)\s*;\s*
        \*ptr\+\+\s*=\s*movk64_immed_u16\s*\(\s*\1\s*,\s*u\.u16\[1\]\s*,\s*2\s*\)\s*;\s*
        \*ptr\+\+\s*=\s*movk64_immed_u16\s*\(\s*\1\s*,\s*u\.u16\[0\]\s*,\s*3\s*\)\s*;
    }{
        my $r = $1; $callsite++;
        "*ptr++ = mov64_immed_u16($r, u.u16[0], 0); /* [J5p] darwinize: call-site addr LE fix */\n".
        "        *ptr++ = movk64_immed_u16($r, u.u16[1], 1);\n".
        "        *ptr++ = movk64_immed_u16($r, u.u16[2], 2);\n".
        "        *ptr++ = movk64_immed_u16($r, u.u16[3], 3);";
    }gex;

    # (D) [J5p] FMOD GP-SCRATCH REGISTER FIX. Emu68's FMOD (opmode 0x21) computes the remainder
    #   into fp_dst with FP scratch v0/v1 (caller-saved SIMD — safe), then derives the FPSR
    #   QUOTIENT byte using the GENERAL-PURPOSE registers x0 and x1 as scratch:
    #       *ptr++ = bic_immed(1, 0, 25, 25);   // x1 = x0 & ~bit25
    #       *ptr++ = orr_immed(0, 0, 25, 25);   // x0 = x0 | bit25
    #       *ptr++ = csel(0, 1, 0, A64_CC_PL);  // x0 = PL ? x1 : x0
    #   On Emu68 bare-metal x0/x1 are free scratch (the m68k context lives in TPIDRRO_EL0). In
    #   OUR hosted model x1 is the PERMANENT struct j5d_m68k_state* (J5C_STATE_X) kept for the
    #   whole block — so FMOD's use of x1 as scratch CORRUPTS the state pointer, and the next
    #   state access (the CCR/FPSR flush) faults. Redirect the SCRATCH register 1 -> the FMOD-
    #   local RA temp `tmp` (allocated at the top of the FMOD case, freed at the bottom — a real
    #   free register), in the BUILD-DIR COPY only. The result (remainder in fp_dst) and the
    #   asserted FPSR cc byte are unchanged; only the intermediate GP scratch moves off x1. The
    #   QUARANTINE M68k_LINEF.c stays BYTE-VERBATIM. (x0 stays — it is free scratch in our model:
    #   the entry arg is moved out of x0 into x1 in the prologue, so x0 is dead for the block.)
    my $fmod = 0;
    $fmod += ($code =~ s/\*ptr\+\+\s*=\s*bic_immed\(1,\s*0,\s*25,\s*25\);/\*ptr++ = bic_immed(tmp, 0, 25, 25); \/* [J5p] FMOD GP-scratch off x1(state) *\//g);
    $fmod += ($code =~ s/\*ptr\+\+\s*=\s*csel\(0,\s*1,\s*0,\s*A64_CC_PL\);/\*ptr++ = csel(0, tmp, 0, A64_CC_PL); \/* [J5p] FMOD GP-scratch off x1(state) *\//g);
    warn "[darwinize --fpu-sandbox] FMOD GP-scratch (x1->tmp) sites=$fmod\n";

    if ($fp || $fpi) {
        my $decl = "\n/* [J5o] darwinize: FP-memory sandbox helpers (OURS, j5d_ea_helpers.c) */\n";
        my %seen;
        for my $h (sort values %fpmap) {
            next if $seen{$h}++;
            $decl .= "uint32_t *$h(uint32_t *ptr, uint8_t reg, uint8_t base, int arg);\n";
        }
        $decl .= "/* [J5o] FP integer-format (.l/.w/.b) memory helpers (base, val, off) */\n";
        for my $h (sort values %imap) {
            next if $seen{$h}++;
            $decl .= "uint32_t *$h(uint32_t *ptr, uint8_t base, uint8_t val, int arg);\n";
        }
        if ($code =~ /(\A.*?(?:^\s*#\s*include[^\n]*\n)+)/ms) {
            my $head = $1; substr($code, length($head), 0) = $decl;
        } else { $code = $decl . $code; }
    }
    warn "[darwinize --fpu-sandbox] FP-mem sites=$fp FP-int-mem sites=$fpi poly-stubs=$poly cache-thunks=$cache call-site-addr-fix=$callsite\n";
}

# ===================== [J5m] MOVE TWO-PUSH MERGE NEUTRALISE =====================
# Applied to M68k_MOVE.c only (--move-no-merge). Emu68's M68k_MOVE.c has a fast path that
# MERGES two consecutive `move.l Reg,-(An)` / `(An)+` / `-(An),Reg` / `(An)+,Reg` into a
# single AArch64 stp/ldp PAIR access (M68k_MOVE.c:142-324). That merged pair accesses
# memory through the RAW 68k address register (e.g. stp_preindex(a7,...)), bypassing
# EMIT_StoreToEffectiveAddress / EMIT_LoadFromEffectiveAddress — so it gets NEITHER the
# sandbox base-adjust NOR the big-endian REV that the darwinized M68k_EA.c applies. The
# hand-written corpus never produced back-to-back register pushes to the same -(An), so it
# was latent; the vbcc C compiler emits exactly that (pushing call args: move.l X,-(a7) ;
# move.l Y,-(a7)), which faulted (str to a raw 68k a7).
#
# FIX (semantics-preserving, NO quarantine edit): neutralise ONLY the outer merge guard so
# `done` stays 0 and EVERY move falls through to the `if (!done)` general path — which uses
# EMIT_Load/StoreFromEffectiveAddress and IS sandbox-transformed (via the --ea-sandbox
# M68k_EA.c). Unmerging changes only HOW MANY AArch64 words are emitted, never the 68k
# semantics. The guard line `if ((opcode & 0xf000) == 0x2000)` is unique in the file. The
# quarantine M68k_MOVE.c stays byte-verbatim; only the build-dir copy is patched.
if ($move_no_merge) {
    # Match ONLY the unique merge-guard condition; rewrite it to a never-true form. The
    # following `{` block and all four inner candidate tests stay intact but never run, so
    # `done` is always 0 and the move falls through to the sandbox-aware `if (!done)` path.
    my $repl = 'if (0 /* [J5m] darwinize: merge disabled, route via sandbox EA */ && (opcode & 0xf000) == 0x2000)';
    my $n = ($code =~ s!\Qif ((opcode & 0xf000) == 0x2000)\E!$repl!g);
    die "!! --move-no-merge: expected exactly 1 merge guard, matched $n\n" unless $n == 1;
}

# ===================== [J5l] EMIT_MOVEM SANDBOX REWRITE =====================
# Applied to M68k_LINE4.c only (--movem-sandbox). Each rewrite is a pure call-substitution
# on a `*ptr++ = <encoder>(base, ...);` site whose base operand is the literal `base`
# (EMIT_MOVEM's EA-base variable) — which uniquely scopes to movem (lea/link/unlk/pea in
# this file use `dest`/`src`/`sp`, never `base`). Operand order is preserved; only WHICH
# emitter is called changes. The pre/post-index amount is passed positive (the helper does
# the sub/add on the 68k An). Run BEFORE the alias rewrite (these sites contain no aliases).
if ($movem_sandbox) {
    my $mv = 0;
    # PAIR stores/loads (32-bit W-register pair). stp_preindex(base,a,b,-N) -> base-=N first.
    $code =~ s{\*ptr\+\+\s*=\s*stp_preindex\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*-(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_stp_pre(ptr, base, $1, $2, $3); /* [J5l] was stp_preindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*stp\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_stp(ptr, base, $1, $2, $3); /* [J5l] was stp */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*ldp_postindex\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_ldp_post(ptr, base, $1, $2, $3); /* [J5l] was ldp_postindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*ldp\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_ldp(ptr, base, $1, $2, $3); /* [J5l] was ldp */"}gex;
    # SINGLE word stores/loads. _preindex carries a negative literal -block_size.
    $code =~ s{\*ptr\+\+\s*=\s*str_offset_preindex\(\s*base\s*,\s*(\w+)\s*,\s*-(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_str_pre(ptr, base, $1, $2); /* [J5l] was str_offset_preindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*str_offset\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_str(ptr, base, $1, $2); /* [J5l] was str_offset */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*ldr_offset\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_ldr(ptr, base, $1, $2); /* [J5l] was ldr_offset */"}gex;
    # SINGLE half stores / signed-half loads.
    $code =~ s{\*ptr\+\+\s*=\s*strh_offset_preindex\(\s*base\s*,\s*(\w+)\s*,\s*-(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_strh_pre(ptr, base, $1, $2); /* [J5l] was strh_offset_preindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*strh_offset\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_strh(ptr, base, $1, $2); /* [J5l] was strh_offset */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*ldrsh_offset\(\s*base\s*,\s*(\w+)\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_ldrsh(ptr, base, $1, $2); /* [J5l] was ldrsh_offset */"}gex;

    # [J5m] pea/link/unlk stack-frame ops: their -(a7) push / (a7)+ pop go through the RAW
    # 68k a7 (`sp`) — same bypass as movem, never reached by the hand-written corpus but
    # emitted by the C COMPILER (vbcc): `pea label` (push a string-literal address), and
    # `link`/`unlk` stack frames. These use `sp` + operand names `ea`/`reg`, which are UNIQUE
    # to EMIT_PEA / EMIT_LINK16/32 / EMIT_UNLK. Route through the SAME sandbox-aware
    # movem helpers (base-adjust + REV + pre/post index).
    $code =~ s{\*ptr\+\+\s*=\s*str_offset_preindex\(\s*sp\s*,\s*ea\s*,\s*-(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_str_pre(ptr, sp, ea, $1); /* [J5m] PEA push: was str_offset_preindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*str_offset_preindex\(\s*sp\s*,\s*reg\s*,\s*-(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_str_pre(ptr, sp, reg, $1); /* [J5m] LINK push: was str_offset_preindex */"}gex;
    $code =~ s{\*ptr\+\+\s*=\s*ldr_offset_postindex\(\s*sp\s*,\s*reg\s*,\s*(\w+)\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_ldr_post(ptr, sp, reg, $1); /* [J5m] UNLK pop: was ldr_offset_postindex */"}gex;

    # [STD68K] EMIT_JSR's return-address push (`str_offset_preindex(sp, REG_PC, -4)`).
    # The old assumption "control-flow sp sites are owned by the dispatcher, never
    # emitted" broke the day gcc emitted `jsr (d16,PC)`: is_terminator() did not know
    # that form, the block compiler inlined EMIT_JSR, and the raw push wrote to the
    # RAW 68k a7 as a host address (host SIGSEGV at the 68k stack address). The
    # dispatcher now DOES decode jsr/jmp (d16,PC) as terminators, but the raw push
    # stays sandboxed anyway so no future jsr EA form can silently corrupt memory.
    $code =~ s{\*ptr\+\+\s*=\s*str_offset_preindex\(\s*sp\s*,\s*REG_PC\s*,\s*-4\s*\)\s*;}
              {$mv++; "ptr = j5d_movem_str_pre(ptr, sp, REG_PC, 4); /* [STD68K] JSR push: was str_offset_preindex */"}gex;

    if ($mv) {
        my $decl = "\n/* [J5l] darwinize: movem sandbox-memory helpers (OURS, j5d_ea_helpers.c) */\n"
                 . "uint32_t *j5d_movem_stp(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int offset);\n"
                 . "uint32_t *j5d_movem_stp_pre(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int amt);\n"
                 . "uint32_t *j5d_movem_str(uint32_t *ptr, uint8_t base, uint8_t rt, int offset);\n"
                 . "uint32_t *j5d_movem_str_pre(uint32_t *ptr, uint8_t base, uint8_t rt, int amt);\n"
                 . "uint32_t *j5d_movem_strh(uint32_t *ptr, uint8_t base, uint8_t rt, int offset);\n"
                 . "uint32_t *j5d_movem_strh_pre(uint32_t *ptr, uint8_t base, uint8_t rt, int amt);\n"
                 . "uint32_t *j5d_movem_ldp(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int offset);\n"
                 . "uint32_t *j5d_movem_ldp_post(uint32_t *ptr, uint8_t base, uint8_t rt1, uint8_t rt2, int amt);\n"
                 . "uint32_t *j5d_movem_ldr(uint32_t *ptr, uint8_t base, uint8_t rt, int offset);\n"
                 . "uint32_t *j5d_movem_ldr_post(uint32_t *ptr, uint8_t base, uint8_t rt, int amt);\n"
                 . "uint32_t *j5d_movem_ldrsh(uint32_t *ptr, uint8_t base, uint8_t rt, int offset);\n";
        if ($code =~ /(\A.*?(?:^\s*#\s*include[^\n]*\n)+)/ms) {
            my $head = $1; substr($code, length($head), 0) = $decl;
        } else { $code = $decl . $code; }
    }
}

# [J5d] EA rewrite — applied to every `*ptr++ = <ldst>(reg_An, *arm_reg, <imm>);` site.
# The substitution is purely textual on the verbatim source we just read; it preserves
# operand order (reg_An base, *arm_reg value, imm index) and only changes WHICH emitter
# is called. Run BEFORE the alias rewrite (these sites contain no aliases). Gated on
# --ea-sandbox so the [J5c] build (no j5d_ea_mem linked) is unaffected.
if ($ea_sandbox) {
    my $ea_rewrites = 0;
    # Direct sites: the (An)/(An)+/-(An) class on `reg_An` (modes 2/3/4, [J5d]) AND the
    # absolute-long (mode 7.1) class on `tmp_reg` ([J5g]) — Emu68 emits abs.l as a separate
    # `ldr_offset(tmp_reg, *arm_reg, 0)` that does NOT route through the funnel helpers.
    # Both are a raw `<ldst>_offset(<base>, *arm_reg, <imm>)` whose <base> holds a 68k
    # address; rewrite each to OUR sandbox emitter (base-adjust + REV + post/pre index).
    $code =~ s{
        \*ptr\+\+\s*=\s*
        (ld[rsu][a-z]*_offset(?:_postindex|_preindex)?|st[ru][a-z]*_offset(?:_postindex|_preindex)?)
        \(\s*(reg_An|tmp_reg)\s*,\s*\*arm_reg\s*,\s*(-?\d+)\s*\)\s*;
    }{
        my ($enc, $base, $imm) = ($1, $2, $3);
        my $kind = ea_kind($enc, $imm + 0);
        $ea_rewrites++;
        "ptr = j5d_ea_mem(ptr, ${kind}u, ${base}, *arm_reg, ${imm}); /* [J5d]/[J5g] EA: was ${enc} */";
    }gex;
    # ===================== [J5g] FUNNEL-HELPER body rewrite =======================
    # The (d16,An)/(d8,An,Xn)/abs/PC-relative memory modes do NOT use the direct
    # ldr_offset(reg_An,...) sites above — the REAL Emu68 EA decoder routes them through
    # four inline funnel helpers. We replace the BODY of each with a single tail-call to
    # OUR sandbox emitter (j5d_ea_helpers.c), preserving the decoder's computed base/index/
    # offset/size/sign exactly. The DECODE (mode select, extension-word reads, index
    # sign/scale) stays the REAL decoder; only the memory touch is sandbox-translated.
    # KIND bit layout (mirrors ea_kind / j5d_ea_helpers.c): bit0..1 size(0=B,1=H,2=W),
    # bit2 store, bit3 signed, bit6 = LEA (compute the 68k address only, no memory touch).
    # [J5m] Emu68 `size==0` is the LEA case AND `size==1` is a BYTE access — both used to map
    # to kind 0u, so a byte funnel access was wrongly treated as LEA (no store/load emitted).
    # Now size==1 -> 0u (byte, sz bits) and size==0 -> 0x40u (the LEA flag, sz bits 0); the
    # helpers branch on J5D_EA_LEA, not sz==0.
    my $szk = '((size==4)?2u:(size==2)?1u:(size==1)?0u:0x40u)';
    my %funnel = (
        # name => body-call-template using the helper's own args
        'load_reg_from_addr_offset' =>
            "return j5d_ea_addr_offset(ptr, $szk | (sign_ext?8u:0u), base, reg, offset, offset_32bit);",
        'store_reg_to_addr_offset'  =>
            "return j5d_ea_addr_offset(ptr, $szk | 4u, base, reg, offset, offset_32bit);",
        'load_reg_from_addr'        =>
            "return j5d_ea_addr_index(ptr, $szk | (sign_ext?8u:0u), base, reg, index, shift);",
        'store_reg_to_addr'         =>
            "return j5d_ea_addr_index(ptr, $szk | 4u, base, reg, index, shift);",
    );
    my $funnel_rewrites = 0;
    for my $name (keys %funnel) {
        # Find:  ... uint32_t * <name> ( <args> ) {  ...balanced...  }
        # and replace the {...} body with our forwarding call. Balanced-brace scan.
        if ($code =~ /(\b\Q$name\E\s*\([^)]*\)\s*)\{/g) {
            my $hdr_end = pos($code);                 # just after the opening brace
            my $open    = $hdr_end - 1;               # index of the '{'
            # scan to the matching close brace
            my $depth = 1; my $i = $hdr_end;
            while ($i < length($code) && $depth) {
                my $ch = substr($code, $i, 1);
                $depth++ if $ch eq '{';
                $depth-- if $ch eq '}';
                $i++;
            }
            my $body_len = $i - $open;                # length of {...}
            substr($code, $open, $body_len) =
                "{ /* [J5g] darwinize: sandbox EA funnel */ $funnel{$name} }";
            $funnel_rewrites++;
        }
        pos($code) = 0;
    }
    if ($ea_rewrites || $funnel_rewrites) {
        # Forward-declare our helpers after the includes so the rewritten calls resolve.
        my $decl = "\n/* [J5d]/[J5g] darwinize: sandbox-memory EA helpers (OURS, j5d_ea_helpers.c) */\n"
                 . "uint32_t *j5d_ea_mem(uint32_t *ptr, unsigned kind, uint8_t reg_An, uint8_t val, int index_amount);\n"
                 . "uint32_t *j5d_ea_addr_offset(uint32_t *ptr, unsigned kind, uint8_t base, uint8_t val, int offset, int offset_32bit);\n"
                 . "uint32_t *j5d_ea_addr_index(uint32_t *ptr, unsigned kind, uint8_t base, uint8_t val, uint8_t index, uint8_t shift);\n";
        if ($code =~ /(\A.*?(?:^\s*#\s*include[^\n]*\n)+)/ms) {
            my $head = $1; substr($code, length($head), 0) = $decl;
        } else { $code = $decl . $code; }
    }
}

# ===================== [J5u] RMW MEMORY-DESTINATION SANDBOX REWRITE =====================
# Applied to M68k_LINE8/9/B/C/D (--rmw-sandbox). The REAL Emu68 EMIT_<ALU>_reg decoders, for
# a read-modify-write ALU op with a MEMORY destination (`or.l Dn,<ea>` / `add.l Dn,<ea>` /
# `and`/`sub`/`eor`), resolve the destination EA address ONCE (EMIT_LoadFromEffectiveAddress
# size 0 -> `dest` holds the 68k ADDRESS) and then emit their OWN raw ldr/<op>/str round-trip
# straight off `dest`:  *ptr++ = ldr_offset(dest, tmp, 0); ... *ptr++ = str_offset(dest, tmp, 0);
# That is the 1:1-MMU + big-endian-CPU assumption, wrong TWICE on the little-endian sandbox:
# (1) `dest` is a 68k address, not a host pointer, so the raw deref faults (esp. abs.l, where
# `dest` is a high 68k constant); (2) it skips the big-endian REV. Unlike MOVE's memory stores
# (which funnel through the darwinized M68k_EA.c helpers), these RMW touches are emitted INLINE
# in the LINE8/9/B/C/D decoders and were NEVER sandbox-translated — the hand-built corpus has
# no `<alu> Dn,(ea)` site, so it stayed latent until real compiler output (vbcc `g |= x;`).
# We rewrite EACH such `ldr*/str*_offset[_pre/postindex](dest, tmp, OFF)` site to OUR existing
# (An)-class emitter j5d_ea_mem (j5d_ea_helpers.c): host = base_adjust(x12) + dest (UXTW), the
# big-endian REV, AND the pre/post-index An update (the -(An)/(An)+ cases) via its index_amount
# argument — exactly as the [J5d] direct-site rewrite does for reg_An. KIND (size/store/signed/
# index) is derived MECHANICALLY by ea_kind() from the encoder name, identical to --ea-sandbox.
# `(dest, tmp)` as (base, value) appears ONLY in these RMW blocks (the BCD/ADDX/SUBX paths use
# an_src/regx/regy/...), so the match is exact. The QUARANTINE files stay BYTE-VERBATIM. Run
# BEFORE the alias rewrite (these sites contain no aliases).
if ($rmw_sandbox) {
    my $rmw_rewrites = 0;
    # Base operand is the literal `dest` (the resolved 68k EA address from EMIT_Load-
    # FromEffectiveAddress size 0); the VALUE operand is the load/op-result/immediate
    # register — `tmp` for the ALU ops, `immed` for the LINE0 immediate ops, etc. We key
    # ONLY on base==`dest` (the BCD/ADDX/SUBX paths use an_src/regx/regy/dst as base, so
    # they never match). Plain (non pre/post) sites are always offset 0 — the decoder folds
    # any displacement into `dest` first — so routing them through j5d_ea_mem (which accesses
    # at [dest+0] for index==0) is faithful; the pre/post amount rides index_amount.
    $code =~ s{
        \*ptr\+\+\s*=\s*
        ((?:ldr|str)[bh]?_offset(?:_preindex|_postindex)?)
        \(\s*dest\s*,\s*([A-Za-z_]\w*)\s*,\s*([^;]+?)\s*\)\s*;
    }{
        my ($enc, $val, $off) = ($1, $2, $3);
        my $imm_num = ($off =~ /^\s*-?\d+\s*$/) ? ($off + 0) : 0;  # suffix drives index; heuristic n/a
        # A plain offset site MUST be 0 (no index suffix, displacement already in `dest`);
        # j5d_ea_mem index==0 ignores the amount, so a nonzero plain offset would mistranslate.
        die "!! --rmw-sandbox: plain (dest,$val) site with nonzero offset '$off' in $src — unhandled\n"
            if ($enc !~ /_(pre|post)index$/ && $imm_num != 0);
        my $kind = ea_kind($enc, $imm_num);
        $rmw_rewrites++;
        "ptr = j5d_ea_mem(ptr, ${kind}u, dest, ${val}, ${off}); /* [J5u] RMW-EA: was ${enc} */";
    }gex;
    if ($rmw_rewrites) {
        my $decl = "\n/* [J5u] darwinize: RMW memory-destination sandbox EA helper (OURS, j5d_ea_helpers.c) */\n"
                 . "uint32_t *j5d_ea_mem(uint32_t *ptr, unsigned kind, uint8_t reg_An, uint8_t val, int index_amount);\n";
        if ($code =~ /(\A.*?(?:^\s*#\s*include[^\n]*\n)+)/ms) {
            my $head = $1; substr($code, length($head), 0) = $decl;
        } else { $code = $decl . $code; }
    }
}

# 1) Collect every aliased name (the function being declared as an alias). All such
#    decls in these files share the signature (uint32_t*, uint16_t, uint16_t**).
my %names;
# two-line form: "TYPE NAME(args)\n __attribute__((alias("T")));"
while ($code =~ /([A-Za-z_]\w*)\s*\([^;{]*\)\s*\n\s*__attribute__\(\(alias\("([A-Za-z_]\w*)"\)\)\)\s*;/g) {
    $names{$1} = 1; $names{$2} = 1;
}
# one-line form
while ($code =~ /([A-Za-z_]\w*)\s*\([^;{]*\)\s*__attribute__\(\(alias\("([A-Za-z_]\w*)"\)\)\)\s*;/g) {
    $names{$1} = 1; $names{$2} = 1;
}

# 2) Rewrite alias declarations -> forwarding definitions (two-line then one-line).
$code =~ s/\)\s*\n\s*__attribute__\(\(alias\("([A-Za-z_]\w*)"\)\)\)\s*;/) { return $1(ptr, opcode, m68k_ptr); }/g;
$code =~ s/\)\s*__attribute__\(\(alias\("([A-Za-z_]\w*)"\)\)\)\s*;/) { return $1(ptr, opcode, m68k_ptr); }/g;

# 3) Inject static forward prototypes after the last top-of-file #include line, so the
#    forwarders resolve regardless of definition order. `static` matches the file's own
#    (mostly static) definitions; non-static targets (a few are extern) still match
#    because a later non-static definition overrides a static prototype only with a
#    warning — to avoid that we emit the prototypes WITHOUT `static` and rely on the
#    real definitions providing linkage. Use a GCC/clang-safe `__attribute__((unused))`.
if (%names) {
    my $protos = "\n/* [J5c] darwinize: forward prototypes for alias-forwarders */\n";
    for my $n (sort keys %names) {
        # Match each prototype's storage class to how the name is DEFINED in the file
        # (a `static foo() { ... }` body, or a `static foo() __FWD` forwarder we just
        # made), so we don't emit a non-static proto before a static definition.
        my $is_static = ($code =~ /\bstatic\s+uint32_t\s*\*\s*\Q$n\E\s*\(/) ? 1 : 0;
        $protos .= ($is_static ? "static " : "")
                 . "uint32_t *$n(uint32_t *ptr, uint16_t opcode, uint16_t **m68k_ptr);\n";
    }
    # insert after the last contiguous #include block near the top
    if ($code =~ /(\A.*?(?:^\s*#\s*include[^\n]*\n)+)/ms) {
        my $head = $1;
        substr($code, length($head), 0) = $protos;
    } else {
        $code = $protos . $code;
    }
}

open my $ofh, '>', $out or die "open $out: $!"; print $ofh $code; close $ofh;
