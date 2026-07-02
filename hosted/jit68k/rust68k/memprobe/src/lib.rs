#![no_std]
#[panic_handler] fn p(_:&core::panic::PanicInfo)->!{loop{}}
unsafe extern "C" { fn putchar68k(c:u32, lib:u32); }
#[no_mangle]
pub extern "C" fn memprobe(lib: u32) -> u32 {
    let src: &[u8] = b"/tmp/std68k-probe.txt ABCDEFGHIJKLMNOPQ\n";
    let mut dst = [0u8; 40];
    dst.copy_from_slice(src);
    let mut bad=0u32; for i in 0..40 { unsafe{putchar68k(dst[i] as u32,lib)} if dst[i]!=src[i]{bad+=1;} } bad
}
