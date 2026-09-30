# TODO

Future work for `pwow`/`pengine`, grouped by area. Anything here is unstarted
unless marked otherwise.

**wowauth moved to pwow.** The vmangos login/realm/world-protocol code
(`wowauth.c/.h`, `wowworld.c/.h`, `wowobject.c/.h`, `wowdbc.c/.h`,
`wow_wire.h`) used to live under `pengine/src/engine/wowauth/`; it is now
`pwow/wowauth/`, since it is WoW-1.12-specific and pengine (a general Vulkan
engine) had no other use for it and could not reuse it for another game.
References below to `wowauth/...` mean this repo's copy, not pengine's.

## Live client (the main direction)

Goal: `pwow` becomes a real client of the local vmangos server, rendering
actual NPCs instead of only static terrain. See memory
`project-pwow-live-client-goal` / `project-vmangos-server` for the full
history, credentials and file:line references behind these items.

1. **Done**: parse `SMSG_UPDATE_OBJECT`/`SMSG_COMPRESSED_UPDATE_OBJECT` for
   creature spawn position + display id. New module `wowauth/wowobject.h`/`.c`
   (now pwow's own — see "wowauth moved to pwow" below), dispatched by a new
   `pe_wowworld_poll()`
   (`wowworld.h`/`.c`) that `pwow_update()` calls once a frame in live mode;
   the world connection is now kept open after login instead of closed. HUD
   lists the nearest few tracked creatures (entry, display id, distance) to
   prove it end to end — see "Text / HUD" below for what's next now that
   there's real entity data to draw.
2. **Done**: render "simple" (non-humanoid) creatures as real models -
   `CreatureDisplayInfo.dbc` rows with `ExtendedDisplayInfoID == 0`, whose
   look comes from their own model+texture rather than
   `CreatureDisplayInfoExtra`'s race/gender/skin/face/hair/equipment (the
   player-character pipeline, which this does not drive - see item 5).
   `tools/resolve_creatures.c` (new, linked against `wowauth/` to reuse `wowdbc.h`) finds
   every such display id and prints its model + any texture-variation
   override; `prepare_creatures.sh` (new, run once, no arguments) converts
   them all via `m22gltf`/`blp_convert` into `data/`, same shape as
   `prepare_tile.sh`. New `creatures.h`/`.c` resolves a tracked creature's
   display id the same way at runtime (cached), loads one `PModel` template
   per unique species and a `pe_vk_model_instance()` copy per tracked
   creature, and draws them from `pwow_draw_scene()`. Verified live: a
   Plainstrider (Tallstrider model) renders correctly textured at its real
   tracked position near Camp Narache.
   - Found and fixed a real, previously-latent pengine gap along the way:
     `pe_vk_pipeline_layout` (`renderer/descriptor_set.h`) is declared and
     destroyed at shutdown but was never actually created anywhere in
     `vulkan.c`'s init - nothing had ever used it, since `pe_vk_load_model()`'s
     own plain descriptor sets were always either thrown away and rebuilt
     skinned (the player) or never paired with a real pipeline before now.
     Worked around by using `pe_vk_pipeline_layout_with_descriptors` instead
     (the one `vulkan.c` actually builds against the same
     `pe_vk_descriptor_set_layout` `pe_vk_load_model()` uses, already proven
     working by `gui.c`'s button quads) rather than fixing the gap itself.
   - Also worth remembering: `pe_vk_model_instance()` shares its source's
     vertex/index buffers by design, but `pe_clean_model()` unconditionally
     frees them - calling it on an instance would free geometry the template
     (and any other instance of the same species) still needs. Creature
     instance teardown (`creatures.c`'s `release_creature_instance()`) only
     frees the uniform buffers and descriptor pool an instance actually owns,
     deliberately not `pe_clean_model()`.
   - **Done**: creatures animate, and switch clips with real movement state
     instead of standing frozen on one idle pose forever. Needed the
     per-instance animation fix (Animation item 2 below) first, or every
     tallstrider on screen would have switched state in lockstep. Each
     instance now plays its species' idle/walk/run clip (resolved by name
     once per species, falling back to whichever clip the species actually
     has, `creatures.c`'s `find_or_load_template()`) from `PWowCreature.
     moving`/`.walking` (`wowauth/wowobject.h`) every frame -
     `play_animation_by_name()` no-ops on a repeat request, so this costs
     nothing once a creature settles into a state. `walking` is the real
     `PRE_WOTLK_RUNMODE` bit off the last `SMSG_MONSTER_MOVE`'s own spline
     flags (WoWee's `spline_packet.hpp`: set means Run, clear means Walk for
     this pre-WotLK wire), not a guess - `pe_wowobject_handle_monster_move()`
     was already parsing that flags word for the Catmull-Rom/cyclic bits and
     throwing the rest away.
3. Humanoid NPCs (most of what's actually near Camp Narache - the Tauren
   quest-givers) still render as nothing: their `CreatureDisplayInfo` row
   points at `CreatureDisplayInfoExtra` (race/gender/skin/face/hair/
   equipment) and names no texture of its own, confirmed their
   `CreatureModelData.modelName` is literally `Character\Tauren\Male\
   TaurenMale.mdx`/`Female\...` - the same models the player uses. Extending
   the player's character pipeline to also drive NPCs from
   `CreatureDisplayInfoExtra` would cover them, but needs per-NPC skin/face/
   hair resolution and likely new race/gender model conversions beyond the
   one Tauren-male model that exists today - a separate, much bigger feature,
   deliberately out of scope for item 2.
4. **Done**: right mouse button now turns the character to face wherever the
   camera looks, and releasing it lets J/L turn the character and camera
   together, instead of the two staying fully independent — WoWee's
   `CameraController::update()` (`cameraDrivesFacing`) is the reference,
   `update_live_character()` in `main.c` is the port. Verified it builds and
   runs against the live server (character and NPCs render at Camp Narache);
   not verified by an actual mouse drag, since there is no input-injection
   tool in this environment to drive one into the running window.
   `CHARACTER_MOVE_SPEED`, `CHARACTER_TURN_SPEED_DEGREES`, camera distance
   (top of `main.c`) are still untuned by hand.
5. Character creation (`CMSG_CHAR_CREATE`) isn't ported — the one test
   character was made with the real WoWee client. Only needed if pwow should
   be able to create a character itself.
6. Everything else of WoWee's `game/` module (chat, spells, quests, inventory,
   transports) is out of scope until something above needs it.

## Text / HUD

**Done**: `pfonts` (`/root/pfonts`) grew a Vulkan rendering backend
(`vulkan.c`/`.h`), alongside its existing GL and CPU-pixel-buffer ones, and
pengine wraps it as `engine/text.h`/`text.c` (`pe_text_init/sync/begin/
draw/end`), wired into `pe_vk_draw_frame()` for the atlas upload and
available to any `pe_vk_draw_scene` hook for drawing. `main.c`'s
`pwow_draw_scene` uses it for a one-line HUD (map name + player position) -
see `pfonts`'s own CLAUDE.md for the Vulkan backend's design and gotchas
(atlas upload has to happen outside the render pass; the pipeline's sample
count has to match the host's render pass's). This is the foundation for:

1. Nameplates/name-and-health text over entities, once item 1 of Live
   client above lands and there is something to label.
2. Anything else that wants on-screen text - an FPS counter, a chat line,
   debug readouts - can now call `pe_text_draw()` from inside
   `pwow_draw_scene` without new engine work.
3. Not done: a real widget/window abstraction, input focus, or anything
   past drawing a string at a position - see the `pfonts` CLAUDE.md's own
   backend doc for exactly what's covered.

## Animation

Context: `tools/m22gltf.c` bakes M2 bones/tracks into a glTF skin + one clip
per M2 sequence; pengine's `animation.c`/`skeletal.c`/`node.c` load and play
that generically. Two real bugs fixed this session (framerate-independent
playback + clean clip switching in pengine; the 383-second baked-duration bug
in `m22gltf.c`). Remaining gaps, in the order they'll probably bite:

1. **No animation blending/crossfade.** Switching Stand→Run is an instant cut
   (`play_animation_by_name` drops the old clip and starts the new one at
   time 0). WoWee's own `M2Sequence.blendTime` exists in the source data and
   isn't read; pengine has no blend-weight concept at all.
2. **Done**: per-instance animation state. `Animation.time`/`.loop` and a
   joint's `translation`/`rotation` all lived on the one `PSkin` a species'
   `pe_vk_load_skin()` call produced, and every instance shared that same
   `PSkin` pointer — fine for pwow's one player character, but every NPC of a
   kind played one shared clip in lockstep, since `play_animation_by_name()`/
   `play_animation_list()` key playback off the `PSkin` pointer. New
   `pe_vk_skin_instance()` (`pengine/src/engine/skeletal.h`/`model.c`) copies
   a skin's joints and animation-clip shells per instance (remapped via each
   `Node`'s own index, since a channel/parent pointer points into the
   source's joints array) while sharing the read-only half — mesh, textures,
   inverse bind matrices, keyframe sampler data. `creatures.c`'s
   `CreatureInstance` now owns one of these instead of sharing its species'
   template skin outright.
3. **Scale channels are dropped.** `Node` (`pengine/src/engine/animation/node.h`)
   has no scale field, and `pe_load_animations()` explicitly skips
   `cgltf_animation_path_type_scale` channels (`model.c`). Some M2 bones may
   depend on this (squash/stretch, some attachment bones); currently silently
   wrong rather than missing entirely.
4. **Rotation interpolation is linear, not slerp** (`glm_vec4_lerp` in
   `animation.c`'s `play_animation()`). Fine for slow bone motion, visibly
   wrong on fast rotations. WoWee uses `glm::slerp` for this exact reason.
5. **Normals aren't re-skinned.** `skinned.vert` only applies `mat3(ubo.model)`
   to normals, never the per-vertex blended joint matrix — lighting on a bent
   limb (an arm mid-swing) will look slightly wrong.
6. **Global-sequence tracks are dropped entirely** by `m22gltf`
   (`track->global_seq == -1` gate). Always-looping secondary motion not tied
   to a named sequence (idle cloth sway, jiggle) is lost. Low priority —
   cosmetic only.
7. No exposed way to query "is this animation currently playing / what's its
   duration" from pengine's animation API — would be needed for one-shot
   animations (jump, emote) to know when they've finished, or to sync
   footsteps to a Walk/Run cycle.
8. **Done**: pure strafing (A/D with no W/S held) now plays `ShuffleLeft`/
   `ShuffleRight` instead of `Run` - taurenmale.glb carries both clips, they
   just weren't picked. Forward/backward still wins the animation over a
   strafe, matching WoWee's `LocomotionFSM`'s `anyStrafeLeft`/
   `anyStrafeRight` (`locomotion_fsm.cpp`, which require `!movingBackward`).
   Backpedaling also moves at a new `CHARACTER_BACK_SPEED` (4.5 yd/s,
   WoWee's `WOW_BACK_SPEED` in `camera_controller.hpp`) instead of the run
   speed the `Walkbackwards` clip was never paced for - the feet used to
   slide against the ground. Still no speed-based Walk vs. Run distinction
   for forward movement: WoWee's own Ctrl-to-walk toggle (`WOW_WALK_SPEED`,
   2.5 yd/s) has no equivalent key in pengine's `Input` struct (`input.h`
   only has `SHIFT`, no `CTRL`), so adding it means an engine-side change
   first.

## Character rendering polish

1. **Lighting.** The character renders dark/underlit from most angles —
   `skinned.vert`'s `ambient_light` is a hardcoded flat `0.12`, so any surface
   facing away from the single fixed light direction is near-black, and
   third-person view mostly looks at the character's back. Needs either a
   better light direction or a higher ambient floor.
2. Only the Tauren male model exists (`data/character/tauren/male/`). Other
   races/genders would each need their own `m22gltf` conversion, and their own
   `prepare_character.sh`-style skin-texture conversion.
3. **`CharSections.dbc` body-skin resolution is done; face, hair and the other
   races/sexes are not.** `wowauth/wowdbc.c` is a minimal WDBC
   reader (record/field access only, no CSV/JSON fallback - pwow only ever
   reads real DBCs); `main.c`'s `resolve_player_skin_path()` scans
   `data/dbc/CharSections.dbc` (copied by `prepare_character.sh`, which also
   converts all 19 Tauren Male skin tones) for the Tauren Male skin row
   (`BaseSection` 0) matching `PLAYER_SKIN_ID`, and `player_load()` loads
   whichever PNG that resolves to instead of a hardcoded path. `PLAYER_SKIN_ID`
   is itself still a `#define` stand-in for the skin id `PLAYER_BYTES` would
   carry (see live-client item 1: nothing parses the player's own object
   update yet, so there is nothing to unpack `PLAYER_BYTES` from). Left for
   later, following WoWee's `char_sections.cpp` as the reference: face and hair
   texture rows (`BaseSection` 1 and 3), and the same resolution for every
   other race/sex once their models and skin textures are converted.
4. **No equipment rendering at all — a live character stands in whatever the
   base model+geoset defaults draw, never what is actually in its equipped
   slots.** Checked against a real WoW.exe screenshot of a Tauren Warrior:
   the belt, bracers, pants and the weapon on its back are all worn items, and
   none of them have any code path here. Two smaller gaps came up in the same
   comparison and are cheap to chase first: the fur reads solid black at
   `PLAYER_SKIN_ID 0` where the reference is a lighter grey-blue - probably
   just a different skin id, or the Tauren male fur is legitimately close to
   black and it is the fur/skin *shading* (see item 1) making it read as a
   flat silhouette - and the reference has a small dark mane tuft between the
   horns that this model may or may not carry; worth checking whether it is
   one of the geosets `pe_primitive_is_default` (`pengine/src/engine/model.c`)
   now omits for having no bare variant (see item 3's commit history there)
   before assuming it is simply missing from the conversion.
   Equipment itself is the real gap and a much bigger feature: it needs (a)
   knowing what is in each equipped slot; (b) an `ItemDisplayInfo.dbc` reader
   to turn an item id into its model/texture and which geoset group it
   drives; (c) per-slot geoset selection and texture compositing onto the
   model the way WoWee's `entity_spawner_player.cpp` does it, which is the
   reference implementation to follow.
   - **Done: (a) and (b), the data side, live and proven end to end** against
     the real vmangos server. `wowauth/wowobject.c` now decodes
     `PLAYER_VISIBLE_ITEM_1_0..19_0` (`UpdateFields_1_12_1.h`: `UNIT_END +
     0x48`, 12-dword stride, 19 slots) out of the local player's own
     CREATE_OBJECT/VALUES blocks into a new `PWowPlayerEquipment` on
     `PWowObjectState`, gated on a new `pe_wowobject_set_local_player_guid()`
     so it can tell "the local player's own object" apart from anyone else's.
     `wowworld.c` gained `pe_wowworld_query_item()` (`CMSG_ITEM_QUERY_SINGLE`/
     `SMSG_ITEM_QUERY_SINGLE_RESPONSE`) to resolve an item entry to its
     `ItemDisplayInfo` id and inventory type - there is no local item-template
     data this could come from instead (unlike creature display ids, vanilla
     ships no client-side `Item.dbc` with a displayid field in this data set).
     `main.c`'s `sync_player_equipment()` wires both together every live
     frame and resolves the result through `ItemDisplayInfo.dbc`
     (`resolve_item_display_info()`, `prepare_character.sh` now copies it
     too), logging geoset groups and the six body-region texture names for
     each equipped slot. Verified against the real server: an Acolyte's Robe
     manually equipped on the test character resolved correctly end to end
     (entry 57 -> displayInfo 12645, invType 20 -> `Robe_A_01Maroon_*`
     torso/leg/arm textures, geosetGroup1/3 both 1), matching the DB's own
     `item_template`/`ItemDisplayInfo.dbc` rows exactly.
     Two real bugs surfaced and got fixed along the way, both worth
     remembering: `pe_wowworld_player_login()` used to silently discard
     every packet while waiting for `SMSG_LOGIN_VERIFY_WORLD`, including (per
     its own old comment) "the player's own object update" - exactly the one
     block `PLAYER_VISIBLE_ITEM` fields arrive in, since the server sends it
     before world-enter confirms. It now takes a `PWowObjectState *` and
     folds object-update/monster-move packets into it while waiting, the
     same as `pe_wowworld_poll()` does (`pe_wowworld_query_item()` does the
     same for its own wait). And `wowobject.c`'s `parse_values_block()` mask
     buffer (`VALUES_MASK_BYTES_MAX`) was sized for a creature's ~188-field
     `UNIT_END` (128 bytes, 6 mask dwords) but the player's own full snapshot
     needs `PLAYER_END` = 1282 fields (41 mask dwords, 164 bytes) - over the
     old cap, so the whole block was silently refused and equipment was
     never seen at all until this was bumped to 256.
   - **Done: (c), the actual visual result - verified against the real
     server.** Needed a pengine change first: `PModel` now keeps every
     primitive's indices (`all_indices`) plus one `PGeosetBatch` per
     primitive (its geoset id and where its slice sits), alongside the
     `index_array` `pe_load_mesh()` still builds from the guessed defaults
     alone, so nothing already drawing a model changed. Three new calls -
     `pe_model_default_geosets()` (what the defaults actually were),
     `pe_model_has_geoset()` (does the model carry a given id at all, for
     falling back to bare when it doesn't), and
     `pe_model_set_active_geosets()` (rebuild `index_array` against an
     arbitrary set and re-upload the GPU index buffer - destroy-and-recreate,
     matching `terrain_world.c`'s own tile-unload convention of waiting for
     the gpu to be idle first, since equipment changes are rare enough not
     to need to be cheaper). `main.c`'s `apply_player_geosets()` ports
     WoWee's `entity_spawner_player.cpp` geoset-selection rules onto this -
     `eraseGroup`/`pickGeoset`/`equippedGeoset` and the per-invType group
     mapping (chest/robe → sleeves group 8 and, for a robe, kilt group 13;
     legs → group 13; feet → group 5; hands → group 4; wrists → group 8 if
     chest didn't already set it; waist → belt group 18; cloak → group 15) -
     minus helm/shoulder model attachment, weapons and belt/tabard art,
     still out of scope.
     The six texture-region fields (torso/leg/arm upper+lower, hand, foot)
     are composited onto the base body skin at fixed pixel rects on its
     256x256 atlas (WoWee's `compositeWithRegions()`'s own coordinate table -
     the Tauren male skin already is 256x256, so none of its upscaling
     applies here) by `main.c`'s `apply_player_texture()`. Getting the art
     itself is a new problem every other texture in this repo sidesteps: an
     item's region texture is only named once the live server sends it, so
     there is no offline `prepare_*.sh` step that could have converted it
     ahead of time - `resolve_item_region_texture()` converts the game's own
     `.blp` to a `data/` png at runtime, the first time a given one is
     needed (`fork`/`execv` of `$BLP_CONVERT`, no shell - the texture name
     ultimately comes off the wire, by way of `ItemDisplayInfo.dbc`), trying
     the gendered file first, then unisex, then bare
     (`item_textures.hpp`'s own resolution order; only ever the male
     spelling here). Verified against the real vmangos server: an Acolyte's
     Robe manually equipped on the test character now renders as an actual
     maroon cloth robe - torso, sleeves and a long skirt down past the
     knees - matching its real in-game look, not just a geometry change on
     an unshaded black silhouette.
     Weapons (a `LeftModel`/`RightModel` attached at a bone), helm/shoulder
     model attachment, and belt/tabard art are still out of scope.

## Engine/tooling cleanup

1. `array_remove_element()` (`pengine/src/engine/array.c`) treats every array
   as an array-of-pointers (`array_get_pointer`/`array_add_pointer`
   internally) — unsafe to call on a value array like
   `array_animation_play_list` (`PEAnimationPlay` by value). The animation
   switch-cleanup added this session works around it with a manual in-place
   compaction instead of using this function; worth fixing or renaming the
   function to make the pointer-only assumption explicit, so the next caller
   doesn't reach for it on a value array and get silent corruption.
2. `m22gltf.c`'s per-clip export loop reads the model's raw multi-minute
   shared keyframe timeline per bone; the sequence-duration clamp added this
   session (`SEQUENCE_TIMESTAMP_START`/`_END`) fixes the symptom but the
   underlying reason *why* some bones' "hold" ranges point at such distant
   timestamps in the first place is understood, not exhaustively verified
   across bones/models beyond the Tauren — worth a spot-check if another
   converted model shows animation weirdness.
