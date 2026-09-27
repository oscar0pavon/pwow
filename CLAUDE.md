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
- `make` also builds `adt2wot`, `wmo2wwb` and `m22wwb`, the converters in `tools/`, which `prepare_tile.sh` calls.
- `prepare_tile.sh` reads the game data from `$GAME_DATA` (default `/root/sources/WoWee/Data/expansions/classic`) and converts BLP textures with `$BLP_CONVERT` (WoWee's `blp_convert`, default under `/root/sources/WoWee/build/bin`). It skips textures already converted and tiles the game does not have. `data/` is gitignored on purpose: the textures come from the user's install and are not ours to distribute.

Controls: W A S D move, Space / C up and down (flying only), I K pitch, J L turn, Shift is faster, Tab toggles between flying and walking, Q quits. Walking follows the ground at a 2 yard eye height at 7 yards a second, using `pe_terrain_world_height_at()`; over a hole or past the loaded tiles it keeps its last height, and it does not know about water, so it wades under a lake.

## Data pipeline

```
world/maps/<map>/<map>_<x>_<y>.adt ──adt2wot──▶ data/<map>_<x>_<y>.wot + .whm + .wwt
world/wmo/**/name.wmo + name_NNN.wmo ──wmo2wwb──▶ data/world/wmo/**/name.wwb
world/**/name.m2 (the props) ──m22wwb──▶ data/world/**/name.wwb
*.blp (tile, building and prop textures) ──blp_convert──▶ data/**/*.png
                                            │
              pe_vk_terrain_world_load_area() ◀┘  (pengine)
```

`tools/adt2wot.c` is a standalone offline converter (no engine dependency) from Blizzard's ADT to WoWee's open `.wot` (JSON: tile coords, texture names, per-chunk layer ids and hole masks) and `.whm` (binary: 256 chunks of 145 heights plus alpha maps), plus a `.wwt` of its own for the water. It prints the PNG path of each texture the tile uses, which is how `prepare_tile.sh` knows what to convert.

The `.wot` also carries the tile's buildings in WoWee's own `wmoNames` / `wmos` fields: each placement's raw ADT position, rotation in degrees, unique id, and the world box Blizzard stored for it. `adt2wot` prints `texture <png>` and `building <wmo path>` lines, which `prepare_tile.sh` acts on. `tools/wmo2wwb.c` converts one building, its root file and its `_NNN` group files, to a `.wwb`; a building with more than 64 groups is refused with exit code 3, which is how Stormwind (the whole city, 306 groups) is skipped. The engine logs the missing `.wwb` once and leaves those placements out.

The props, the trees and fences and barrels and grass, are the tile's MDDF placements (`doodadNames` / `doodads` in the `.wot`, 36-byte records: raw position, rotation, scale with 1024 for life size) and are models of the game's own `.m2` format. A Goldshire tile places 200 to 1500 of them, 7500 in the 3x3 block, of about 200 kinds. `tools/m22wwb.c` reads a classic model (version 256, whose views are inside the file and not in `.skin` files) and writes a `.wwb` of one group, so the building renderer draws it unchanged. What it keeps: the first view, the batches on the base layer (`materialLayer` 0), and materials whose blend is opaque, alpha-key or alpha, the last drawn as alpha-key; a batch that adds or multiplies (glows, light shafts) is dropped. Vertex colours are white, since a model carries no baked light. A model with nothing left to draw (`fireflies01.m2`) is refused with exit code 3, and the engine logs its missing `.wwb` once. The map files name a model `.mdx` and the file is `.m2`, so `adt2wot` writes `.m2`.

`.wwb` is ours too (`WWB2`): the box round all the groups, a texture path table, materials `{texture, blend, flags}`, then per group its flags, its own box, 36-byte vertices, u32 indices and batches `{first_index, index_count, material}`. The group flags and boxes are read and not used to draw. Bumping the magic means old `.wwb` files are refused, so delete `data/world/wmo/**/*.wwb` and rerun `prepare_tile.sh` after changing the format. WoWee's own building format merges materials per group and loses the batches, which is why it is not used.

`.wwt` is **not WoWee's format**: WoWee's `.wot` keeps one flat height per water chunk, which loses the real surface. Ours is `WWT1`, a count, then per water chunk its index, liquid type (water, ocean, magma, slime), 9x9 float heights, 9x9 depth bytes and 64 quad-visible flags. A tile with no water has no `.wwt`, and the engine treats a missing one as fine.

Things that were checked against real data and are easy to get wrong:

- In a tile file name `<map>_<x>_<y>.adt` the **first number is `tileX`, along the west axis**, and it is what pengine's `tile_x` means.
- Sub-chunk offsets inside an MCNK are counted from the start of the chunk *including* its 8-byte header.
- Every layer's alpha map is written to the `.whm` as **8-bit, decoded, back to back, one per layer above the base**. The `.wot` does not record where a layer's map starts, so pengine's loader infers the layout from the blob's size; that is why the converter normalises 4-bit and run-length maps instead of copying them.
- A layer above the base with no alpha map covers everything under it.
- Classic stores water per chunk (MCLQ, not the later MH2O). Its 9x9 grid sits on the chunk's corner vertices, rows along X and columns along Y like the ground, and a vertex the liquid does not reach holds `FLT_MAX`, which the converter replaces with the block's lowest height. The liquid type comes from the chunk's flags, and a quad tile flag of `0x0F` (or bit `0x80`) means no liquid there.
- `pe_terrain_world_height_at()` answers with the surface that is drawn: each quad is four triangles fanned from its centre vertex, and the centre is often a yard off the plane of the corners (up to 2.4 yards from a bilinear guess in this data), so it does not interpolate the corners. It is `false` in a hole and in an unloaded tile.
- A building placement (MODF) is turned into a world position and orientation by one rule, taken from WoWee and confirmed here against the box Blizzard stored for every placement (worst error 2 mm over 49): translate, then Z, Y, X rotations by the degrees taken as `(rot[2], rot[0], rot[1] + 180)`, with the world's Y reflection in front. The `+180` is not optional: without it 29 of the 49 boxes are more than a yard out. The stored box is the union of the *group* boxes; a WMO's root box (MOHD) is sometimes stale (`farm.wmo`, `goldshireblacksmith.wmo`), so never compare against it.
- **A building is an exterior shell plus interior rooms, and back faces are culled unless the material is two sided.** Group flag `0x8` is exterior and `0x2000` interior; the barracks is 1 exterior group and 27 rooms, with 18 times more geometry inside the walls than in them. All the groups are drawn, always, and the game's own rule decides what shows: a triangle is culled from behind, except that a material with flag `0x4` (two sided, the same bit in a `.wmo` material and an `.m2` render flag) is drawn from both, on a second pipeline. A room's walls face into the room, so from outside they are back faces and the shell hides them; seen through a door or a porch they are what shows. The front face is `VK_FRONT_FACE_COUNTER_CLOCKWISE` for a building, the terrain's is the other way, because the placement's reflection of Y turns every winding. Before culling the rooms were drawn only when the camera was inside one, to hide their plaster and beams showing through the walls in patches; that was the missing cull and not the rooms, and it also left doors and porches empty and the back of every wall visible. The `.wwb` still carries each group's flags and box, unused.
- The terrain is one-sided (back faces are culled), so a camera underground sees through it and catches the far slopes edge-on as long tan ribbons in the sky. A test camera at a fixed height will do this wherever the ground is higher; it is not a rendering fault.
- About 7% of a building's indices are in no batch. They are collision-only triangles, absent from the game's own render batches too, and are what building collision would be made from.
- Materials in these buildings use only blend modes 0 (opaque) and 1 (alpha-test, cut at 0.5); the ones for windows and leaves rely on the texture's own alpha. Vertex colours are baked light and are multiplied in.
- A prop's placement is the building rule above, then the scale (`scale / 1024`, uniform, about the model's own origin). No stored box exists to check a prop against, so it was checked against the ground: of the 1243 props of tile (31, 49) that stand over ground, the height they are placed at is 0.00 yards from `pe_terrain_heights_at()` at the median, within 0.44 for 90% and within 4.3 for 99% (the rest stand on tables, roofs and docks). That checks the position and the flip. The rotation was judged by eye: fences follow the slope and posts stand upright.
- Models are static here. A classic model has bones and animations, and this reads none of it: vertices are drawn as stored. The trees have one bone with no flags, so nothing is lost. A model with billboard bones, whose leaf cards the game turns to face the camera, is drawn with the cards as authored.
- **The trees are big**, canopies 100 yards across and trunks over 30 high, and the old start position 85 yards up is now inside one. Long thin green streaks over the canopy seen from above are the edges of its large flat leaf planes, which is how the game models them.
- The Goldshire block has 22 chunks with **hole masks**, and the inn stands in one of them. Rectangular black-looking gaps in the ground are legitimate holes (building and cave footprints), not rendering cracks.

## How the program is put together

`pwow.c` fills a `PGame` and calls `pengine_run()`. Then:

1. `init` builds the world: `pe_vk_terrain_world_create()`, then `pe_vk_terrain_world_load_area()` for a square of tiles around a centre. Meshes, materials and the shared texture cache are created here, on the GPU, once. There is no unloading yet.
2. `update` moves the camera from `input.<KEY>.pressed`, scaled by `delta_time` (seconds).
3. The engine calls the `pe_vk_draw_scene` hook every frame. `pwow_draw_scene` fills a `PTerrainFrame` (camera via `pe_terrain_frame_set_camera()`, which also stamps the time the water animates by, then lighting, fog and sky colours) and hands it to `pe_vk_terrain_world_draw()`, which uploads it, draws the sky, the ground of every tile, the buildings and props, and last the water, which blends over everything.

Update and draw both run on the main thread, so the camera is not raced.

Conventions specific to this program:

- World units are yards; a tile is `PE_TERRAIN_TILE_SIZE` (1600/3). Tile (32, 32) is the middle of the map. **The world is X north, Y east, Z up**, which is left handed to match the engine's camera. The game stores it as X north, Y *west*, right handed, and drawn like that everything is its own mirror image (facing north, west on the right); the module negates Y once, in `pe_terrain_point_y`. Rows run toward -X (south) and columns toward +Y (east), so the start position is `(32 - tile_y - 0.5) * size, -(32 - tile_x - 0.5) * size`. Anything read from the game data that carries a position must go through that flip; the sun direction in `pwow.c` is in these axes too.
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
