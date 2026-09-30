# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`pwow` is a small program built on the author's own Vulkan engine, **pengine** (`/root/pengine`, a separate git repo). It flies a camera over real World of Warcraft **Classic 1.12** terrain, read from the user's own extracted game data. It is the consumer used to exercise pengine's `src/engine/terrain/` code; nearly all terrain logic lives in pengine, and `pwow.c` is only the application on top of it.

A second, newer direction (`--live`) turns it into a real client of a local vmangos server instead of only a static terrain viewer - a walking character and real NPCs, not just flown-over ground. See `TODO.md`'s "Live client" section for what's done and what's missing, and the `project-pwow-live-client-goal`/`project-vmangos-server` memories for the server setup and the long-term direction.

The repo is local only (no remote). Changes to the engine are committed and pushed in `/root/pengine`, not here.

## Build and run

```
make -C /root/pengine -j24     # the engine first: builds lib/libpengine.a and the .spv shaders
make                           # here: builds ./pwow and ./adt2wot
./prepare_tile.sh kalimdor 36 32 2   # once: convert the 5x5 block around the Crossroads into data/
./pwow                              # run from this directory, it loads data/ by relative path
./prepare_tile.sh azeroth 31 49 2   # Goldshire, around its inn
./pwow azeroth 31 49                # start over the middle of a tile of any map
```

With no arguments it starts 40 yards over the Crossroads, in the Barrens (tile 36, 32, at X -437, Y 2596), looking south. With a map and a tile it starts 40 yards over the middle of that tile: the middle of Goldshire's tile is not the inn, and a big tree there can be taller than that.

The world streams: tiles within `STREAM_DISTANCE` (900 yards) of the camera are loaded and the rest given back, so walk or fly as far as the converted `data/` reaches. Convert a wide block if you mean to travel: a tile that is not in `data/` is simply not there, and the edge of the block is the edge of the world.

- `Makefile` defaults `WORKDIR ?= ../pengine` (override with `make WORKDIR=/path/to/pengine` if it isn't a sibling checkout) and includes pengine's `include.make`. The compile flags matter to a consumer, not just the engine: `-fcommon` and the `CGLM_FORCE_*` defines change struct layout and projection maths.
- pengine is a **static library**, and `make` here only compares `pwow.c` against `libpengine.a`'s timestamp. After any engine change, rebuild the engine and then `make -B` here, or a stale binary is what you run.
- There are no tests and no lint target. "It builds" and "it renders correctly" are the only checks; see below for how to look at it.
- `make` also builds `adt2wot`, `wmo2wwb` and `m22wwb`, the converters in `tools/`, which `prepare_tile.sh` calls.
- `prepare_tile.sh` reads the game data from `$GAME_DATA` (default `/root/sources/WoWee/Data/expansions/classic`) and converts BLP textures with `$BLP_CONVERT` (WoWee's `blp_convert`, default under `/root/sources/WoWee/build/bin`). It skips textures already converted and tiles the game does not have. `data/` is gitignored on purpose: the textures come from the user's install and are not ours to distribute.

Controls: W A S D move, Space / C up and down (flying only), I K pitch, J L turn, Shift is faster, Tab toggles between flying and walking. Walking only: A/D turn the character and camera together while the right mouse button is up, and strafe while it is held; Q/E always strafe, the same as retail (`input.c`'s `pwow_input_read()`, matching WoWee's `CameraController::update()`). Walking follows the floor at a 2 yard eye height at 7 yards a second: the ground from `pe_terrain_world_height_at()`, or the highest floor of a building at or below a step (0.6 yards) above the feet, whichever is higher, from `pe_terrain_world_floor_at()`; and two spheres of 0.4 yards, at 1.0 and 1.6 yards above the feet, are pushed out of building walls by `pe_terrain_world_push_out()`. Over a hole or past the loaded tiles it keeps its last height, and it does not know about water, so it wades under a lake. Props are solid too, by their own collision: a tree's trunk, a fence, a barrel, a table; a bush and a blade of grass have none and are walked through.

## Data pipeline

```
world/maps/<map>/<map>_<x>_<y>.adt ──adt2wot──▶ data/<map>_<x>_<y>.wot + .whm + .wwt
world/wmo/**/name.wmo + name_NNN.wmo ──wmo2wwb──▶ data/world/wmo/**/name.wwb + name.wwd
world/**/name.m2 (the props, and those inside buildings) ──m22wwb──▶ data/world/**/name.wwb + name.wwc
*.blp (tile, building and prop textures) ──blp_convert──▶ data/**/*.png
                                            │
              pe_vk_terrain_world_stream() ◀┘  (pengine)
```

`tools/adt2wot.c` is a standalone offline converter (no engine dependency) from Blizzard's ADT to WoWee's open `.wot` (JSON: tile coords, texture names, per-chunk layer ids and hole masks) and `.whm` (binary: 256 chunks of 145 heights plus alpha maps), plus a `.wwt` of its own for the water. It prints the PNG path of each texture the tile uses, which is how `prepare_tile.sh` knows what to convert.

The `.wot` also carries the tile's buildings in WoWee's own `wmoNames` / `wmos` fields: each placement's raw ADT position, rotation in degrees, unique id, and the world box Blizzard stored for it. `adt2wot` prints `texture <png>` and `building <wmo path>` lines, which `prepare_tile.sh` acts on. `tools/wmo2wwb.c` converts one building, its root file and its `_NNN` group files, to a `.wwb`; a building with more than 64 groups is refused with exit code 3, which is how Stormwind (the whole city, 306 groups) is skipped. The engine logs the missing `.wwb` once and leaves those placements out.

The props, the trees and fences and barrels and grass, are the tile's MDDF placements (`doodadNames` / `doodads` in the `.wot`, 36-byte records: raw position, rotation, scale with 1024 for life size) and are models of the game's own `.m2` format. A Goldshire tile places 200 to 1500 of them, 7500 in the 3x3 block, of about 200 kinds. `tools/m22wwb.c` reads a classic model (version 256, whose views are inside the file and not in `.skin` files) and writes a `.wwb` of one group, so the building renderer draws it unchanged. What it keeps: the first view, and the batches whose blend is 0 opaque, 1 alpha-key, 2 alpha or 3 additive, the game's own numbers; a batch that multiplies (4 and up) is dropped, and so is a solid batch above the base layer, which would only cover the base. Vertex colours are white, since a model carries no baked light. A model is walked into by the few triangles the game gives it for that, the `boundingTriangles` and `boundingVertices` of its header (at `0xEC` and `0xF4` in a classic one), which `m22wwb` writes to `name.wwc` (`WWC1`: a count of positions, a count of indices, the positions as three floats, the indices as words); 297 of the 485 props here have one, a trunk of 12 triangles or a fence of 28, and a bush has none and no file. A prop converted before the `.wwc` existed is walked through, and `prepare_tile.sh` will not do it again since it has its `.wwb`: delete the props' `.wwb` and the buildings' `.wwd` and run it. A model with nothing left to draw (`fireflies01.m2`), or with no view at all (the particle emitters and smokes of the Barrens), is refused with exit code 3, and the engine logs its missing `.wwb` once. The map files name a model `.mdx` and the file is `.m2`, so `adt2wot` writes `.m2`.

A building also holds props inside it: its tables, lamps, barrels, rugs and chandeliers, 67 kinds in the Goldshire inn. They are in its root `.wmo`, not in the map: MODD lists them (40 bytes each: a name that is a byte offset into MODN, position, a quaternion x y z w, scale) and MODS groups them into sets. `wmo2wwb` writes them to `name.wwd` (`WWD1`: model paths as `.wwb`, sets as first and count, props as model, position, quaternion, scale), always, empty for a building with none, and prints a `prop <m2>` line for each model, which `prepare_tile.sh` converts. A placement draws set 0 and the set named by its `doodadSet`. The prop's matrix is the building's own placement matrix times the prop's place in the building (translate, quaternion, scale), in the building's axes with no reflection; taken as written, the chairs stand upright around their table and the chandeliers hang. A building converted before the `.wwd` existed has none, so `prepare_tile.sh` converts it again when the `.wwd` is missing.

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
- Materials have four blend modes, the game's own numbers: 0 opaque, 1 alpha-test (cut at 0.5), 2 alpha and 3 additive. The buildings of Goldshire use only 0 and 1; 32 materials of the props use 2 (smoke, waterfalls, candles, light shafts, ghostly tombstones) and none uses 3 there. A blended material is drawn after everything solid, its instances far to near, on two more pipelines that test depth and do not write it and are not culled; an additive one fades to nothing in the fog and not to the fog colour. Order is by instance and not by triangle, so two blended things that cross each other can be wrong. Vertex colours are baked light and are multiplied in.
- A prop's placement is the building rule above, then the scale (`scale / 1024`, uniform, about the model's own origin). No stored box exists to check a prop against, so it was checked against the ground: of the 1243 props of tile (31, 49) that stand over ground, the height they are placed at is 0.00 yards from `pe_terrain_heights_at()` at the median, within 0.44 for 90% and within 4.3 for 99% (the rest stand on tables, roofs and docks). That checks the position and the flip. The rotation was judged by eye: fences follow the slope and posts stand upright.
- Models are static here. A classic model has bones and animations, and this reads none of it: vertices are drawn as stored. The trees have one bone with no flags, so nothing is lost. A model with billboard bones, whose leaf cards the game turns to face the camera, is drawn with the cards as authored.
- Collision is a mesh in a grid of 4 yard cells over the model's X and Y (`terrain_collision.c`), and a building's is every triangle of its `.wwb`, the drawn ones and the collision-only ones (26 kinds in the Goldshire block, the biggest 54000 triangles, a crypt), where a prop's is its `.wwc`. A floor question on the inn, 26000 triangles, takes about 6 microseconds. A triangle within about 53 degrees of level is a floor or a ceiling and the rest are walls. A floor is found by a ray down from a step above the feet, and the nearest walkable triangle is the floor, so an upper floor or a roof overhead is not, and the top of a crate lower than a step is stood on; a wall pushes a sphere out sideways only, a few times over for a corner. A prop's scale is taken out of the ray and the radius. The game leaves some triangles of a building out of collision by a flag (`MOPY`) that `wmo2wwb` does not keep, so a decoration that the game would walk through can block here. It was checked by walking a scripted camera: at the Goldshire inn, blocked by its chimney and by the porch's door frame, in through the porch and up onto its floor; at a fence, stopped half a yard short of it; past a tree's thin mid trunk, pushed 0.7 yards to the side. The big trees have a wide flare of trunk as collision, 3 to 8 yards across at the root.
- **The trees are big**, canopies 100 yards across and trunks over 30 high, and the old start position 85 yards up is now inside one. Long thin green streaks over the canopy seen from above are the edges of its large flat leaf planes, which is how the game models them.
- The Goldshire block has 22 chunks with **hole masks**, and the inn stands in one of them. Rectangular black-looking gaps in the ground are legitimate holes (building and cave footprints), not rendering cracks.

## How the program is put together

`pwow.c` fills a `PGame` and calls `pengine_run()`. Then:

1. `init` builds the world: `pe_vk_terrain_world_create()`, then `pe_vk_terrain_world_stream()` called until it has nothing left to load around the start position.
2. `update` moves the camera from `input.<KEY>.pressed`, scaled by `delta_time` (seconds), then calls `pe_vk_terrain_world_stream()` once, which loads at most one tile.
3. The engine calls the `pe_vk_draw_scene` hook every frame. `pwow_draw_scene` fills a `PTerrainFrame` (camera via `pe_terrain_frame_set_camera()`, which also stamps the time the water animates by, then lighting, fog and sky colours) and hands it to `pe_vk_terrain_world_draw()`, which uploads it, draws the sky, the ground of every tile, the buildings and props, and last the water, which blends over everything.

Update and draw both run on the main thread, so the camera is not raced.

Streaming, in pengine (`terrain_world.c`), worked out against the Barrens with a scripted camera flying five tiles out and back:

- A tile is wanted while the camera is within the distance of its rectangle, and kept until it is `PE_TERRAIN_STREAM_MARGIN` (100 yards) farther, so a border crossed back and forth does not load and unload the same tiles. The distance plus the margin has to stay under two tiles (1066 yards), or more than the 5x5 slots can be wanted.
- Loading is on the main thread, one tile a call, nearest first, and it is the hitch: a tile costs 20 to 60 ms once its buildings and textures are known, and 100 to 480 ms when it brings new kinds of prop and their textures (the first tile at the Crossroads took 485 ms, 430 of them in `pe_vk_terrain_buildings_add_tile`). A loader thread would need the file reads and the mesh building off the main thread, and the Vulkan uploads left on it.
- A tile's mesh is built with the eight tiles around it read from disk again each time, loaded or not, so the border normals of a tile do not depend on the order the tiles came in.
- Unloading waits for the gpu to be idle (`vkDeviceWaitIdle`, once per call), then gives back the tile's buffers, alpha atlas and descriptor pool, and takes away the buildings and props it placed.
- A building on a border is listed by every tile it touches and is added once, by the first, with each tile that lists it recorded as an owner (up to four); it stands until the last owner is unloaded. A prop inside a building carries the building's unique id and owners. Instance counts come back exactly the same on the way back over a tile (7147 at the start and at the return), which is how that was checked.
- A kind of building or prop is loaded when the first placement of it stands and given back when the last is gone, with its descriptor sets (the pool is made with `FREE_DESCRIPTOR_SET_BIT`), buffers and collision. One that could not be loaded is kept, holding nothing, so it is not tried again by every tile.
- Textures are counted by what uses them and are kept after the last user lets go, in a cache of 2048, and the one unused for longest is given back only when the cache is full. Around a Barrens tile the live set is 220 textures for 5 tiles and 620 for 21; a cache too small for what is live logs `no room for texture` and shows the magenta checker, which was tried with caches of 400 and 620.
- `pe_vk_destroy_buffer()` takes a buffer out of the list `pe_vk_end()` destroys at exit, or it would be destroyed twice.

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

## Creatures (live client)

`creatures.c`/`.h` render the "simple" (non-humanoid) creatures `pe_wowworld_poll()` tracks: a `CreatureDisplayInfo.dbc` row with `ExtendedDisplayInfoID == 0`, whose look is its own model+texture rather than the player-character race/gender/gear pipeline (`CreatureDisplayInfoExtra`). A humanoid creature is tracked the same way but silently never given an instance - see `TODO.md`.

One `CreatureTemplate` per unique species (glb path, converted by `prepare_creatures.sh`) holds the shared, read-only half of a skinned model - mesh, textures, joint topology, animation clips - loaded once via pengine's `pe_vk_load_skin()`. Each tracked creature gets its own `CreatureInstance` with its own `PSkin`, built by pengine's `pe_vk_skin_instance()`: it copies out a skin's joints and animation-clip shells per instance (a channel/parent pointer is remapped via each joint's own index, since it points into the *source* skin's joints array) while sharing the mesh/texture/keyframe-data half. Without this, every instance of a species shared one `PSkin` outright and played one clip in lockstep, since pengine's `play_animation_by_name()`/`play_animation_list()` key playback off a `PSkin` pointer, not an instance.

Each instance switches every frame between its species' idle/walk/run clip (resolved by name once per species in `find_or_load_template()`, falling back to whichever clip the species actually has) from `PWowCreature.moving`/`.walking` (`pengine/src/engine/wowauth/wowobject.h`). `walking` is the real `PRE_WOTLK_RUNMODE` bit off the last `SMSG_MONSTER_MOVE`'s own spline flags (WoWee's `spline_packet.hpp`: set means Run, clear means Walk on this pre-WotLK wire) - `pe_wowobject_handle_monster_move()` already parsed that flags word for the Catmull-Rom/cyclic bits, so reading this one too was free.

## Conventions

- Commit messages are lowercase imperative summaries with a prose body explaining the mechanism and consequence, not a bullet list of edits. End them with the attribution line from the session's reminder.
- Code is written to need no comment; where one exists it is a `//INFO` line explaining *why* something non-obvious holds.
