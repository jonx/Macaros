//! RustHello -- a friendly `std` demo bundled with Macaros.
//!
//! Real Rust `std` running on AROS, hosted on Apple Silicon. C owns AROS startup
//! (`hello_main.c`) and calls `rust_hello_main`; everything below goes through the
//! real standard library and the `aros` platform layer (`sys/*/aros.rs`).

use std::io::Write;

#[no_mangle]
pub extern "C" fn rust_hello_main() -> i32 {
    println!("+-----------------------------------------------+");
    println!("|  Hello from Rust std -- running on AROS!       |");
    println!("|  arm64 / Apple Silicon, hosted by Macaros      |");
    println!("+-----------------------------------------------+");
    println!("Each line below is a live standard-library feature:");
    println!();

    // args -- std::env::args() reads the shell command line
    let args: Vec<String> = std::env::args().collect();
    println!("  args      : {args:?}");

    // Vec + iterators + the global allocator
    let squares: Vec<u64> = (1..=8).map(|n| n * n).collect();
    let sum: u64 = squares.iter().sum();
    println!("  iterators : squares of 1..=8 = {squares:?}  sum = {sum}");

    // env -- override with `SetEnv RUST_DEMO_WHO <name>` before running
    let who = std::env::var("RUST_DEMO_WHO").unwrap_or_else(|_| "friend".into());
    println!("  env       : RUST_DEMO_WHO = {who:?}");

    // time -- a monotonic Instant around a little work
    let t0 = std::time::Instant::now();
    let mut acc = 0u64;
    for i in 0..100_000u64 {
        acc = acc.wrapping_add(i);
    }
    println!("  time      : summed 0..100k in {:?} (acc = {acc})", t0.elapsed());

    // fs -- list the MacRW: host share, and leave a keepsake file on it
    match std::fs::read_dir("MacRW:") {
        Ok(rd) => println!("  fs        : MacRW: host share has {} entries", rd.count()),
        Err(e) => println!("  fs        : could not read MacRW: ({e})"),
    }
    let note = "MacRW:rust-was-here.txt";
    match std::fs::File::create(note) {
        Ok(mut f) => {
            let _ = writeln!(f, "Rust std wrote this on AROS. Hi from Macaros! ({} args seen)", args.len());
            println!("  fs        : wrote {note} on the Mac share");
        }
        Err(e) => println!("  fs        : could not write {note} ({e})"),
    }

    // threads -- three real threads over pthread.library
    let handles: Vec<_> = (1..=3).map(|id| std::thread::spawn(move || id * 100)).collect();
    let results: Vec<i32> = handles.into_iter().filter_map(|h| h.join().ok()).collect();
    println!("  threads   : 3 threads returned {results:?}");

    println!();
    println!("Rust std on AROS: it just works. Enjoy Macaros! :)");
    0
}
