//! rust68k corpus — REAL Rust (core + alloc, no_std) compiled for m68k and run as
//! AmigaOS hunk executables through the 68k JIT and the independent reference
//! interpreter (run68k / run68k --interp).  See ../README.md for the pipeline and
//! ../UPSTREAM-LLVM-CCR-BUG.md for the documented upstream miscompile the
//! `vecsum_inclusive` canary tracks.
#![no_std]

extern crate alloc;
use alloc::vec::Vec;
use core::alloc::{GlobalAlloc, Layout};

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

/// Bump allocator over a BSS buffer: no OS calls, so `alloc` (Vec, String, ...)
/// works under run68k's stub OS with nothing but the sandbox. dealloc is a no-op;
/// the corpus programs allocate a few hundred bytes against a 4 KiB heap.
struct Bump;
static mut HEAP: [u8; 4096] = [0; 4096];
static mut OFF: usize = 0;

unsafe impl GlobalAlloc for Bump {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        let a = l.align().max(4);
        let o = (OFF + a - 1) & !(a - 1);
        if o + l.size() > HEAP.len() {
            return core::ptr::null_mut();
        }
        OFF = o + l.size();
        HEAP.as_mut_ptr().add(o)
    }
    unsafe fn dealloc(&self, _p: *mut u8, _l: Layout) {}
}

#[global_allocator]
static A: Bump = Bump;

// NOTE: the core-only `fib` program lives in its own crate (fibcore/), NOT here.
// With the upstream MOVE-clobbers-CCR bug open, whether a given loop compiles
// correctly is register-allocation luck, and the luck changes with the crate's
// sibling functions. fibcore/'s exact shape is the verified-correct one.

/// The allocator itself, without Vec: two raw allocations, write through both,
/// read back. entry_allocprobe.s -> exit 10 (7 + 3).
#[no_mangle]
pub extern "C" fn allocprobe() -> u32 {
    unsafe {
        let p = A.alloc(Layout::from_size_align(16, 4).unwrap());
        if p.is_null() {
            return 200;
        }
        *p = 7;
        let q = A.alloc(Layout::from_size_align(32, 4).unwrap());
        if q.is_null() {
            return 201;
        }
        *q = 3;
        (*p + *q) as u32
    }
}

/// alloc + Vec (grow path included: no with_capacity) with an EXCLUSIVE range,
/// summed by explicit indexing. entry_vecsum.s -> exit 91 (1+..+13).
#[no_mangle]
pub extern "C" fn vecsum() -> u32 {
    let mut v: Vec<u32> = Vec::with_capacity(16);
    for i in 1..14u32 {
        v.push(i);
    }
    let mut s = 0u32;
    let mut k = 0usize;
    while k < v.len() {
        s = s.wrapping_add(v[k]);
        k += 1;
    }
    s
}

/// CANARY for the upstream LLVM M68k CCR bug (UPSTREAM-LLVM-CCR-BUG.md). The correct
/// answer is 91, but rustc nightly 2026-06-27 compiles the INCLUSIVE `1..=13` loop
/// with a `move.l` scheduled between the loop test's flag-setting `sub.l` and the
/// `bcs` that consumes it; MOVE sets CCR on a real 68k, so the loop body runs ONCE
/// and this returns 1 — on the JIT, on the interpreter, and on real silicon alike.
/// The regression asserts 1 under BOTH engines; when this starts returning 91 the
/// upstream fix has landed and the assert in the Makefile target must flip to 91.
#[no_mangle]
pub extern "C" fn vecsum_inclusive() -> u32 {
    let mut v: Vec<u32> = Vec::with_capacity(16);
    for i in 1..=13u32 {
        v.push(i);
    }
    v.iter().sum()
}
