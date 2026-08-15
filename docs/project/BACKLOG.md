# Project backlog (TODO)

Granular, cross-cutting tasks that are not tied to a single feature live here.
Per-subsystem design and status lives in the
[feature index](../features/README.md).

## Host app / FFView
- [x] **FFView drag-and-drop** — AppWindow registered; dropping a Wanderer file icon
      opens it in the viewer (empty "drop a file" state when launched with no arg).
      Done 2026-07-13, verified live (drop big.m4v -> plays).
- [x] **Datatypes integration** — `ffmpeg.datatype` (libavcodec-backed picture
      datatype) decodes a video's first frame so MultiView / the desktop preview
      media. Done 2026-07-13, verified live (MultiView opens test.avi → first
      frame shown). Class + `FFmpeg.dtd` in aros-upstream; build via
      `hosted/ffmpeg/build-datatype.sh`. Follow-ups: an *animation* datatype
      (multi-frame playback in MultiView), more container descriptors (mp4/mov/
      mkv route in the picture group), and launching FFView from the datatype.

## OS / build
- [ ] **Full OS rebuild with `-ffixed-x18`** — the make.cfg config is committed and
      `gfx.hidd` is rebuilt; rebuild the rest so every module is x18-safe. Pending the
      AROS maintainers' call on reserving x18 ABI-wide vs preserving it in the host
      context switch. See the x18 finding in the
      [decision log](../history/DECISION-LOG.md).

## Rust on AROS
- The Rust `std` port keeps its own resume map (errno/time/env/fs/thread/net plus the
  AROS-side `clock_gettime` and `SetVar` blockers it surfaced) in
  [`hosted/rust/STD-PORT.md`](../../hosted/rust/STD-PORT.md).
