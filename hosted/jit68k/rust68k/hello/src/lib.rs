//! hello68k — a Rust program that PRINTS through the stub OS's PutChar LVO.
//!
//! run68k enters the program with A6 = the stub library base; entry_hello.s
//! passes it to `hello(libbase)` and provides `putchar68k(c, libbase)`, a
//! 12-byte thunk that does the real 68k library call (`jsr -30(a6)`, d0 = char).
//! Everything the program prints lands on run68k's stdout.
#![no_std]

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! {
    loop {}
}

unsafe extern "C" {
    fn putchar68k(c: u32, libbase: u32);
}

fn puts(s: &str, lib: u32) {
    for &b in s.as_bytes() {
        unsafe { putchar68k(b as u32, lib) }
    }
}

/// Print `n` in decimal (digits collected LSB-first, emitted in reverse).
fn put_u32(mut n: u32, lib: u32) {
    let mut buf = [0u8; 10];
    let mut i = 0usize;
    loop {
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
        if n == 0 {
            break;
        }
    }
    while i > 0 {
        i -= 1;
        unsafe { putchar68k(buf[i] as u32, lib) }
    }
}

#[no_mangle]
pub extern "C" fn hello(libbase: u32) -> u32 {
    puts("Hello from Rust on 68k AROS!\n", libbase);

    let (mut a, mut b) = (0u32, 1u32);
    for _ in 0..10u32 {
        let t = a.wrapping_add(b);
        a = b;
        b = t;
    }
    puts("fib(10) = ", libbase);
    put_u32(a, libbase);

    puts("\n6 * 7 = ", libbase);
    put_u32(6 * 7, libbase);
    puts("\n", libbase);
    0
}
