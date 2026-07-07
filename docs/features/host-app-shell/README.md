# Host app shell — Macaros

**Status: built.** Makes the AROS window a **first-class macOS app**: an app menu
bar, an About panel (the macaron), a custom icon, and a schema-driven **Settings**
window — all delivered by the cocoametal dylib, so it travels with the display,
not as a separate process.

**Macaros** = a macaron: AROS on a Mac.

## Package it

```sh
graft/make-aros-app.sh        # a dev Macaros.app that wraps the local boot tree
graft/make-aros-release.sh    # a SELF-CONTAINED, relocatable Macaros.app (+ --dmg)
```

`make-aros-app.sh` is the developer wrapper (points at your `~/aros-build`);
`make-aros-release.sh` embeds the whole prepared AROS volume so the `.app` boots
the Wanderer desktop on a clean Mac, ready to Developer-ID sign + notarize.

The Settings window is *generated* from
[`hosted/cocoametal/settings.json`](../../../hosted/cocoametal/settings.json)
(`AROS_SETTINGS_SCHEMA`); its choices are written to `aros-host.conf`, which the
launcher turns into boot env (see
[`graft/aros-host-conf.sh`](../../../graft/aros-host-conf.sh)).

## Docs

- [design.md](design.md) — the app-shell architecture, the two-tier Settings
- [spec.md](spec.md) — implementation spec
- Surfaces it umbrellas: [display](../cocoa-metal-display/README.md) ·
  [clipboard](../clipboard-bridge/README.md) · [host volume](../host-volume/README.md)
