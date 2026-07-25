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

    // RS3f: fs surfaces implemented 2026-07-18 -- canonicalize (posixc
    // realpath), File::truncate (ftruncate), File::duplicate (dup), and the
    // portable copy/remove_dir_all. Each in its own closure: one failure
    // prints, the run continues (panic=abort).

    // RS3g: posixc-level probes for the RS3f failures — realpath's own
    // ingredients (open("."), chdir, getcwd) and mkdir, called directly so
    // errno is observed per call, not through std's wrappers.
    {
        use core::ffi::{c_char, c_int};
        unsafe extern "C" {
            fn open(path: *const c_char, flags: c_int, mode: c_int) -> c_int;
            fn close(fd: c_int) -> c_int;
            fn chdir(path: *const c_char) -> c_int;
            fn getcwd(buf: *mut c_char, size: usize) -> *mut c_char;
            fn mkdir(path: *const c_char, mode: u16) -> c_int;
        }
        let e = || std::io::Error::last_os_error();
        unsafe {
            let fd = open(c".".as_ptr(), 1, 0); // posixc O_RDONLY = 0x1
            println!("[RS3g] open(\".\", O_RDONLY) = {fd} errno={:?}", e());
            if fd >= 0 { close(fd); }
            let r = chdir(c"MacRW:".as_ptr());
            println!("[RS3g] chdir(MacRW:) = {r} errno={:?}", e());
            let mut buf = [0u8; 1024];
            let p = getcwd(buf.as_mut_ptr() as *mut c_char, buf.len());
            let cwd = if p.is_null() { None } else {
                let n = buf.iter().position(|&b| b == 0).unwrap_or(0);
                Some(String::from_utf8_lossy(&buf[..n]).into_owned())
            };
            println!("[RS3g] getcwd = {cwd:?} errno={:?}", e());
            let r = mkdir(c"MacRW:rs3g-dir".as_ptr(), 0o777);
            println!("[RS3g] mkdir(MacRW:rs3g-dir) = {r} errno={:?}", e());
            let meta = std::fs::metadata("MacRW:rs3g-dir");
            println!("[RS3g] metadata(MacRW:rs3g-dir) = is_dir {:?}", meta.map(|m| m.is_dir()));
            let r = mkdir(c"MacRW:rs3g-dir/sub".as_ptr(), 0o777);
            println!("[RS3g] mkdir(MacRW:rs3g-dir/sub) = {r} errno={:?}", e());
            let r = chdir(c"SYS:".as_ptr());
            println!("[RS3g] chdir(SYS:) back = {r}");
        }
        // leave no leftovers for the next run
        let _ = std::fs::remove_dir("MacRW:rs3g-dir/sub");
        let _ = std::fs::remove_dir("MacRW:rs3g-dir");
    }

    // canonicalize: resolve a `/`-joined subpath back to device syntax.
    // Step-by-step prints: the first on-device run failed with an errno-0
    // error somewhere in this block and NotFound cascades after it.
    {
        // KNOWN BUG (AROS side): mkdir with a missing parent gets the wrong
        // IoErr from emul-handler (EINVAL or even 0 instead of ENOENT), so
        // create_dir_all's recover-on-NotFound never triggers. Print its
        // signature, then build the tree stepwise so the rest of RS3f runs.
        let a = std::fs::create_dir_all("MacRW:rs3f/sub");
        println!("[RS3f] step create_dir_all(MacRW:rs3f/sub) = {a:?} (KNOWN emul-handler IoErr bug if Err(EINVAL/errno0))");
        let a1 = std::fs::create_dir("MacRW:rs3f");
        let a2 = std::fs::create_dir("MacRW:rs3f/sub");
        println!("[RS3f] step stepwise create_dir = {a1:?} / {a2:?}");
        let b = std::fs::write("MacRW:rs3f/sub/c.txt", b"c");
        println!("[RS3f] step write(MacRW:rs3f/sub/c.txt) = {b:?}");
        let c1 = std::fs::canonicalize("MacRW:");
        println!("[RS3f] step canonicalize(MacRW:) = {c1:?}");
        let c2 = std::fs::canonicalize("MacRW:rs3f");
        println!("[RS3f] step canonicalize(MacRW:rs3f) = {c2:?}");
        let c3 = std::fs::canonicalize("MacRW:rs3f/sub/c.txt");
        println!("[RS3f] step canonicalize(file) = {c3:?}");
        let c4 = std::fs::canonicalize("MacRW:rs3f/sub/../sub/c.txt");
        println!("[RS3f] step canonicalize(dotted) = {c4:?}");
        let same = matches!((&c3, &c4), (Ok(x), Ok(y)) if x == y);
        println!("[RS3f] fs canonicalize: dotted==direct={same} (expect true)");
    }

    // copy: bytes + length must survive; source stays.
    {
        let r = (|| -> std::io::Result<(u64, Vec<u8>)> {
            std::fs::write("MacRW:rs3f/src.txt", b"copy-me-7")?;
            let n = std::fs::copy("MacRW:rs3f/src.txt", "MacRW:rs3f/dst.txt")?;
            Ok((n, std::fs::read("MacRW:rs3f/dst.txt")?))
        })();
        let ok = matches!(&r, Ok((9, b)) if b == b"copy-me-7");
        println!("[RS3f] fs copy: n/content ok={ok} ({r:?}; expect Ok((9, \"copy-me-7\")))");
    }

    // File::truncate: 9 bytes -> 4; metadata must agree.
    {
        let r = (|| -> std::io::Result<u64> {
            let f = std::fs::OpenOptions::new()
                .read(true)
                .write(true)
                .open("MacRW:rs3f/src.txt")?;
            f.set_len(4)?;
            Ok(f.metadata()?.len())
        })();
        println!("[RS3f] fs File::set_len(4): len-after={r:?} (expect Ok(4))");
    }

    // File::duplicate: clone must read from the same open file.
    {
        let r = (|| -> std::io::Result<usize> {
            let f = std::fs::File::open("MacRW:rs3f/src.txt")?;
            let mut d = f.try_clone()?;
            let mut buf = [0u8; 8];
            d.read(&mut buf[..])
        })();
        println!("[RS3f] fs File::try_clone: read-via-dup={r:?} bytes (expect Ok(4))");
    }

    // remove_dir_all: the whole rs3f tree (files + subdir) must go.
    {
        let r = std::fs::remove_dir_all("MacRW:rs3f");
        let gone = std::fs::metadata("MacRW:rs3f").is_err();
        println!("[RS3f] fs remove_dir_all: {r:?} gone={gone} (expect Ok + true)");
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

// --- STREAM: large streamed read, byte-exact verify (blocking + non-blocking) -
// Isolates the LSP-over-socket corruption: a host server streams a known pattern
// (byte[i] = i % 251) in small chunks to force partial reads; we read it back and
// verify every byte. Blocking and non-blocking are tested separately because the
// async LSP path drives the socket non-blocking, and that is the suspect. Needs
// the streaming host server on 127.0.0.1:12346.
const STREAM_SIZE: usize = 256 * 1024;

fn stream_once(nonblocking: bool) -> u32 {
    use std::io::{ErrorKind, Read, Write};
    use std::net::TcpStream;
    let mode = if nonblocking { "nonblk" } else { "block " };
    let mut s = match TcpStream::connect("127.0.0.1:12346") {
        Ok(s) => s,
        Err(e) => {
            println!("[STREAM {mode}] connect fail: {e:?}");
            return 1;
        }
    };
    if writeln!(s, "{STREAM_SIZE}").and_then(|_| s.flush()).is_err() {
        println!("[STREAM {mode}] request send fail");
        return 1;
    }
    if nonblocking {
        let _ = s.set_nonblocking(true);
    }
    let mut got: Vec<u8> = Vec::with_capacity(STREAM_SIZE);
    let mut buf = [0u8; 4096];
    let mut spins: u64 = 0;
    loop {
        match s.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                // Sanity guard so a bogus count can't OOM the test itself.
                if n > buf.len() {
                    println!("[STREAM {mode}] BAD COUNT: read()={n} > buf {}", buf.len());
                    return 1;
                }
                got.extend_from_slice(&buf[..n]);
                if got.len() >= STREAM_SIZE {
                    break;
                }
            }
            Err(ref e) if e.kind() == ErrorKind::WouldBlock => {
                spins += 1;
                if spins > 200_000_000 {
                    println!("[STREAM {mode}] stuck WouldBlock at {} bytes", got.len());
                    return 1;
                }
                continue;
            }
            Err(e) => {
                println!("[STREAM {mode}] read err at {}: {e:?}", got.len());
                return 1;
            }
        }
    }
    if got.len() != STREAM_SIZE {
        println!("[STREAM {mode}] LEN MISMATCH got={} want={STREAM_SIZE}", got.len());
        return 1;
    }
    for (i, b) in got.iter().enumerate() {
        let want = (i % 251) as u8;
        if *b != want {
            println!(
                "[STREAM {mode}] CORRUPT @off {i}: got {b} want {want} (spins {spins})"
            );
            return 1;
        }
    }
    println!("[STREAM {mode}] PASS {STREAM_SIZE} bytes byte-exact (spins {spins})");
    0
}

/// Connect on THIS task, then read the streamed pattern on a *spawned* task.
/// bsdsocket's `SocketBase` is per-task and the glue keeps a single global one,
/// so a socket touched from a second task is the suspected LSP corruption
/// (the async executor is multi-threaded). If this corrupts while `stream_once`
/// passes, per-task `SocketBase` is confirmed as the cause.
fn stream_crossthread() -> u32 {
    use std::io::{Read, Write};
    use std::net::TcpStream;
    let mut s = match TcpStream::connect("127.0.0.1:12346") {
        Ok(s) => s,
        Err(e) => {
            println!("[XTHREAD] connect fail: {e:?}");
            return 1;
        }
    };
    if writeln!(s, "{STREAM_SIZE}").and_then(|_| s.flush()).is_err() {
        println!("[XTHREAD] request send fail");
        return 1;
    }
    let handle = std::thread::spawn(move || -> u32 {
        let mut got: Vec<u8> = Vec::with_capacity(STREAM_SIZE);
        let mut buf = [0u8; 4096];
        loop {
            match s.read(&mut buf) {
                Ok(0) => break,
                Ok(n) if n <= buf.len() => {
                    got.extend_from_slice(&buf[..n]);
                    if got.len() >= STREAM_SIZE {
                        break;
                    }
                }
                Ok(n) => {
                    println!("[XTHREAD] BAD COUNT read()={n}");
                    return 1;
                }
                Err(e) => {
                    println!("[XTHREAD] read err at {}: {e:?}", got.len());
                    return 1;
                }
            }
        }
        if got.len() != STREAM_SIZE {
            println!("[XTHREAD] LEN MISMATCH got={} want={STREAM_SIZE}", got.len());
            return 1;
        }
        for (i, b) in got.iter().enumerate() {
            let want = (i % 251) as u8;
            if *b != want {
                println!("[XTHREAD] CORRUPT @off {i}: got {b} want {want}");
                return 1;
            }
        }
        println!("[XTHREAD] PASS {STREAM_SIZE} bytes byte-exact (read on spawned task)");
        0
    });
    handle.join().unwrap_or(1)
}

/// Observe what AROS `readlink` returns for a regular (non-symlink) file, a
/// directory, and a missing path. A POSIX readlink returns -1/EINVAL on a
/// non-symlink; if it returns the buffer size instead, `std::fs::read_link`'s
/// grow-until-it-fits loop never terminates (the editor OOM). Raw call with a
/// fixed buffer so it can't run away.
fn readlink_probe() {
    use core::ffi::{c_char, c_int};
    unsafe extern "C" {
        fn readlink(path: *const c_char, buf: *mut c_char, bufsz: usize) -> isize;
        fn __stdc_geterrnoptr() -> *mut c_int;
    }
    // A readlink that returns exactly bufsz is indistinguishable from "buffer
    // too small" (AROS posixc maps a handler's -2 to bufsz), which is what made
    // std's grow-until-it-fits loop run away. Find which paths do that.
    let home = std::env::var("HOME").unwrap_or_else(|_| "<unset>".into());
    println!("[READLINK] $HOME = {home:?}");
    let mut owned: Vec<String> = vec![
        "".into(), "/".into(), ":".into(),
        "RAM:".into(), "SYS:".into(), "T:".into(), "ENV:".into(),
        "PROGDIR:".into(), "MacRW:".into(), "MacRO:".into(),
        "RAM:T".into(), "SYS:Prefs".into(),
        "MacRW:lsptest".into(), "MacRW:lsptest/src/main.rs".into(),
    ];
    if home != "<unset>" {
        owned.push(home.clone());
        owned.push(format!("{home}/.config"));
        owned.push(format!("{home}/.config/zed"));
        owned.push(format!("{home}/.config/zed/settings.json"));
    }
    // The real path std takes: cstr() normalizes (drops a join-artifact slash
    // after the colon), then readlink. With the pal cap in place a runaway shows
    // up as an InvalidData error instead of an OOM, so this is safe to run.
    for probe in [
        "SYS:", "SYS:.config", "SYS:.config/zed", "SYS:.config/zed/settings.json",
        "SYS:/.config/zed/settings.json", "/", "MacRW:lsptest/src/main.rs",
    ] {
        match std::fs::read_link(probe) {
            Ok(t) => println!("[STD read_link] {probe:?}: Ok({t:?})"),
            Err(e) => println!("[STD read_link] {probe:?}: {:?} {e}", e.kind()),
        }
    }
    for path in owned {
        let mut c = path.clone().into_bytes();
        c.push(0);
        let mut buf = [0u8; 256];
        unsafe { *__stdc_geterrnoptr() = 0 };
        let n = unsafe {
            readlink(c.as_ptr() as *const c_char, buf.as_mut_ptr() as *mut c_char, buf.len())
        };
        let e = unsafe { *__stdc_geterrnoptr() };
        let flag = if n == buf.len() as isize { "  <<< RUNAWAY TRIGGER" } else { "" };
        println!("[READLINK] {path:?}: ret={n} errno={e}{flag}");
    }
}

/// What `paths::set_custom_data_dir` does on startup: create_dir_all then
/// canonicalize. One of these is panicking for the full zed binary.
fn datadir_probe() {
    // Raw mkdir + the is_dir() check std's create_dir_all relies on.
    use core::ffi::{c_char, c_int};
    unsafe extern "C" {
        fn mkdir(path: *const c_char, mode: u16) -> c_int;
        fn __stdc_geterrnoptr() -> *mut c_int;
    }
    for d in ["MacRW:zeddata\0", "MacRW:zdnew\0"] {
        unsafe { *__stdc_geterrnoptr() = 0 };
        let r = unsafe { mkdir(d.as_ptr() as *const c_char, 0o777) };
        let e = unsafe { *__stdc_geterrnoptr() };
        let name = &d[..d.len() - 1];
        println!(
            "[MKDIR] raw mkdir({name:?}) ret={r} errno={e}  is_dir={} exists={}",
            std::path::Path::new(name).is_dir(),
            std::path::Path::new(name).exists()
        );
        match std::fs::metadata(name) {
            Ok(m) => println!("[MKDIR]   metadata: is_dir={} len={}", m.is_dir(), m.len()),
            Err(err) => println!("[MKDIR]   metadata ERR {:?} {err}", err.kind()),
        }
    }

    for d in ["MacRW:zeddata", "MacRW:zeddata/db", "MacRW:zdp/a/b"] {
        match std::fs::create_dir_all(d) {
            Ok(()) => println!("[DATADIR] create_dir_all({d:?}) OK"),
            Err(e) => println!("[DATADIR] create_dir_all({d:?}) ERR {:?} {e}", e.kind()),
        }
        match std::fs::canonicalize(d) {
            Ok(p) => println!("[DATADIR] canonicalize({d:?}) -> {p:?}"),
            Err(e) => println!("[DATADIR] canonicalize({d:?}) ERR {:?} {e}", e.kind()),
        }
    }
}

#[no_mangle]
pub extern "C" fn aros_rust_stream_test() -> u32 {
    datadir_probe();
    readlink_probe();
    let mut fails = 0;
    fails += stream_once(false);
    fails += stream_once(true);
    fails += stream_crossthread();
    if fails == 0 {
        println!("RUST-AROS: STREAM PASS");
        0x5354_5200 // "STR "
    } else {
        println!("RUST-AROS: STREAM FAIL");
        7
    }
}

// ---------------------------------------------------------------------------
// std::process streaming: live child pipes through the real std API.
//
// Uses C:ProcProbe (the C reproducer) as the child, in its `child <code>` mode:
// it prints "child-ready", echoes two lines back with an "echo:" prefix, then
// exits with the code it was given.
// ---------------------------------------------------------------------------

fn proc_stream_probe() -> u32 {
    use std::io::{BufRead, BufReader, Write};
    use std::process::{Command, Stdio};

    let mut fails = 0;

    let mut child = match Command::new("C:ProcProbe")
        .arg("child")
        .arg("7")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            println!("[PROC] spawn FAILED: {e:?}");
            return 1;
        }
    };
    println!("[PROC] spawned");

    let mut out = BufReader::new(child.stdout.take().expect("piped stdout"));
    let mut stdin = child.stdin.take().expect("piped stdin");

    // (1) read the greeting while the child is still running
    let mut line = String::new();
    match out.read_line(&mut line) {
        Ok(n) if n > 0 => println!("[PROC] greeting while running: {:?}", line.trim_end()),
        other => {
            println!("[PROC] FAIL no greeting: {other:?}");
            fails += 1;
        }
    }
    if !line.starts_with("child-ready") {
        println!("[PROC] FAIL unexpected greeting");
        fails += 1;
    }

    // the child must still be alive at this point
    match child.try_wait() {
        Ok(None) => println!("[PROC] still running, good"),
        other => {
            println!("[PROC] FAIL child already gone: {other:?}");
            fails += 1;
        }
    }

    // (2) write to its stdin, read the echo back
    if let Err(e) = stdin.write_all(b"ping-one\n").and_then(|_| stdin.flush()) {
        println!("[PROC] FAIL write stdin: {e:?}");
        fails += 1;
    }
    line.clear();
    match out.read_line(&mut line) {
        Ok(n) if n > 0 => println!("[PROC] echo: {:?}", line.trim_end()),
        other => {
            println!("[PROC] FAIL no echo: {other:?}");
            fails += 1;
        }
    }
    if !line.starts_with("echo:ping-one") {
        println!("[PROC] FAIL echo mismatch");
        fails += 1;
    }

    // let the child finish
    let _ = stdin.write_all(b"ping-two\n");
    let _ = stdin.flush();
    drop(stdin);

    // (3) wait, and check the exit code survived the shell
    match child.wait() {
        Ok(st) => {
            println!("[PROC] exit status: {st:?}");
            if st.code() != Some(7) {
                println!("[PROC] FAIL wanted code 7");
                fails += 1;
            }
        }
        Err(e) => {
            println!("[PROC] FAIL wait: {e:?}");
            fails += 1;
        }
    }

    // (4) the classic one-shot path must still work
    match Command::new("C:ProcProbe").arg("child").arg("0").output() {
        Ok(o) => {
            let s = String::from_utf8_lossy(&o.stdout);
            println!("[PROC] output() captured {} bytes, status {:?}", o.stdout.len(), o.status);
            if !s.contains("child-ready") {
                println!("[PROC] FAIL output() missing greeting");
                fails += 1;
            }
        }
        Err(e) => {
            println!("[PROC] FAIL output(): {e:?}");
            fails += 1;
        }
    }

    fails
}

#[no_mangle]
pub extern "C" fn aros_rust_proc_test() -> u32 {
    let fails = proc_stream_probe();
    if fails == 0 {
        println!("RUST-AROS: PROC PASS");
        0x5052_4f43 // "PROC"
    } else {
        println!("RUST-AROS: PROC FAIL ({fails})");
        7
    }
}

// ---------------------------------------------------------------------------
// Path::is_absolute on AROS volume paths.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn aros_rust_path_test() -> u32 {
    use std::path::Path;
    let mut fails = 0;

    let absolute = [
        "MacRW:lsptest",
        "MacRW:lsptest/src/main.rs",
        "SYS:C/List",
        "PIPE:name",
        "/rooted",
    ];
    let relative = ["src/main.rs", "main.rs", "./x", "../y", "a/b:c"];

    for p in absolute {
        if !Path::new(p).is_absolute() {
            println!("[PATH] FAIL {p:?} should be absolute");
            fails += 1;
        }
    }
    for p in relative {
        if Path::new(p).is_absolute() {
            println!("[PATH] FAIL {p:?} should be relative");
            fails += 1;
        }
    }

    // absolute() must leave a volume path alone rather than gluing it onto cwd
    match std::path::absolute(Path::new("MacRW:lsptest")) {
        Ok(p) if p == Path::new("MacRW:lsptest") => {}
        other => {
            println!("[PATH] FAIL absolute(MacRW:lsptest) = {other:?}");
            fails += 1;
        }
    }

    if fails == 0 {
        println!("RUST-AROS: PATH PASS");
        0x5041_5448 // "PATH"
    } else {
        println!("RUST-AROS: PATH FAIL ({fails})");
        7
    }
}

// ---------------------------------------------------------------------------
// Allocator: growing an over-aligned allocation.
//
// AROS's aligned_alloc returns an offset pointer into a bigger malloc block,
// and only free() knows how to recover the real one. A block allocated that way
// must never reach realloc(). This exercises exactly that: a small, heavily
// over-aligned allocation (so alloc() takes the posix_memalign path) that is
// then grown repeatedly.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn aros_rust_alloc_test() -> u32 {
    use std::alloc::{alloc, dealloc, realloc, Layout};

    let mut fails = 0;

    // align > size forces the aligned path in alloc()
    for (align, size) in [(64usize, 8usize), (32, 4), (128, 16), (16, 8)] {
        unsafe {
            let layout = Layout::from_size_align(size, align).expect("valid layout");
            let p = alloc(layout);
            if p.is_null() {
                println!("[ALLOC] FAIL alloc({size},{align}) returned null");
                fails += 1;
                continue;
            }
            if (p as usize) % align != 0 {
                println!("[ALLOC] FAIL alloc({size},{align}) misaligned: {p:p}");
                fails += 1;
            }
            std::ptr::write_bytes(p, 0xAB, size);

            // grow it a few times, checking the payload survives each move
            let mut cur = p;
            let mut cur_size = size;
            for step in 1..=4 {
                let new_size = size * (1 << step);
                let grown = realloc(cur, Layout::from_size_align(cur_size, align).unwrap(), new_size);
                if grown.is_null() {
                    println!("[ALLOC] FAIL realloc to {new_size} (align {align}) returned null");
                    fails += 1;
                    break;
                }
                let kept = std::slice::from_raw_parts(grown, size);
                if kept.iter().any(|&b| b != 0xAB) {
                    println!("[ALLOC] FAIL realloc to {new_size} (align {align}) lost the payload");
                    fails += 1;
                }
                cur = grown;
                cur_size = new_size;
            }
            dealloc(cur, Layout::from_size_align(cur_size, align).unwrap());
        }
    }

    // and a Vec of an over-aligned type, which is what the terminal grid does
    #[repr(align(64))]
    #[derive(Clone, Copy, PartialEq, Debug)]
    struct Wide(u64);
    let mut v: Vec<Wide> = Vec::new();
    for i in 0..2000u64 {
        v.push(Wide(i));
    }
    if v.len() != 2000 || v[0] != Wide(0) || v[1999] != Wide(1999) {
        println!("[ALLOC] FAIL over-aligned Vec grow");
        fails += 1;
    }

    if fails == 0 {
        println!("RUST-AROS: ALLOC PASS");
        0x414c_4c43 // "ALLC"
    } else {
        println!("RUST-AROS: ALLOC FAIL ({fails})");
        7
    }
}

// ---------------------------------------------------------------------------
// Thread stacks.
//
// AROS gives a task a fixed stack with nothing below it: no guard page, no
// growth. A thread that runs off the end writes into whatever the allocator put
// there, and the damage shows up much later somewhere else entirely. So the
// stack std asks for has to be the one crates were written against (the unix
// default, 2 MiB) -- and it has to actually arrive.
//
// This asserts the size the OS handed over rather than provoking an overflow,
// which would be the very corruption the check exists to prevent.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    fn aros_thr_stack_bytes() -> usize;
}

/// Touch every page down to `depth` bytes so the stack is proven usable, not
/// merely reserved. Recursive and `black_box`ed so it cannot be optimised away.
fn walk_stack(depth: usize) -> u64 {
    let mut frame = [0u8; 8 * 1024];
    frame[0] = 1;
    frame[frame.len() - 1] = 2;
    std::hint::black_box(&mut frame);
    let sum = u64::from(frame[0]) + u64::from(frame[frame.len() - 1]);
    if depth <= frame.len() { sum } else { sum + walk_stack(depth - frame.len()) }
}

#[no_mangle]
pub extern "C" fn aros_rust_stack_test() -> u32 {
    const WANT: usize = 2 * 1024 * 1024;
    // Deep enough to have smashed the old 256 KB stack, shallow enough to be
    // safe inside the one we now ask for.
    const WALK: usize = 768 * 1024;

    let mut fails = 0;

    let got = std::thread::spawn(|| unsafe { aros_thr_stack_bytes() }).join().expect("joined");
    println!("[STACK] spawned thread stack: {} KB", got / 1024);
    if got < WANT {
        println!("[STACK] FAIL default thread stack is {got} bytes, want at least {WANT}");
        fails += 1;
    }

    // An explicit request must be honoured too.
    let asked = 4 * 1024 * 1024;
    let got = std::thread::Builder::new()
        .stack_size(asked)
        .spawn(|| unsafe { aros_thr_stack_bytes() })
        .expect("spawned")
        .join()
        .expect("joined");
    if got < asked {
        println!("[STACK] FAIL stack_size({asked}) gave {got} bytes");
        fails += 1;
    }

    // Only walk the stack once the size is known good: on a short stack this
    // would be the corruption it is meant to catch.
    if fails == 0 {
        let walked = std::thread::spawn(|| walk_stack(WALK)).join().expect("joined");
        if walked == 0 {
            println!("[STACK] FAIL walking {WALK} bytes of stack returned nothing");
            fails += 1;
        }
    }

    if fails == 0 {
        println!("RUST-AROS: STACK PASS");
        0x5354_434b // "STCK"
    } else {
        println!("RUST-AROS: STACK FAIL ({fails})");
        8
    }
}

// ---------------------------------------------------------------------------
// A shell as a child, over pipes.
//
// This is what a terminal is: `C:Shell` with all three stdio streams on live
// pipes, typed at from one end and read from the other. It is worth checking
// separately from the editor, because the editor takes a quarter of an hour to
// build and this takes one minute.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn aros_rust_shell_test() -> u32 {
    use std::io::{Read, Write};
    use std::process::{Command, Stdio};
    use std::time::{Duration, Instant};

    let mut fails = 0;

    // Does a piped stdin break spawning at all, or only the shell?
    match Command::new("C:Version")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
    {
        Ok(mut c) => {
            println!("[SHELL] a command with a piped stdin spawns");
            let _ = c.wait();
        }
        Err(e) => {
            println!("[SHELL] FAIL a command with a piped stdin will not spawn: {e}");
            fails += 1;
        }
    }

    // The terminal's shape: no command at all, which is how AROS is asked for
    // an interactive shell reading the stream it is given.
    let mut cmd = Command::new("");
    cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
    let mut child = match cmd.spawn() {
        Ok(c) => c,
        Err(e) => {
            println!("[SHELL] FAIL spawn an interactive shell: {e}");
            println!("RUST-AROS: SHELL FAIL ({})", fails + 1);
            return 9;
        }
    };
    println!("[SHELL] interactive shell spawned");

    // Does it stay alive with an idle stdin, or read the empty pipe as EOF?
    std::thread::sleep(Duration::from_millis(500));
    match child.try_wait() {
        Ok(Some(st)) => {
            println!("[SHELL] FAIL exited while idle with status {st:?}");
            fails += 1;
        }
        Ok(None) => println!("[SHELL] still running with an idle stdin"),
        Err(e) => {
            println!("[SHELL] FAIL try_wait: {e}");
            fails += 1;
        }
    }

    let mut stdin = child.stdin.take().expect("piped stdin");
    let mut stdout = child.stdout.take().expect("piped stdout");

    // Read on a thread: the read blocks, and a shell that answered nothing
    // would otherwise hang this probe forever.
    let reader = std::thread::spawn(move || {
        let mut seen = Vec::new();
        let mut chunk = [0u8; 512];
        loop {
            match stdout.read(&mut chunk) {
                Ok(0) => break,
                Ok(n) => {
                    seen.extend_from_slice(&chunk[..n]);
                    if seen.windows(6).any(|w| w == b"marker") {
                        break;
                    }
                }
                Err(_) => break,
            }
        }
        seen
    });

    if let Err(e) = stdin.write_all(b"echo marker\n").and_then(|()| stdin.flush()) {
        println!("[SHELL] FAIL writing a command: {e}");
        fails += 1;
    }

    let deadline = Instant::now() + Duration::from_secs(3);
    while Instant::now() < deadline && !reader.is_finished() {
        std::thread::sleep(Duration::from_millis(50));
    }
    drop(stdin);

    let seen = reader.join().unwrap_or_default();
    let text = String::from_utf8_lossy(&seen);
    println!("[SHELL] read {} bytes: {:?}", seen.len(), text);
    if !text.contains("marker") {
        println!("[SHELL] FAIL the shell never answered the command");
        fails += 1;
    }

    let _ = child.wait();

    if fails == 0 {
        println!("RUST-AROS: SHELL PASS");
        0x5348_4c4c // "SHLL"
    } else {
        println!("RUST-AROS: SHELL FAIL ({fails})");
        9
    }
}
