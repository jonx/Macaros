//! aros-rt — minimal `#![no_std]` Rust runtime for darwin-aarch64 AROS.
//!
//! Deliverables [RS0] (codegen + link + startup interop) and [RS1] (allocator +
//! collections) from `docs/features/rust-aros/README.md`. Two things live here:
//!
//!   1. A [`#[global_allocator]`] (`AROS_ALLOC`) that bridges Rust's `GlobalAlloc`
//!      to exec `AllocVec`/`FreeVec` through a flat C boundary — so `alloc`
//!      (`Vec`, `String`, `Box`, iterators) works on AROS.
//!   2. A `#[panic_handler]` that routes to the host C shim and aborts
//!      (`panic = "abort"`; unwinding is a later nicety — see the doc's RS-risks).
//!
//! Plus the `extern "C"` selftest entrypoints an AROS C program calls to PROVE
//! each milestone on booted AROS (RS0 = "the Rust fn ran"; RS1 = "the allocator
//! round-trips a Vec/String").
//!
//! ## The ABI boundary (the only contact surface with AROS)
//!
//! Rust never touches the AROS library-base register convention or AROS headers.
//! It imports four flat C symbols, provided by `aros_rt_glue.c` (compiled by the
//! AROS crosstools, where `<proto/exec.h>` etc. are real) — the same "flat C
//! ABI surface" the host shims use (`hostcpu_shim.h`, the bsdsock verbs):
//!
//!   void *aros_exec_allocvec(unsigned long size);   // -> AllocVec(size, MEMF_ANY)
//!   void  aros_exec_freevec(void *p);               // -> FreeVec(p)
//!   void  aros_rt_puts(const char *nul_terminated);  // -> PutStr() on Output()
//!   void  aros_rt_abort(void);                       // -> never returns
//!
//! Independent work: no third-party Rust-on-AROS / OS-port implementation source
//! was read, searched, or consulted; the allocator is the textbook
//! aligned-allocation-over-a-malloc trick, written from `core::alloc::GlobalAlloc`
//! and the AROS `exec.library/AllocVec` autodoc. Any resemblance is coincidental.

#![no_std]
// `alloc` is part of `-Zbuild-std=core,alloc`; we provide its allocator below.
extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};
use core::ffi::c_void;
use core::ptr;

// ---------------------------------------------------------------------------
// The flat C boundary (resolved by aros_rt_glue.c at the AROS-side link).
// ---------------------------------------------------------------------------
extern "C" {
    fn aros_exec_allocvec(size: usize) -> *mut c_void;
    fn aros_exec_freevec(p: *mut c_void);
    fn aros_rt_puts(s: *const u8);
    fn aros_rt_abort() -> !;
}

// ---------------------------------------------------------------------------
// [RS1] #[global_allocator] over exec AllocVec/FreeVec.
//
// AllocVec returns memory aligned to MEM_BLOCKSIZE (>= 8), but Rust `Layout`s can
// demand any power-of-two alignment (16 for u128/SIMD, more for over-aligned
// types). So we don't rely on AllocVec's alignment: over-allocate, hand back an
// aligned pointer inside the block, and stash the original AllocVec pointer in the
// `usize` immediately below it so `dealloc` can hand the exact same pointer to
// FreeVec (AllocVec tracks the size itself — FreeVec needs no length).
// ---------------------------------------------------------------------------
struct ArosAllocator;

const HDR: usize = core::mem::size_of::<usize>();

unsafe impl GlobalAlloc for ArosAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        // Need room for: the stashed base pointer (HDR) + worst-case alignment slack.
        let align = layout.align().max(HDR);
        let total = match layout.size().checked_add(align + HDR) {
            Some(t) => t,
            None => return ptr::null_mut(),
        };
        let base = aros_exec_allocvec(total) as usize;
        if base == 0 {
            return ptr::null_mut();
        }
        // First `align`-aligned address at least HDR bytes above `base`.
        let user = (base + HDR + (align - 1)) & !(align - 1);
        // Stash base in the HDR-sized slot just below the user pointer.
        ((user - HDR) as *mut usize).write(base);
        user as *mut u8
    }

    unsafe fn dealloc(&self, p: *mut u8, _layout: Layout) {
        let base = (p as usize - HDR) as *const usize;
        aros_exec_freevec(base.read() as *mut c_void);
    }
}

#[global_allocator]
static AROS_ALLOC: ArosAllocator = ArosAllocator;

// ---------------------------------------------------------------------------
// [RS0]/panic: a #[panic_handler] is mandatory for #![no_std]. panic = "abort",
// so we print a short marker via dos and abort — no unwinding, no eh_personality.
// ---------------------------------------------------------------------------
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    unsafe {
        aros_rt_puts(b"aros-rt: rust panic -> abort\n\0".as_ptr());
        aros_rt_abort()
    }
}

// ---------------------------------------------------------------------------
// [RS0] selftest: prove codegen + linking + startup interop. The C harness calls
// this and asserts the return equals AROS_RS0_MAGIC. No allocation, no I/O beyond
// one PutStr — the narrowest possible "the Rust function actually ran" check.
// ---------------------------------------------------------------------------

/// "RS0 " — must match `AROS_RS0_MAGIC` in rs0_main.c.
const AROS_RS0_MAGIC: u32 = 0x5253_3020;

#[no_mangle]
pub extern "C" fn aros_rust_selftest() -> u32 {
    unsafe { aros_rt_puts(b"aros-rt: [RS0] rust selftest ran\n\0".as_ptr()) };
    AROS_RS0_MAGIC
}

// ---------------------------------------------------------------------------
// [RS1] alloc selftest: exercise the allocator hard (many reallocs => alloc + copy
// + free), then fold the live bytes with FNV-1a and return the digest. The C
// harness asserts it equals AROS_RS1_EXPECTED. FNV over u32 words + bytes is plain
// wrapping integer arithmetic => target-independent, so the expected constant is
// reproducible on the host (see ../README.md "Reproducing the [RS1] digest").
// ---------------------------------------------------------------------------

const FNV_OFFSET: u32 = 2166136261;
const FNV_PRIME: u32 = 16777619;

#[inline]
fn fnv_u32(mut h: u32, x: u32) -> u32 {
    h ^= x;
    h.wrapping_mul(FNV_PRIME)
}

#[no_mangle]
pub extern "C" fn aros_rust_alloc_checksum() -> u32 {
    // Knuth multiplicative hash fills a growing Vec -> forces several reallocs.
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1000u32 {
        v.push(i.wrapping_mul(2654435761));
    }
    // A String that grows past its inline/initial capacity -> exercises realloc.
    let mut s = String::new();
    for _ in 0..100 {
        s.push_str("rust-on-aros\n");
    }

    let mut h = FNV_OFFSET;
    for &x in &v {
        h = fnv_u32(h, x);
    }
    for b in s.bytes() {
        h = fnv_u32(h, b as u32);
    }
    // v and s drop here -> exercises dealloc on the round trip.
    h
}
