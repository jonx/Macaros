# aros-env.sh -- export the AROS cross-compile recipe for cc-rs C dependencies.
#
#   source hosted/zed/aros-env.sh
#   cargo +nightly-2026-06-27 build -p zed_aros_app --target <spec> -Zbuild-std=...
#
# Several dependency crates (sqlite, tree-sitter, ring) build C code through
# cc-rs, which reads CC_/AR_/CFLAGS_<target> from the environment. The tracked
# .cargo/config.toml stays path-free (see the [env] note there), so the
# machine-specific SDK-include roots and the ELF archiver are set here instead,
# derived from $AROS_BUILD and $AROS_CROSSTOOLS exactly like
# crates/gpui_aros/build.rs. frontier-check.sh expects this to be sourced first.
#
# The underscore form of the env names is required: cc-rs also accepts the
# dashed form (CFLAGS_aarch64-unknown-aros) but the shell cannot export names
# with dashes.

: "${AROS_BUILD:=$HOME/aros-build}"
: "${AROS_CROSSTOOLS:=$HOME/aros-crosstools}"

_aros_sdk="$AROS_BUILD/bin/darwin-aarch64"
_aros_shim="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)/c-compat"

export CC_aarch64_unknown_aros="$AROS_CROSSTOOLS/bin/clang"
export AR_aarch64_unknown_aros="$AROS_CROSSTOOLS/bin/llvm-ar"

# The crosstools clang defaults to the aarch64-unknown-aros triple. The two
# SQLITE_* flags drop mmap (AROS has no mmap) and the load-extension path.
# HAVE_ENDIAN_H + the c-compat shim satisfy endian.h / sys/mman.h lookups.
# -fno-pic -mcmodel=large and -ffixed-x18 match the Rust target ABI
# (relocation-model "static", code-model "large", "+reserve-x18" in
# aarch64-unknown-aros.json): this C runs inside AROS processes, where x18
# belongs to the platform and addresses need the large model. They must agree
# or the linked binary misbehaves at run time. (large model requires -fno-pic.)
export CFLAGS_aarch64_unknown_aros="\
--target=aarch64-unknown-aros \
-fno-pic -mcmodel=large -ffixed-x18 \
-D_GNU_SOURCE -DHAVE_ENDIAN_H \
-DSQLITE_MAX_MMAP_SIZE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
-I$_aros_shim \
-isystem $_aros_sdk/AROS/Developer/include/aros/posixc \
-isystem $_aros_sdk/AROS/Developer/include/aros/stdc \
-isystem $_aros_sdk/AROS/Developer/include \
-isystem $_aros_sdk/gen/include/aros/posixc \
-isystem $_aros_sdk/gen/include/aros/stdc \
-isystem $_aros_sdk/gen/include"

unset _aros_sdk _aros_shim
