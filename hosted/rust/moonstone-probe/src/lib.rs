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

// The AROS present+input shim (aros_moonstone_gfx.c).
unsafe extern "C" {
    fn aros_ms_open(w: i32, h: i32) -> i32;
    fn aros_ms_blit(rgba: *const u8, w: i32, h: i32);
    fn aros_ms_input() -> i32;
    fn aros_ms_delay(ticks: i32);
    fn aros_ms_close();
}

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

/// Stage 2a: the LIVE present+input shim. Opens an AROS window and, each frame,
/// redraws the game's SoftwareCanvas (the PIV background + a cursor the player moves
/// with the arrow keys) and blits it to the window. Proves the whole platform surface
/// (window + WritePixelArray blit + RAWKEY input) end to end; Stage 2b swaps this
/// simple driver for the real SceneStack loop.
#[no_mangle]
pub extern "C" fn aros_moonstone_play() -> u32 {
    let bg = match std::fs::read(IN_PIV) {
        Ok(b) => PivImage::load(&b),
        Err(e) => {
            println!("[moonstone] read {IN_PIV} FAILED: {e:?}");
            return 1;
        }
    };

    if unsafe { aros_ms_open(320, 200) } != 0 {
        println!("[moonstone] could not open the AROS window");
        return 2;
    }
    println!("[moonstone] live window open; arrows move, Esc/close quits");

    let mut canvas = SoftwareCanvas::new();
    let (mut px, mut py) = (156i32, 96i32);

    loop {
        let inp = unsafe { aros_ms_input() };
        if inp & 32 != 0 {
            break;
        }
        if inp & 1 != 0 { px -= 2; }
        if inp & 2 != 0 { px += 2; }
        if inp & 4 != 0 { py -= 2; }
        if inp & 8 != 0 { py += 2; }
        px = px.clamp(0, 311);
        py = py.clamp(0, 191);

        // redraw: the game background + a cursor block (yellow, red while firing)
        canvas.draw_fullscreen(&bg);
        let fire = inp & 16 != 0;
        for dy in 0..8usize {
            for dx in 0..8usize {
                let x = px as usize + dx;
                let y = py as usize + dy;
                let o = (y * 320 + x) * 4;
                canvas.rgba[o] = 255;
                canvas.rgba[o + 1] = if fire { 0 } else { 255 };
                canvas.rgba[o + 2] = 0;
                canvas.rgba[o + 3] = 255;
            }
        }

        unsafe {
            aros_ms_blit(canvas.rgba.as_ptr(), 320, 200);
            aros_ms_delay(1); // ~20ms/frame
        }
    }

    unsafe { aros_ms_close() };
    println!("MOONSTONE-AROS: LIVE PASS");
    0x4D4F_4F4E
}

/// Log Rust panics (message + file:line) to both stdout and a persistent file
/// before the process aborts. With `panic = abort` a panic still tears the
/// instance down, but this leaves an exact record of what failed (e.g. an asset
/// a scene could not load) instead of a bare SIGBUS.
const CRASH_LOG: &str = "MacRW:moonstone-crash.log";
fn install_panic_logger() {
    std::panic::set_hook(Box::new(|info| {
        use std::io::Write;
        let text = format!("[moonstone] {info}\n");
        print!("{text}");
        let _ = std::io::stdout().flush();
        if let Ok(mut f) =
            std::fs::OpenOptions::new().create(true).append(true).open(CRASH_LOG)
        {
            let _ = f.write_all(text.as_bytes());
        }
    }));
}

/// Stage 2b: the REAL game. Builds the scene stack and runs the game's own fixed-step
/// loop live on an AROS window: read the keyboard into InputState, tick the scenes,
/// render into the SoftwareCanvas, blit. Assets are resolved from the current dir, so
/// run it from the packaged folder (`cd MacRW:Moonstone`), which holds
/// `extracted/moonahdk/`.
#[no_mangle]
pub extern "C" fn aros_moonstone_game() -> u32 {
    use moonstone_core::scenes::build_root;
    spawn_game(build_root)
}

/// Same game, but start the scene stack at the main menu (skip the intro
/// cinematic). Handy for jumping straight to menu / knight selection.
#[no_mangle]
pub extern "C" fn aros_moonstone_game_skip() -> u32 {
    use moonstone_core::scene::Scene;
    use moonstone_core::scenes::MenuScene;
    println!("[moonstone] --skip-intro: starting at the main menu");
    spawn_game(|| Box::new(MenuScene::new()) as Box<dyn Scene>)
}

/// Run the game loop on a big-stack thread (the scene stack + rasterizer overflows
/// the shell's default command stack, which faults in Exec_NewStackSwap). The thread
/// inherits the launcher's current dir, so assets resolve against `cd MacRW:Moonstone`.
/// `build` runs on the thread, so its asset loads see that current dir.
fn spawn_game<F>(build: F) -> u32
where
    F: FnOnce() -> Box<dyn moonstone_core::scene::Scene> + Send + 'static,
{
    install_panic_logger();
    match std::thread::Builder::new().stack_size(1024 * 1024).spawn(move || run_scene_stack(build())) {
        Ok(h) => h.join().unwrap_or(3),
        Err(e) => {
            println!("[moonstone] could not spawn the game thread: {e:?}");
            3
        }
    }
}

fn run_scene_stack(root: Box<dyn moonstone_core::scene::Scene>) -> u32 {
    use moonstone_core::input::InputState;
    use moonstone_core::scene::SceneStack;
    use moonstone_core::session::Session;

    println!("[moonstone] Moonstone on AROS: arrows move, Space fires, Esc quits");
    if unsafe { aros_ms_open(320, 200) } != 0 {
        println!("[moonstone] could not open the AROS window");
        return 2;
    }

    let mut stack = SceneStack::new(root);
    let mut session = Session::new_single(0x1234_5678);
    let mut canvas = SoftwareCanvas::new();

    loop {
        let bits = unsafe { aros_ms_input() };
        if bits & 32 != 0 {
            break;
        }
        let input = InputState {
            left: bits & 1 != 0,
            right: bits & 2 != 0,
            up: bits & 4 != 0,
            down: bits & 8 != 0,
            fire: bits & 16 != 0,
            ..Default::default()
        };

        stack.tick(input, &mut session);
        stack.render(&mut canvas, &session);

        unsafe {
            aros_ms_blit(canvas.rgba.as_ptr(), 320, 200);
            aros_ms_delay(1);
        }
    }

    unsafe { aros_ms_close() };
    println!("MOONSTONE-AROS: GAME PASS");
    0x4D4F_4F4E
}
