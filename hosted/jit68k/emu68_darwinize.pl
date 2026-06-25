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

# Usage: emu68_darwinize.pl <src.c> <out.c> [--ea-sandbox]
#   --ea-sandbox  enable the [J5d] (An)-class sandbox-memory EA rewrite (M68k_EA.c).
#                 Without it, only the alias-forwarder transform runs (the [J5c] build,
#                 which is register-direct and links no j5d_ea_mem helper). The [J5d]/
#                 apps builds pass it so the (An)/(An)+ memory modes route through the
#                 sandbox-base + REV helper.
use strict; use warnings;
my ($src, $out, @opts) = @ARGV;
die "usage: $0 <src.c> <out.c> [--ea-sandbox]\n" unless $src && $out;
my $ea_sandbox = grep { $_ eq '--ea-sandbox' } @opts;
local $/; open my $fh, '<', $src or die "open $src: $!"; my $code = <$fh>; close $fh;

# [J5d] EA rewrite — applied to every `*ptr++ = <ldst>(reg_An, *arm_reg, <imm>);` site.
# The substitution is purely textual on the verbatim source we just read; it preserves
# operand order (reg_An base, *arm_reg value, imm index) and only changes WHICH emitter
# is called. Run BEFORE the alias rewrite (these sites contain no aliases). Gated on
# --ea-sandbox so the [J5c] build (no j5d_ea_mem linked) is unaffected.
if ($ea_sandbox) {
    my $ea_rewrites = 0;
    $code =~ s{
        \*ptr\+\+\s*=\s*
        (ld[rsu][a-z]*_offset(?:_postindex|_preindex)?|st[ru][a-z]*_offset(?:_postindex|_preindex)?)
        \(\s*reg_An\s*,\s*\*arm_reg\s*,\s*(-?\d+)\s*\)\s*;
    }{
        my ($enc, $imm) = ($1, $2);
        my $kind = ea_kind($enc, $imm + 0);
        $ea_rewrites++;
        "ptr = j5d_ea_mem(ptr, ${kind}u, reg_An, *arm_reg, ${imm}); /* [J5d] EA: was ${enc} */";
    }gex;
    if ($ea_rewrites) {
        # Forward-declare our helper after the includes so the rewritten calls resolve.
        my $decl = "\n/* [J5d] darwinize: sandbox-memory EA helper (OURS, j5d_ea_helpers.c) */\n"
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
