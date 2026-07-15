# Quake II — SDL3 / OpenGL 3.3 port

A modern source port of id Software's **Quake II** (GPL release), rebuilt on
**SDL3** with a **from-scratch OpenGL 3.3 core** renderer. The engine, game
logic, and file formats stay faithful to retail Quake II; the platform layer
and renderer are new.

Requires the retail `pak0.pak` (and `pak1`/`pak2` for the full game) — this
repository contains only source code, no game assets.

---

## Features

### Renderer (`src/ref_gl3`, written from scratch)

A GLSL / VBO / UBO pipeline (glad2 loader, no legacy fixed-function `qgl`
layer) with full behavioural parity to id's `ref_gl`, plus modern additions:

- MSAA, anisotropic filtering, and an HDR FBO post pipeline (bloom, underwater
  warp) — `gl_msaa`, `gl_anisotropy`, `gl_bloom`
- Per-pixel dynamic lights and bump mapping — `gl_dynamic`, `gl_bump`
- Soft round particles, blob shadows — `gl_shadows`
- Hi-res texture override packs (`.png`/`.tga`/`.jpg`) — `gl_retexture`
- Red/cyan anaglyph stereo — `cl_stereo`, `cl_stereo_separation`
- Live `vid_fullscreen` / `gl_mode` / UI-scale changes (no `vid_restart`)

### Gameplay & engine

- **Widescreen HUD** — `hud_wide` (statusbar layout program, game-DLL side)
- **Nemesis** wave-survival mode — start a deathmatch and type `nemesis` in
  the console (`src/game/g_waves.c`)
- **Asset cache** — keep pak assets resident across map changes (`cache_assets`)
- Both game DLLs shipped: `baseq2` and `ctf`

---

## Building

**Requirements:** CMake, a C23-capable toolchain (developed with Visual Studio
2022 / MSVC on Windows x64), and network access for the first configure (SDL3
is fetched from source via CMake `FetchContent`).

```sh
cmake -S . -B build
cmake --build build --config Release
```

This builds five targets:

| Target     | Output                     | Notes                              |
|------------|----------------------------|------------------------------------|
| `quake2`   | `build/quake2.exe`         | Client (GUI subsystem on Windows)  |
| `q2ded`    | `build/q2ded.exe`          | Dedicated server                   |
| `ref_gl3`  | `build/ref_gl3.dll`        | Renderer (loaded via `GetRefAPI`)  |
| `game`     | `build/baseq2/game_x86_64.dll` | baseq2 game logic (`GetGameAPI`) |
| `ctf`      | `build/ctf/game_x86_64.dll`    | CTF game logic                   |

> Adding a **new** `.c` file requires re-running the configure step
> (`cmake -S . -B build`) — the source lists use a glob that CMake caches.

## Running

Place retail `pak0.pak` (and `pak1.pak`, `pak2.pak`) in `build/baseq2/`, then
run `build/quake2.exe`. To launch the wave mode:

```
quake2.exe +set deathmatch 1 +map q2dm1
```

…then open the console and type `nemesis`.

---

## Project layout

```
src/
  common/    engine core (cmd, cvar, files, cmodel, net, pmove)
  server/    server
  client/    client (input, prediction, sound mixer, HUD, menus, cinematics)
  platform/  SDL3 layer (video, input, audio, sockets, system)
  ref_gl3/   OpenGL 3.3 renderer (+ vendored glad2, stb_image)
  game/      baseq2 game DLL
  ctf/       CTF game DLL
```

The engine ↔ renderer boundary is the classic `refimport`/`refexport` function
table (renderer is a runtime-loaded DLL); the engine ↔ game boundary is the
`GetGameAPI` ABI. SDL is confined to `src/platform` and the renderer's GLimp.

---

## Security hardening

Quake II's original loaders and network code trust a great deal of on-the-wire
and on-disk data. That was acceptable in 1997; today a player connects to
arbitrary internet servers and auto-downloads maps, models, textures, sounds,
and cinematics from them. This port has been through a dedicated
memory-safety hardening pass under that threat model — **a malicious server,
and any auto-downloaded or shared file (including savegames), is untrusted.**

### What was found

The port inherited essentially all of vanilla Quake II's known memory-safety
weaknesses: roughly **40 distinct vulnerabilities** (about **60 individual
bounds checks** added). By severity:

| Severity | Count | Examples |
|----------|-------|----------|
| **Exploitable write / RCE-class** | ~18 | Six remote configstring→`strcpy` stack/global smashes; `areabits` frame overflow; cinematic audio stack smash; TGA/PCX/WAL/MD2/SP2 heap overflows; a BSP node-children **write-what-where** (via `Mod_SetParent`); two areaportal OOB writes into `map_areas[]`/`portalopen[]` (flood + door-open); a per-frame vis-decompression buffer overflow; savegame count/entnum heap overflow |
| **Out-of-bounds read** (crash / info-leak / DoS) | ~20+ | Download block over-read; TGA/PCX/WAV decode over-reads; unterminated MD2 skin name; the full BSP cross-lump index set (edge→vertex, surfedge→edge, face/node/leaf/submodel/texinfo/brush/brushside/leafbrush/cluster indices) in **both** the render and collision loaders |

By reachability:

- **Remote, from any server you connect to:** ~8
- **Crafted / auto-downloaded asset** (map, model, texture, sound, cinematic): ~27
- **Shared savegame:** ~5

### Notes

- **Almost none of these were introduced by the port** — they are vanilla id
  code carried over unhardened. The notable exception: the port's own
  multi-slot configstring change *elevated* a latent id `strcpy` into a live
  **remote** stack smash, which is why that class was the highest-value fix.
- **One issue is left open by design:** the savegame `F_FUNCTION` / `F_MMOVE`
  restore reconstructs raw code/data pointers as `base + offset` (an
  arbitrary-call primitive). It remains gated by the existing same-binary
  checks (`__DATE__` match and `InitGame` base-address check), because
  soundly bounding a raw code offset needs segment information the game DLL
  does not expose.

### How it was verified

The pass combined a multi-agent code audit with several adversarial
verification and completeness rounds; each round surfaced real issues the
prior one missed (the areaportal OOB write, for instance, was only caught
late because it lives in trace/flood logic rather than an obvious loader
index). Every finding was confirmed against the actual source before a fix
was applied, and each fix was runtime-verified — loading and rendering
several diverse retail maps (deathmatch, liquid, lava, large multi-area/
areaportal maps), a save/load round-trip, player movement (collision traces),
and PCX HUD rendering — to confirm valid data is never rejected.

---

## Author

Port authored and maintained by **Len Mudgett**.

## License

Quake II is licensed under the **GNU General Public License v2** (see id's
`LICENSE`/`gnu.txt`). This port remains GPL-2 overall. Newly authored source
files in this repository omit the per-file GPL header by preference; files
adapted from the original id release retain theirs.
