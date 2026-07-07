// PR ARTIFACT (not built here): the `aros` module for the `libc` crate.
//
// Destination in a rust-lang/libc fork:
//     src/aros/mod.rs   (a top-level OS module; AROS is NOT unix-family here -- the
//     std pal does not build on sys/unix, so libc's aros is standalone like `hermit`)
// Gate it in src/lib.rs:
//     } else if #[cfg(target_os = "aros")] {
//         mod aros;
//         pub use self::aros::*;
//
// Scope: exactly the posixc / pthread / stdc surface the std pal declares today, so
// migrating the pal is 1:1 -- each `mod c { unsafe extern "C" { ... } }` block in
// library/std/src/sys/*/aros.rs is deleted and replaced with `use libc::*`. Values are
// AROS-specific (from ../aros-upstream compiler/crt/posixc headers), NOT NetBSD/unix
// defaults: O_RDONLY is 1 (not 0), mode_t is 16-bit, time_t is 32-bit.
//
// The bsdsocket surface (socket/connect/recv/... /Dup2Socket/WaitSelect) is
// deliberately NOT here: those LVOs dispatch through SocketBase in a register (Amiga
// library calling convention), so they cannot be plain `extern "C"` symbols. They stay
// behind hosted/rust/aros_net_glue.c. Likewise the `aros_*` glue helpers (stat, dir
// listing, utimes, env enumeration, process, sync) are project glue, not libc.

#![allow(non_camel_case_types)]

pub use core::ffi::{c_char, c_int, c_long, c_void};

// -- scalar types (AROS LP64: __WORDSIZE == 64) --------------------------------
pub type size_t = usize;
pub type ssize_t = isize;
pub type mode_t = u16; //  <-- 16-bit on AROS (posixc <sys/types.h>)
pub type off_t = i64; //   64-bit (__WORDSIZE == 64)
pub type pid_t = i32;
pub type time_t = i32; //  <-- 32-bit on AROS (the REALTIME/RTC caveat lives here)
pub type suseconds_t = c_long;
pub type nlink_t = u32;
pub type ino_t = u64;
pub type pthread_key_t = c_int;

// -- structs -------------------------------------------------------------------
// AROS `struct timespec` is { time_t tv_sec; long tv_nsec; } => { i32; i64 } with 4
// bytes of tail padding before tv_nsec on LP64. std/sys/time/aros.rs relies on this.
#[repr(C)]
pub struct timespec {
    pub tv_sec: time_t,
    pub tv_nsec: c_long,
}

#[repr(C)]
pub struct timeval {
    pub tv_sec: time_t,
    pub tv_usec: suseconds_t,
}

// -- open() flags (AROS-specific; O_RDONLY is 1, NOT 0) ------------------------
pub const O_RDONLY: c_int = 0x0001;
pub const O_WRONLY: c_int = 0x0002;
pub const O_RDWR: c_int = 0x0003;
pub const O_ACCMODE: c_int = 0x0003;
pub const O_CREAT: c_int = 0x0040;
pub const O_EXCL: c_int = 0x0080;
pub const O_TRUNC: c_int = 0x0200;
pub const O_APPEND: c_int = 0x0400;

pub const SEEK_SET: c_int = 0;
pub const SEEK_CUR: c_int = 1;
pub const SEEK_END: c_int = 2;

// st_mode type bits (standard octal values; posixc <sys/stat.h>)
pub const S_IFMT: u32 = 0o170000;
pub const S_IFREG: u32 = 0o100000;
pub const S_IFDIR: u32 = 0o040000;
pub const S_IFLNK: u32 = 0o120000;

// clockid_t values used by the pal
pub const CLOCK_MONOTONIC: c_int = 0;
pub const CLOCK_REALTIME: c_int = 2;

// errno (NetBSD numbering, which AROS stdc uses) -- the subset the pal maps
pub const EINVAL: c_int = 22;
pub const ENOENT: c_int = 2;
pub const EACCES: c_int = 13;
pub const EEXIST: c_int = 17;
pub const ENOTDIR: c_int = 20;
pub const EISDIR: c_int = 21;
pub const ENOSPC: c_int = 28;
pub const EROFS: c_int = 30;

// dirent d_type values (posixc <dirent.h>)
pub const DT_DIR: u32 = 4;
pub const DT_REG: u32 = 8;
pub const DT_LNK: u32 = 10;

unsafe extern "C" {
    // allocation (sys/alloc/aros.rs)
    pub fn malloc(size: size_t) -> *mut c_void;
    pub fn calloc(nmemb: size_t, size: size_t) -> *mut c_void;
    pub fn realloc(ptr: *mut c_void, size: size_t) -> *mut c_void;
    pub fn free(ptr: *mut c_void);
    pub fn posix_memalign(memptr: *mut *mut c_void, align: size_t, size: size_t) -> c_int;

    // raw I/O (sys/stdio/aros.rs, sys/fs/aros.rs)
    pub fn read(fd: c_int, buf: *mut c_void, count: size_t) -> ssize_t;
    pub fn write(fd: c_int, buf: *const c_void, count: size_t) -> ssize_t;
    pub fn open(path: *const c_char, flags: c_int, ...) -> c_int; // variadic mode_t
    pub fn close(fd: c_int) -> c_int;
    pub fn lseek(fd: c_int, offset: off_t, whence: c_int) -> off_t;

    // fs metadata / mutation (sys/fs/aros.rs)
    pub fn unlink(path: *const c_char) -> c_int;
    pub fn rename(old: *const c_char, new: *const c_char) -> c_int;
    pub fn mkdir(path: *const c_char, mode: mode_t) -> c_int;
    pub fn rmdir(path: *const c_char) -> c_int;
    pub fn chmod(path: *const c_char, mode: mode_t) -> c_int;
    pub fn fchmod(fd: c_int, mode: mode_t) -> c_int;
    pub fn symlink(target: *const c_char, linkpath: *const c_char) -> c_int;
    pub fn readlink(path: *const c_char, buf: *mut c_char, bufsiz: size_t) -> ssize_t;
    pub fn utimes(path: *const c_char, times: *const timeval) -> c_int;

    // environment (sys/env/aros.rs) -- local vars via SetVar/GetVar under the hood
    pub fn getenv(name: *const c_char) -> *mut c_char;
    pub fn setenv(name: *const c_char, value: *const c_char, overwrite: c_int) -> c_int;
    pub fn unsetenv(name: *const c_char) -> c_int;

    // time (sys/time/aros.rs)
    pub fn clock_gettime(clk: c_int, tp: *mut timespec) -> c_int;

    // errno + messages (sys/io/error/aros.rs). __stdc_geterrnoptr is AROS stdc's
    // per-opener errno accessor (see FIX-PLAN M1: process-global today).
    pub fn __stdc_geterrnoptr() -> *mut c_int;
    pub fn strerror(errnum: c_int) -> *const c_char;

    // randomness (sys/random/aros.rs) -- host CSPRNG on hosted, weak fallback native
    pub fn arc4random_buf(buf: *mut c_void, nbytes: size_t);

    // pthread-key TLS (sys/thread_local/key/aros.rs). Destructor is Option<fn> so
    // `None` is a valid (null-pointer-optimized) value, not UB.
    pub fn pthread_key_create(
        key: *mut pthread_key_t,
        destructor: Option<unsafe extern "C" fn(*mut c_void)>,
    ) -> c_int;
    pub fn pthread_key_delete(key: pthread_key_t) -> c_int;
    pub fn pthread_getspecific(key: pthread_key_t) -> *mut c_void;
    pub fn pthread_setspecific(key: pthread_key_t, value: *const c_void) -> c_int;
}
