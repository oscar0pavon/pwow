# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`pwow` is a small program built on the author's own Vulkan engine, **pengine** (`/root/pengine`, a separate git repo). It flies a camera over real World of Warcraft **Classic 1.12** terrain, read from the user's own extracted game data. It is the consumer used to exercise pengine's `src/engine/terrain/` code; nearly all terrain logic lives in pengine, and `pwow.c` is only the application on top of it.

The repo is local only (no remote). Changes to the engine are committed and pushed in `/root/pengine`, not here.

## Build and run

```
make -C /root/pengine -j24     # the engine first: builds lib/libpengine.a and the .spv shaders
make                           # here: builds ./pwow and ./adt2wot
./prepare_tile.sh azeroth 31 49 1   # once: convert the 3x3 block around Goldshire into data/
./pwow                              # run from this directory, it loads data/ by relative path
```

- `Makefile` has `WORKDIR := /root/pengine` hardcoded and includes pengine's `include.make`. The compile flags matter to a consumer, not just the engine: `-fcommon` and the `CGLM_FORCE_*` defines change struct layout and projection maths.
- pengine is a **static library**, and `make` here only compares `pwow.c` against `libpengine.a`'s timestamp. After any engine change, rebuild the engine and then `make -B` here, or a stale binary is what you run.
- There are no tests and no lint target. "It builds" and "it renders correctly" are the only checks; see below for how to look at it.
- `prepare_tile.sh` reads the game data from `$GAME_DATA` (default `/root/sources/WoWee/Data/expansions/classic`) and converts BLP textures with `$BLP_CONVERT` (WoWee's `blp_convert`, default under `/root/sources/WoWee/build/bin`). It skips textures already converted and tiles the game does not have. `data/` is gitignored on purpose: the textures come from the user's install and are not ours to distribute.

Controls: W A S D move, Space / C up and down, I K pitch, J L turn, Shift is 4x speed, Q quits.

## Data pipeline

```
world/maps/<map>/<map>_<x>_<y>.adt ──adt2wot──▶ data/<map>_<x>_<y>.wot + .whm + .wwt
tileset/**/*.blp ──blp_convert──▶ data/tileset/**/*.png
                                            │
              pe_vk_terrain_world_load_area() ◀┘  (pengine)
```

`tools/adt2wot.c` is a standalone offline converter (no engine dependency) from Blizzard's ADT to WoWee's open `.wot` (JSON: tile coords, texture names, per-chunk layer ids and hole masks) and `.whm` (binary: 256 chunks of 145 heights plus alpha maps), plus a `.wwt` of its own for the water. It prints the PNG path of each texture the tile uses, which is how `prepare_tile.sh` knows what to convert.

`.wwt` is **not WoWee's format**: WoWee's `.wot` keeps one flat height per water chunk, which loses the real surface. Ours is `WWT1`, a count, then per water chunk its index, liquid type (water, ocean, magma, slime), 9x9 float heights, 9x9 depth bytes and 64 quad-visible flags. A tile with no water has no `.wwt`, and the engine treats a missing one as fine.

Things that were checked against real data and are easy to get wrong:

- In a tile file name `<map>_<x>_<y>.adt` the **first number is `tileX`, along the west axis**, and it is what pengine's `tile_x` means.
- Sub-chunk offsets inside an MCNK are counted from the start of the chunk *including* its 8-byte header.
- Every layer's alpha map is written to the `.whm` as **8-bit, decoded, back to back, one per layer above the base**. The `.wot` does not record where a layer's map starts, so pengine's loader infers the layout from the blob's size; that is why the converter normalises 4-bit and run-length maps instead of copying them.
- A layer above the base with no alpha map covers everything under it.
- Classic stores water per chunk (MCLQ, not the later MH2O). Its 9x9 grid sits on the chunk's corner vertices, rows along X and columns along Y like the ground, and a vertex the liquid does not reach holds `FLT_MAX`, which the converter replaces with the block's lowest height. The liquid type comes from the chunk's flags, and a quad tile flag of `0x0F` (or bit `0x80`) means no liquid there.
- The Goldshire block has 22 chunks with **hole masks**. Rectangular black-looking gaps in the ground are legitimate holes (building and cave footprints), not rendering cracks.

## How the program is put together

`pwow.c` fills a `PGame` and calls `pengine_run()`. Then:

1. `init` builds the world: `pe_vk_terrain_world_create()`, then `pe_vk_terrain_world_load_area()` for a square of tiles around a centre. Meshes, materials and the shared texture cache are created here, on the GPU, once. There is no unloading yet.
2. `update` moves the camera from `input.<KEY>.pressed`, scaled by `delta_time` (seconds).
3. The engine calls the `pe_vk_draw_scene` hook every frame. `pwow_draw_scene` fills a `PTerrainFrame` (camera via `pe_terrain_frame_set_camera()`, which also stamps the time the water animates by, then lighting, fog and sky colours) and hands it to `pe_vk_terrain_world_draw()`, which uploads it, draws the sky, the ground of every tile, and last the water, which blends over the ground.

Update and draw both run on the main thread, so the camera is not raced.

Conventions specific to this program:

- World units are yards; a tile is `PE_TERRAIN_TILE_SIZE` (1600/3). Tile (32, 32) is the middle of the map, and rows run toward -X and columns toward -Y, so the start position is `(32 - tile_y - 0.5) * size, (32 - tile_x - 0.5) * size`.
- **The fog colour and the sky's horizon colour must be the same** (`HORIZON_COLOR`), or the far ground shows as a band against the sky. `FOG_END` also acts as the view distance: chunks entirely past it are not drawn.
- The engine's `transparency` flag in `PCreateShaderInfo` sets blend factors but never enables blending, so it does nothing. The water pipeline passes its own colour-blend state instead.
- The renderer's render pass clears to black on purpose, and `pe_change_background_color()` does nothing on the Vulkan path. Do not use it to set a sky.

## Looking at the result

The user's compositor is sway and their desktop holds other windows, so:

- **Float the test window** (`swaymsg '[pid=<pid>] floating enable, resize set 1280 720'`) as soon as it maps; a tiled test window disrupts their layout.
- **Capture only that window** with `grim -g "<x>,<y> <w>x<h>"` using the geometry from `swaymsg -t get_tree`. A bare `grim` captures the whole desktop, including their browser and terminals.
- Rendering glitches here have usually been in the engine, not this repo (a 0.001 camera near plane once tore cracks in the ground). To tell missing geometry from wrongly shaded pixels, temporarily change the clear colour to magenta.
- The IDE's clangd reports missing headers and unknown types in `pwow.c` because it does not know the engine's include flags. Trust `make`, not those diagnostics.

## Conventions

- Commit messages are lowercase imperative summaries with a prose body explaining the mechanism and consequence, not a bullet list of edits. End them with the attribution line from the session's reminder.
- Code is written to need no comment; where one exists it is a `//INFO` line explaining *why* something non-obvious holds.
