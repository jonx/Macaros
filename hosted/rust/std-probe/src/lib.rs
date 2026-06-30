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
    println!("RUST-AROS: STD PASS");
    0x5253_3320 // "RS3 "
}
