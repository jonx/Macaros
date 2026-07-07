// PR ARTIFACT (not built here): the built-in form of hosted/rust/aarch64-unknown-aros.json.
//
// Destination in a rust-lang/rust fork:
//     compiler/rustc_target/src/spec/targets/aarch64_unknown_aros.rs
// Register it in compiler/rustc_target/src/spec/mod.rs by adding this line to the
// `supported_targets! { ... }` macro (alphabetical order):
//     ("aarch64-unknown-aros", aarch64_unknown_aros),
//
// This mirrors the JSON spec 1:1 (that JSON + `-Zjson-target-spec` is what the
// *verified* std builds use today; keep the two byte-compatible). It cannot be
// compiled in this repo's clone: the clone is a `rust-src` subset (library-only, no
// `compiler/` tree). Byte-identical-build confirmation is the mechanical upstream
// step: check out rust-lang/rust at the toolchain commit (`rustc +nightly-2026-06-27
// -vV` -> commit-hash), drop this file in, `./x build`, then diff a probe built with
// `--target aarch64-unknown-aros` against one built from the JSON.
//
// The exact `crate::spec` helper names drift between rustc versions; the field values
// below are authoritative (they equal the JSON). A reviewer reconciles helper spelling
// against the target rustc.

use crate::spec::{
    Cc, CodeModel, LinkerFlavor, Lld, PanicStrategy, RelocModel, Target, TargetMetadata,
    TargetOptions,
};

pub(crate) fn target() -> Target {
    let flavor = LinkerFlavor::Gnu(Cc::Yes, Lld::Yes);
    Target {
        // AROS loads GOT-less and reuses the bare-metal aarch64 LLVM triple; the AROS
        // ABI comes from the ELF flavour flag in pre_link_args, not the LLVM triple.
        llvm_target: "aarch64-unknown-none-elf".into(),
        metadata: TargetMetadata {
            description: Some("ARM64 AROS (hosted on Apple Silicon / aarch64-darwin)".into()),
            tier: Some(3),
            host_tools: Some(false),
            std: Some(true),
        },
        pointer_width: 64,
        data_layout:
            "e-m:e-p270:32:32-p271:32:32-p272:64:64-i8:8:32-i16:16:32-i64:64-i128:128-n32:64-S128-Fn32"
                .into(),
        arch: "aarch64".into(),
        options: TargetOptions {
            os: "aros".into(),
            vendor: "unknown".into(),

            // +reserve-x18: Apple Silicon zeroes x18 across signal delivery and
            // AROS-hosted preempts via SIGALRM, so x18 must be reserved or long-lived
            // compiler temporaries parked there get clobbered (the ffmpeg/h264 class of
            // bug). The whole OS runtime path is also built `-ffixed-x18`.
            features: "+v8a,+neon,+reserve-x18".into(),
            max_atomic_width: Some(128),

            panic_strategy: PanicStrategy::Abort,
            relocation_model: RelocModel::Static,
            code_model: Some(CodeModel::Large), // mandatory: AROS loads GOT-less
            disable_redzone: true,

            // No TLS section support (the pal uses pthread-key TLS instead), no dynamic
            // loader, but real executables.
            has_thread_local: false,
            dynamic_linking: false,
            executables: true,
            emit_debug_gdb_scripts: false,

            // Link through clang with the AROS ELF flavour; collect-aros/crosstools do
            // the final ET_REL relocatable link (see hosted/rust/std-build.sh).
            linker_flavor: flavor,
            linker: Some("clang".into()),
            pre_link_args: TargetOptions::link_args(
                flavor,
                &["-Wl,-maarch64elf_aros", "-Wl,--allow-multiple-definition"],
            ),

            ..Default::default()
        },
    }
}
