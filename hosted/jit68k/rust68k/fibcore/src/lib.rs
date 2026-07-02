#![no_std]
// core-only corpus program: iterative Fibonacci. entry_fib.s calls fib(10) -> exit 55.
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

#[no_mangle]
pub extern "C" fn fib(n: u32) -> u32 {
    let (mut a, mut b) = (0u32, 1u32);
    for _ in 0..n {
        let t = a.wrapping_add(b);
        a = b;
        b = t;
    }
    a
}
