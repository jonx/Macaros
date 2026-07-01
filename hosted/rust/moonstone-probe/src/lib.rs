//! Moonstone-on-AROS probe (stage 1): render a REAL game frame headless to a PNG.
//!
//! This runs the game's own software renderer (PIV decode -> palette ->
//! SoftwareCanvas, the 320x200 RGBA framebuffer) on booted AROS and writes the frame
//! out as a PNG (the game's dependency-free encoder) to `MacRW:`. It is the exact
//! render path the live shim will use; only "present" differs (PNG file here, blit to
//! the AROS screen there). The game crates are unmodified.

use moonstone_formats::piv::PivImage;
use moonstone_render::canvas::SoftwareCanvas;
use moonstone_render::png;
use std::path::Path;

const IN_PIV: &str = "MacRW:moonstone/CH.PIV";
const OUT_PNG: &str = "MacRW:moonstone-frame.png";

#[no_mangle]
pub extern "C" fn aros_moonstone_render() -> u32 {
    println!("[moonstone] software renderer on AROS: loading {IN_PIV}");
    let bytes = match std::fs::read(IN_PIV) {
        Ok(b) => {
            println!("[moonstone] read {} bytes", b.len());
            b
        }
        Err(e) => {
            println!("[moonstone] read FAILED: {e:?}");
            return 1;
        }
    };

    let piv = PivImage::load(&bytes);
    println!("[moonstone] PIV decoded: {} palette entries", piv.palette.len());

    let mut canvas = SoftwareCanvas::new();
    canvas.draw_fullscreen(&piv);

    match png::write(Path::new(OUT_PNG), &canvas.rgba, 320, 200) {
        Ok(()) => {
            println!("[moonstone] rendered a real frame -> {OUT_PNG} (320x200)");
            println!("MOONSTONE-AROS: RENDER PASS");
            0x4D4F_4F4E // "MOON"
        }
        Err(e) => {
            println!("[moonstone] png write FAILED: {e:?}");
            2
        }
    }
}
