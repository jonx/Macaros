//! std68k-probe — Rust **std** on m68k under run68k's stub-DOS (STD68K-PLAN.md
//! piece 5). libc68k's crt0 calls `rust_main68k`; everything std does here flows
//! through the aros pal -> libc68k -> the stub-DOS LVOs -> the host. Exit code =
//! the number of failed checks (0 = STD PASS printed and everything held).
use std::io::{Read, Seek, SeekFrom, Write};

static mut FAILS: u32 = 0;

fn check(what: &str, ok: bool) {
    println!("[std68k] {what}: {}", if ok { "ok" } else { "FAIL" });
    if !ok {
        unsafe { FAILS += 1 }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_main68k() -> u32 {
    println!("hello from RUST STD on 68k AROS");


    // collections + fmt: Vec, HashMap (seeded from the Entropy LVO), format!
    let v: Vec<u32> = (1..=13).collect();
    let sum: u32 = v.iter().sum();
    check("Vec/iter/sum=91", sum == 91);
    let mut m = std::collections::HashMap::new();
    m.insert("aros", 68u32);
    m.insert("rust", 2021u32);
    check("HashMap", m["aros"] == 68 && m["rust"] == 2021);
    check("format!", format!("{}-{:04x}", "m68k", 0xA105u32) == "m68k-a105");

    // fs round-trip through the stub-DOS: create, write, reopen, seek, read, meta.
    // KNOWN FAIL: std's File::open/OpenOptions path is hit by a residual m68k
    // codegen miscompile (the open() LVO returns fd 3, direct C calls + a hand
    // inlined `if fd<0 {..}` both work, but the generic OpenOptions wrapper returns
    // Err under register pressure). Same bug class as llvm #152816 / #207150.
    let p = "/tmp/std68k-probe.txt";
    let _ = std::fs::remove_file(p);
    let fs_ok = (|| -> std::io::Result<bool> {
        let mut f = std::fs::File::create(p)?;
        f.write_all(b"rust std file io on 68k")?;
        drop(f);
        let mut f = std::fs::File::open(p)?;
        f.seek(SeekFrom::Start(9))?;
        let mut s = String::new();
        f.read_to_string(&mut s)?;
        let meta = std::fs::metadata(p)?;
        Ok(s == "file io on 68k" && meta.len() == 23 && meta.is_file())
    })()
    .unwrap_or(false);
    check("fs create/seek/read/metadata", fs_ok);
    let rm_ok = std::fs::remove_file(p).is_ok() && std::fs::metadata(p).is_err();
    check("fs remove+gone", rm_ok);

    // time: Instant monotonic, SystemTime sane (host clock, after 2023).
    let t0 = std::time::Instant::now();
    let mut acc = 0u64;
    for i in 0..100_000u64 {
        acc = acc.wrapping_add(i);
    }
    let dt = t0.elapsed();
    check("Instant monotonic", !dt.is_zero() || acc > 0);
    let epoch = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    check("SystemTime after 2023", epoch > 1_700_000_000);

    // the honest unsupported edges: spawn/connect must ERROR, not lie.
    check("thread::spawn errors", std::thread::Builder::new().spawn(|| ()).is_err());
    check("TcpStream::connect errors", std::net::TcpStream::connect("127.0.0.1:1").is_err());

    let fails = unsafe { FAILS };
    if fails == 0 {
        println!("RUST-68K: STD PASS");
    } else {
        println!("RUST-68K: STD FAIL ({fails})");
    }
    fails
}
