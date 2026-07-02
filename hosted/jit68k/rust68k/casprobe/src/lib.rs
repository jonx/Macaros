#![no_std]
#[panic_handler] fn p(_:&core::panic::PanicInfo)->!{loop{}}
use core::sync::atomic::{AtomicU32, AtomicU8, Ordering};
#[no_mangle]
pub extern "C" fn casprobe() -> u32 {
    let a = AtomicU32::new(100);
    // compare_exchange success then fail
    let ok = a.compare_exchange(100, 250, Ordering::SeqCst, Ordering::SeqCst).is_ok();
    let bad = a.compare_exchange(100, 999, Ordering::SeqCst, Ordering::SeqCst).is_err();
    a.fetch_add(5, Ordering::SeqCst);   // 250 -> 255
    let v = a.load(Ordering::SeqCst);   // 255
    let b = AtomicU8::new(7);
    b.fetch_add(3, Ordering::SeqCst);   // 10
    let bv = b.load(Ordering::SeqCst) as u32; // 10
    // exit = 255 - 10 + (ok as u32) + (bad as u32) = 247
    v - bv + (ok as u32) + (bad as u32)
}
