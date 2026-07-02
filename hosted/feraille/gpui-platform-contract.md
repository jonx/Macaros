# GPUI platform backend contract (rev 1d217ee) — recon for gpui_aros

Verified against /Users/user/Source/zed-aros (branch aros-platform). Summary of
what a new backend must implement; the authoritative source is the code.

Key findings:
- **No CPU scene rasterizer exists anywhere in the tree** (macos=Metal,
  linux/web=wgpu; render_to_image is Metal-only). gpui_aros authors one
  (tiny-skia is already a transitive dep via resvg).
- **Text system is reusable**: `gpui_wgpu::CosmicTextSystem`
  (crates/gpui_wgpu/src/cosmic_text_system.rs) is OS-independent
  (cosmic-text/swash/fontdb only). Depend on gpui_wgpu, do not copy. Use
  `new_without_system_fonts(...)` + `add_fonts(bundled)`; leave `font-kit` OFF.
- **Template**: gpui_web (crates/gpui_web/src/{platform,window,dispatcher,
  display,events,keyboard,http_client}.rs) shows exactly what can be stubbed
  while GPUI still boots.
- **`NoopTextSystem`** (gpui/src/platform.rs:846) can boot before fonts wire up.
- **`DummyKeyboardMapper`** (gpui/src/platform/keyboard.rs) is a ready stub.

## Traits (gpui/src/platform.rs)

- `Platform` (:122): ~45 required methods — executors, text_system, run/quit,
  displays, open_window, clipboard, cursor, menus, prompts, credentials,
  keyboard. Defaults exist for window_stack/screen-capture/dock/jump-list.
  aros does NOT get the linux-only primary-clipboard methods (cfg-gated).
  `run(on_finish_launching)` OWNS the main loop: init, call the closure (app
  opens its window there), pump OS events + drain main-thread runnables until
  quit(). Stubs acceptable initially: restart/activate/hide/menus/credentials
  (Task::ready), prompts (send Ok(None)), clipboard (None/no-op),
  thermal_state (Nominal).
- `PlatformWindow` (:620, : HasWindowHandle + HasDisplayHandle): bounds/
  content_size/scale_factor, set_input_handler/take_input_handler,
  callbacks (on_request_frame/on_input/on_resize/on_should_close/on_close/
  on_active_status_change/on_hover_status_change/on_moved/on_appearance_changed/
  on_hit_test_window_control), `draw(&Scene)`, `sprite_atlas() ->
  Arc<dyn PlatformAtlas>`, is_subpixel_rendering_supported (return false =>
  GPUI emits only MonochromeSprites), gpu_specs, update_ime_position,
  prompt/minimize/zoom/fullscreen. Repaint model: window fires the stored
  `on_request_frame` callback (from a timer/vsync tick or on demand); GPUI
  calls back `draw(&scene)`; `completed_frame()` (default no-op) is the
  present hook.
- `PlatformDispatcher` (:782, Send+Sync): is_main_thread, dispatch (bg),
  dispatch_on_main_thread, dispatch_after, spawn_realtime. Main-thread
  runnables MUST run on the thread that called run(); the run loop's blocking
  wait is the park, posting must wake it. Single-thread shortcut (web): route
  dispatch -> main thread.
- `PlatformDisplay` (:254): id/uuid/bounds (+ default visible_bounds/
  default_bounds). One synthetic display is fine.
- `PlatformTextSystem` (:812): font_id/metrics/typographic_bounds/advance/
  glyph_for_char/glyph_raster_bounds/rasterize_glyph (returns alpha coverage
  bytes)/layout_line/add_fonts/all_font_names/recommended_rendering_mode.
  => use CosmicTextSystem.
- `PlatformAtlas` (:1064): get_or_insert_with(key, build) -> AtlasTile;
  AtlasKey = Glyph|Svg|Image; kinds Monochrome/Polychrome/Subpixel. Reference
  impl gpui_wgpu/src/wgpu_atlas.rs (etagere shelf packing) — port with Vec<u8>
  CPU textures.

## Renderer boundary (gpui/src/scene.rs)

`scene.batches()` -> ordered `PrimitiveBatch`:
Shadows (bounds, corner_radii, blur_radius, color Hsla, content_mask, inset),
Quads (bounds, corner_radii, border_widths, border_color, background:
Background, border_style, content_mask),
Paths (vertices mesh, color Background),
Underlines (bounds, color, thickness, wavy),
MonochromeSprites {texture_id, ...} (bounds, color, AtlasTile, transformation,
content_mask) = glyphs (tint tile alpha),
SubpixelSprites (skip: report unsupported),
PolychromeSprites (bounds, corner_radii, grayscale, opacity, AtlasTile) =
images,
Surfaces (video; no-op like wgpu backend).
All coords ScaledPixels (already x scale). content_mask = clip rect.
Renderer shape (mirror WgpuRenderer): own framebuffer + atlas,
`draw(&Scene)`, `update_drawable_size(Size<DevicePixels>)`.

## Wiring

- zed-aros root Cargo.toml: workspace members += "crates/gpui_aros";
  [workspace.dependencies] gpui_aros = { path, default-features = false }.
- gpui_platform/Cargo.toml: [target.'cfg(target_os = "aros")'.dependencies]
  gpui_aros.workspace = true; extend feature lists if needed.
- gpui_platform.rs current_platform(): add
  `#[cfg(target_os = "aros")] { Rc::new(gpui_aros::ArosPlatform::new(headless)) }`.
- gpui_aros/Cargo.toml: [lib] path = "src/gpui_aros.rs"; deps gpui, gpui_wgpu
  (CosmicTextSystem), anyhow, collections, parking_lot, smallvec,
  raw-window-handle 0.6, log, etagere, tiny-skia.

## Gotchas

- WindowParams (:1540): read bounds + window_min_size, ignore the rest first.
- PlatformInputHandler (:1168) wraps InputHandler (:1368) — store it, call on
  text/IME events. update_ime_position no-op OK.
- ForegroundExecutor is !Send; is_main_thread must be correct (store ThreadId
  at construction).
- http_client: not required by Platform trait; Application needs an
  Arc<dyn HttpClient> — minimal no-op impl acceptable.
- gpu_specs: Some(software specs) or None.
- Feraille consumes the fork via [patch."https://github.com/zed-industries/zed"]
  in its root Cargo.toml (aros-port branch).
