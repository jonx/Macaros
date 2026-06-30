//! std spike: does `std` compile + link for aarch64-unknown-aros?
//!
//! Not no_std. Uses std::println (pulls in std's stdio platform layer) so the
//! build either succeeds (some pal selected) or fails at the pal/libc wall.
//! With the aros stdio pal + build.rs known-target entry, std is no longer
//! `restricted_std` and println writes through posixc to dos Output().

use std::collections::HashMap;

#[no_mangle]
pub extern "C" fn aros_rust_std_hello() -> u32 {
    // println! -> std stdout -> aros stdio pal -> posixc write -> dos Output()
    println!("hello from rust std on AROS");

    // Vec + iterators + the global allocator (sys/alloc/aros.rs over posixc)
    let v: Vec<i32> = (1..=10).collect();
    let sum: i32 = v.iter().sum();

    // HashMap exercises sys/random/aros.rs (hash keys) AND the allocator hard
    let mut m: HashMap<&str, i32> = HashMap::new();
    m.insert("aros", 64);
    m.insert("rust", 2021);

    // format! (alloc::fmt) + width formatting
    println!(
        "[RS3] Vec sum={sum}  HashMap aros={} rust={}  fmt={}",
        m["aros"],
        m["rust"],
        format!("{:>6}", "ok")
    );
    // RS3c: isolate env / SystemTime / Instant, each with its own marker so a
    // single run shows exactly how far it got.
    // env read (getenv) works; the startup sets RUST_GREET via SetEnv. (AROS
    // `setenv`/SetVar fails at runtime so writes are unsupported; `time` is wired
    // but AROS `clock_gettime` faults, so neither is exercised here yet.)
    let greet = std::env::var("RUST_GREET").unwrap_or_else(|_| "<unset>".into());
    println!("[RS3c] env: getenv RUST_GREET={greet}");

    println!("RUST-AROS: STD PASS");
    0x5253_3320 // "RS3 "
}

// --- RS4: TCP round-trip over the bsdsocket bridge (aros_net_glue.c) ----------
use core::ffi::c_void;

unsafe extern "C" {
    fn aros_net_open() -> i32;
    fn aros_net_close();
    fn aros_tcp_socket() -> i32;
    fn aros_connect_v4(s: i32, addr_net: u32, port_net: u16) -> i32;
    fn aros_send(s: i32, buf: *const c_void, len: usize) -> isize;
    fn aros_recv(s: i32, buf: *mut c_void, len: usize) -> isize;
    fn aros_closesocket(s: i32);
    fn aros_sock_errno() -> i32;
}

/// 127.0.0.1:12345 in network byte order (matches socktest.c).
const ADDR_LOCALHOST_NET: u32 = 0x0100_007f;
const PORT_NET: u16 = 0x3930; // htons(12345)

/// Connect to a host echo server, send "PING42", expect it echoed back. Returns
/// the magic on a verified round-trip, a small nonzero code otherwise. No panics,
/// no unwraps: every failure path prints and cleans up.
#[no_mangle]
pub extern "C" fn aros_rust_net_test() -> u32 {
    unsafe {
        if aros_net_open() != 0 {
            println!("[NET] FAIL: cannot open bsdsocket.library");
            return 1;
        }
        let s = aros_tcp_socket();
        if s < 0 {
            println!("[NET] FAIL: socket() errno {}", aros_sock_errno());
            aros_net_close();
            return 2;
        }
        if aros_connect_v4(s, ADDR_LOCALHOST_NET, PORT_NET) < 0 {
            println!("[NET] FAIL: connect() errno {}", aros_sock_errno());
            aros_closesocket(s);
            aros_net_close();
            return 3;
        }
        let msg = b"PING42";
        if aros_send(s, msg.as_ptr() as *const c_void, msg.len()) != msg.len() as isize {
            println!("[NET] FAIL: send() errno {}", aros_sock_errno());
            aros_closesocket(s);
            aros_net_close();
            return 4;
        }
        let mut buf = [0u8; 16];
        let n = aros_recv(s, buf.as_mut_ptr() as *mut c_void, buf.len() - 1);
        aros_closesocket(s);
        aros_net_close();
        if n == 6 && &buf[..6] == b"PING42" {
            println!("[NET] PASS: rust TCP round-trip over bsdsocket echoed {:?}", &buf[..6]);
            println!("RUST-AROS: NET PASS");
            0x5253_3700 // "RS7 "
        } else {
            let got = if n < 0 { 0 } else { n as usize };
            println!("[NET] FAIL: recv()={} bytes={:?}", n, &buf[..got.min(buf.len())]);
            5
        }
    }
}
