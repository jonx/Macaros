//! getrandom 0.4 custom backend for AROS: forward to posixc `arc4random_buf`
//! (real CSPRNG since the upstream entropy.resource merge; infallible by
//! contract). Selected by `--cfg getrandom_backend="custom"` in
//! .cargo/config.toml. This will move into a shared aros support crate once
//! more than the probe needs it.
#![cfg(target_os = "aros")]

use core::ffi::c_void;

extern "C" {
    fn arc4random_buf(buf: *mut c_void, nbytes: usize);
}

#[no_mangle]
unsafe extern "Rust" fn __getrandom_v03_custom(
    dest: *mut u8,
    len: usize,
) -> Result<(), getrandom::Error> {
    arc4random_buf(dest.cast(), len);
    Ok(())
}
