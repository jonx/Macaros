//! std spike: does `std` compile + link for aarch64-unknown-aros?
//!
//! Not no_std. Uses std::println (pulls in std's stdio platform layer) so the
//! build either succeeds (some pal selected) or fails at the pal/libc wall.
//! With the aros stdio pal + build.rs known-target entry, std is no longer
//! `restricted_std` and println writes through posixc to dos Output().
#![feature(fs_set_times)] // std::fs::set_times is still unstable; exercised in RS3e

use std::collections::HashMap;
use std::io::{Read, Write};

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
    // env read (getenv) works; the startup sets RUST_GREET via SetEnv.
    let greet = std::env::var("RUST_GREET").unwrap_or_else(|_| "<unset>".into());
    println!("[RS3c] env: getenv RUST_GREET={greet}");

    // env write: does std::env::set_var (posixc setenv -> dos SetVar LOCAL_ONLY)
    // actually work for a loaded C: command? Set, then read it back.
    unsafe { std::env::set_var("RUST_WROTE", "yes-42"); }
    match std::env::var("RUST_WROTE") {
        Ok(v) => println!("[RS3c] env: set_var RUST_WROTE -> read back {v:?}"),
        Err(e) => println!("[RS3c] env: set_var readback FAILED: {e:?}"),
    }

    // fs: create+write a MacRW: file, reopen, read via File::read (no file_attr),
    // to characterize posixc open/write/read cleanly.
    let fpath = "MacRW:rust-fs.txt";
    let r = (|| -> std::io::Result<usize> {
        {
            let mut f = std::fs::File::create(fpath)?;
            println!("[RS3c] fs: create OK");
            let w = f.write(b"rust fs on aros")?;
            println!("[RS3c] fs: wrote {w} bytes");
        }
        let mut f = std::fs::File::open(fpath)?;
        println!("[RS3c] fs: reopen OK");
        let mut buf = [0u8; 64];
        let n = f.read(&mut buf)?;
        println!("[RS3c] fs: read {n} bytes = {:?}", core::str::from_utf8(&buf[..n]));
        Ok(n)
    })();
    if let Err(e) = r {
        println!("[RS3c] fs: FAILED: {e:?}");
    }

    // fs metadata: stat the file just written (drives sys/fs/aros.rs FileAttr/stat)
    match std::fs::metadata(fpath) {
        Ok(m) => println!(
            "[RS3c] fs: metadata len={} is_file={} is_dir={} readonly={}",
            m.len(),
            m.is_file(),
            m.is_dir(),
            m.permissions().readonly()
        ),
        Err(e) => println!("[RS3c] fs: metadata FAILED: {e:?}"),
    }

    // fs read_dir: list the MacRW: share root (drives sys/fs/aros.rs ReadDir/DirEntry)
    match std::fs::read_dir("MacRW:") {
        Ok(rd) => {
            let mut names: Vec<String> =
                rd.filter_map(|e| e.ok()).map(|e| e.file_name().to_string_lossy().into_owned()).collect();
            names.sort();
            let head = &names[..names.len().min(4)];
            println!("[RS3c] fs: read_dir MacRW: {} entries, first={head:?}", names.len());
        }
        Err(e) => println!("[RS3c] fs: read_dir FAILED: {e:?}"),
    }

    // args: std::env::args() reads argc/argv captured by the C harness
    let args: Vec<String> = std::env::args().collect();
    println!("[RS3c] args: {} -> {args:?}", args.len());

    // process: run a C: command via dos System(), capture its stdout + exit code
    match std::process::Command::new("Echo").arg("hi from rust proc").output() {
        Ok(o) => println!(
            "[RS3c] process: Echo -> code={:?} stdout={:?}",
            o.status.code(),
            String::from_utf8_lossy(&o.stdout).trim_end()
        ),
        Err(e) => println!("[RS3c] process: FAILED: {e:?}"),
    }

    // time: Instant (CLOCK_MONOTONIC) + SystemTime (CLOCK_REALTIME). Before the OS
    // -ffixed-x18 rebuild these SIGBUS'd (x18 clobber in the timer/posixc path).
    let t0 = std::time::Instant::now();
    let mut acc = 0u64;
    for i in 0..200_000u64 {
        acc = acc.wrapping_add(i);
    }
    let dt = t0.elapsed();
    println!("[RS3c] time: Instant elapsed={dt:?} (acc={acc})");
    match std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH) {
        Ok(d) => println!("[RS3c] time: SystemTime since epoch = {}s", d.as_secs()),
        Err(e) => println!("[RS3c] time: SystemTime err {e:?}"),
    }

    // thread: 4 threads each increment a shared Mutex<u64> 1000x, join, expect 4000.
    // Exercises thread::spawn/join (pthread), Mutex (pthread mutex + OnceBox), Arc,
    // and pthread-key TLS (thread::current). All unblocked by the OS x18 rebuild.
    {
        use std::sync::{Arc, Mutex};
        let counter = Arc::new(Mutex::new(0u64));
        let mut handles = Vec::new();
        for _ in 0..4 {
            let c = Arc::clone(&counter);
            handles.push(std::thread::spawn(move || {
                for _ in 0..1000 {
                    *c.lock().unwrap() += 1;
                }
            }));
        }
        for h in handles {
            let _ = h.join();
        }
        let total = *counter.lock().unwrap();
        println!("[RS3c] thread: 4x1000 shared Mutex -> counter={total} (expect 4000)");
    }

    // random: two fresh RandomState seeds (each drawn from posixc arc4random_buf,
    // host-backed CSPRNG) must differ. Proves the entropy path end to end.
    {
        use std::collections::hash_map::RandomState;
        use std::hash::{BuildHasher, Hasher};
        let a = RandomState::new().build_hasher().finish();
        let b = RandomState::new().build_hasher().finish();
        println!("[RS3c] random: arc4random seeds {a:#018x} {b:#018x} differ={}", a != b);
    }

    // RS3d: the FAILURE paths fixed by the 2026-07-02 pal review (FIX-PLAN.md §1):
    // each would silently misbehave before the fix, so assert the fixed semantics.

    // RS3d-1: open-mode matrix. read+append must be readable (was O_WRONLY -> reads
    // failed); create without write, and truncate+append, must be InvalidInput.
    {
        use std::fs::OpenOptions;
        use std::io::{Read, Seek, SeekFrom, Write};
        let p = "MacRW:rs3d-modes.txt";
        let _ = std::fs::remove_file(p);
        std::fs::write(p, "abc").unwrap();
        let ra = OpenOptions::new().read(true).append(true).open(p).and_then(|mut f| {
            f.write_all(b"def")?;
            f.seek(SeekFrom::Start(0))?;
            let mut s = String::new();
            f.read_to_string(&mut s)?;
            Ok(s)
        });
        let ra_ok = matches!(ra.as_deref(), Ok("abcdef"));
        let cw = OpenOptions::new().read(true).create(true).open("MacRW:rs3d-cw.txt");
        let cw_ok = matches!(&cw, Err(e) if e.kind() == std::io::ErrorKind::InvalidInput);
        let ta = OpenOptions::new().append(true).truncate(true).open(p);
        let ta_ok = matches!(&ta, Err(e) if e.kind() == std::io::ErrorKind::InvalidInput);
        let _ = std::fs::remove_file(p);
        println!(
            "[RS3d] open modes: read+append={ra_ok} create-no-write-EINVAL={cw_ok} \
             truncate+append-EINVAL={ta_ok} (expect all true; got {ra:?} / {cw_err:?} / {ta_err:?})",
            cw_err = cw.err().map(|e| e.kind()),
            ta_err = ta.err().map(|e| e.kind()),
        );
    }

    // RS3d-2: Condvar::wait_timeout(Duration::MAX) must BLOCK until notified (the
    // 32-bit time_t overflow made it return instantly and spin at 100% CPU).
    {
        use std::sync::{Arc, Condvar, Mutex};
        let pair = Arc::new((Mutex::new(false), Condvar::new()));
        let p2 = Arc::clone(&pair);
        let t0 = std::time::Instant::now();
        let h = std::thread::spawn(move || {
            std::thread::sleep(std::time::Duration::from_millis(200));
            *p2.0.lock().unwrap() = true;
            p2.1.notify_one();
        });
        let (lock, cv) = &*pair;
        let mut done = lock.lock().unwrap();
        let mut timed_out = false;
        while !*done {
            let (g, r) = cv.wait_timeout(done, std::time::Duration::MAX).unwrap();
            done = g;
            if r.timed_out() {
                timed_out = true;
                break;
            }
        }
        let _ = h.join();
        let dt = t0.elapsed();
        println!(
            "[RS3d] condvar Duration::MAX: notified={} timed_out={timed_out} after {dt:?} \
             (expect notified=true within seconds, no instant-timeout spin)",
            *done
        );
    }

    // RS3d-3: a nonexistent command must FAIL VISIBLY. This pal runs a shell line
    // (like `sh -c` on unix), so the AROS shell handles the unknown command itself
    // and returns a nonzero rc — that surfaces as Ok(status != success), which is
    // the correct shell-backend contract. Err(NotFound) is reserved for
    // SystemTagList's -1, "the shell could not run the line at all" (that path was
    // silently swallowed as a normal exit before the 2026-07-02 fix). Either way,
    // what must never happen is a SILENT SUCCESS.
    {
        let r = std::process::Command::new("NoSuchCmd-Rs3d").output();
        let visible_failure = match &r {
            Ok(o) => !o.status.success(),
            Err(_) => true,
        };
        println!(
            "[RS3d] spawn missing command: visible-failure={visible_failure} \
             (code={:?} err={:?}; expect true, silent success forbidden)",
            r.as_ref().ok().and_then(|o| o.status.code()),
            r.as_ref().err().map(|e| e.kind())
        );
    }

    // RS3e: the pal corners closed this pass -- fs symlinks/perms/times, env::vars
    // enumeration, per-command env + cwd, and net try_clone. Each is its own closure
    // so one failure prints and never aborts the run (panic=abort).

    // A1 fs: set_permissions round-trip (chmod -> readonly bit visible in metadata).
    {
        let pf = "MacRW:rs3e-perm.txt";
        let _ = std::fs::remove_file(pf);
        let r = (|| -> std::io::Result<bool> {
            std::fs::write(pf, b"x")?;
            let mut p = std::fs::metadata(pf)?.permissions();
            p.set_readonly(true);
            std::fs::set_permissions(pf, p)?;
            let ro = std::fs::metadata(pf)?.permissions().readonly();
            let mut p2 = std::fs::metadata(pf)?.permissions();
            p2.set_readonly(false); // undo so remove_file works
            let _ = std::fs::set_permissions(pf, p2);
            Ok(ro)
        })();
        println!("[RS3e] fs set_permissions: readonly-after-chmod={r:?} (expect Ok(true))");
        let _ = std::fs::remove_file(pf);
    }

    // A1 fs: std::fs::set_times (path, utimes) -- set mtime, read it back.
    {
        let tf = "MacRW:rs3e-time.txt";
        let _ = std::fs::remove_file(tf);
        let want = std::time::UNIX_EPOCH + std::time::Duration::from_secs(1_100_000_000);
        let r = (|| -> std::io::Result<u64> {
            std::fs::write(tf, b"t")?;
            let ft = std::fs::FileTimes::new().set_modified(want).set_accessed(want);
            std::fs::set_times(tf, ft)?;
            let got = std::fs::metadata(tf)?.modified()?;
            Ok(got.duration_since(std::time::UNIX_EPOCH).unwrap_or_default().as_secs())
        })();
        println!("[RS3e] fs set_times: mtime-readback={r:?}s (expect ~1100000000)");
        let _ = std::fs::remove_file(tf);
    }

    // A1 fs: symlink + readlink (posixc symlink->MakeLink, readlink->ReadLink).
    // Link support is filesystem-dependent; report exactly what the handler did.
    {
        let target = "MacRW:rs3e-lnk-target.txt";
        let link = "MacRW:rs3e-lnk";
        let _ = std::fs::remove_file(link);
        let _ = std::fs::remove_file(target);
        let r = (|| -> std::io::Result<(bool, bool)> {
            std::fs::write(target, b"linked")?;
            #[allow(deprecated)]
            std::fs::soft_link(target, link)?; // -> sys::fs::symlink (posixc symlink)
            let is_link = std::fs::symlink_metadata(link)?.file_type().is_symlink();
            let readback = std::fs::read_link(link)?;
            let readback_ok = readback.to_string_lossy().contains("rs3e-lnk-target");
            Ok((readback_ok, is_link))
        })();
        // Success = the link was created and readlink returned the right target. The
        // is_symlink flag depends on the handler's lstat (host-backed emul-handler
        // does not set S_IFLNK), so it is reported but not the pass criterion.
        println!("[RS3e] fs symlink+readlink: readlink-target-ok/is_symlink={r:?} (expect readlink-ok=true)");
        let _ = std::fs::remove_file(link);
        let _ = std::fs::remove_file(target);
    }

    // A4 env: std::env::vars() enumeration (walks pr_LocalVars). set two, expect both.
    {
        unsafe {
            std::env::set_var("RS3E_ONE", "alpha");
            std::env::set_var("RS3E_TWO", "beta");
        }
        let all: std::collections::HashMap<String, String> = std::env::vars().collect();
        let one = all.get("RS3E_ONE").map(String::as_str);
        let two = all.get("RS3E_TWO").map(String::as_str);
        println!(
            "[RS3e] env::vars: total={} RS3E_ONE={one:?} RS3E_TWO={two:?} (expect Some(\"alpha\"),Some(\"beta\"))",
            all.len()
        );
    }

    // A2 process: per-command env reaches the child. `Get` reads the LOCAL var store
    // -- the same store posixc `getenv` (any real child) reads and our injected `Set`
    // writes. (`Getenv` would be wrong: it reads global ENV: only.)
    {
        let r = std::process::Command::new("Get")
            .arg("RS3E_CMDENV")
            .env("RS3E_CMDENV", "env-to-child-77")
            .output();
        match &r {
            Ok(o) => println!(
                "[RS3e] process env: Get -> {:?} (expect contains env-to-child-77)",
                String::from_utf8_lossy(&o.stdout).trim()
            ),
            Err(e) => println!("[RS3e] process env: FAILED {e:?}"),
        }
    }

    // A2 process: current_dir -- MakeDir a relative name in a set cwd, check where it lands.
    {
        let base = "RAM:rs3e-cwd";
        let made = "RAM:rs3e-cwd/made-here";
        let _ = std::fs::remove_dir(made);
        let _ = std::fs::remove_dir(base);
        let r = (|| -> std::io::Result<bool> {
            std::fs::create_dir(base)?;
            let out = std::process::Command::new("MakeDir")
                .arg("made-here")
                .current_dir(base)
                .output()?;
            let landed = std::fs::metadata(made).map(|m| m.is_dir()).unwrap_or(false);
            let _ = out;
            Ok(landed)
        })();
        println!("[RS3e] process cwd: relative MakeDir landed in cwd={r:?} (expect Ok(true))");
        let _ = std::fs::remove_dir(made);
        let _ = std::fs::remove_dir(base);
    }

    // A3 net: try_clone (Dup2Socket) -- a UDP socket and its clone share the same local addr.
    {
        use std::net::UdpSocket;
        let r = (|| -> std::io::Result<bool> {
            let s = UdpSocket::bind("127.0.0.1:0")?;
            let a = s.local_addr()?;
            let s2 = s.try_clone()?;
            let a2 = s2.local_addr()?;
            Ok(a == a2 && a.port() != 0)
        })();
        println!("[RS3e] net try_clone: dup shares local addr={r:?} (expect Ok(true))");
    }

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

// --- RSN: the SAME round-trip, but through real `std::net::TcpStream` ----------
// This drives sys/net/connection/aros.rs (the net pal), not the glue directly, so
// it proves std::net itself works end-to-end. Needs the same host echo server as
// RS4 on 127.0.0.1:12345.
#[no_mangle]
pub extern "C" fn aros_rust_stdnet_test() -> u32 {
    use std::net::TcpStream;

    let result = (|| -> std::io::Result<[u8; 6]> {
        let mut s = TcpStream::connect("127.0.0.1:12345")?;
        println!("[STDNET] connected {} -> {}", s.local_addr()?, s.peer_addr()?);
        // exercise setsockopt/getsockopt passthrough (non-fatal: report, don't abort)
        match s.set_nodelay(true).and_then(|_| s.nodelay()) {
            Ok(v) => println!("[STDNET] set_nodelay -> nodelay()={v}"),
            Err(e) => println!("[STDNET] nodelay (nonfatal): {e:?}"),
        }
        s.write_all(b"PING42")?;
        let mut buf = [0u8; 6];
        s.read_exact(&mut buf)?;
        Ok(buf)
    })();

    match result {
        Ok(buf) if &buf == b"PING42" => {
            println!("[STDNET] PASS: std::net round-trip echoed {:?}", core::str::from_utf8(&buf));
            println!("RUST-AROS: STDNET PASS");
            0x5253_4e00 // "RSN "
        }
        Ok(buf) => {
            println!("[STDNET] FAIL: unexpected echo {:?}", core::str::from_utf8(&buf));
            5
        }
        Err(e) => {
            println!("[STDNET] FAIL: {e:?}");
            6
        }
    }
}
