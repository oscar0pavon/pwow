# TODO

Future work for `pwow`/`pengine`, grouped by area. Anything here is unstarted
unless marked otherwise.

## Live client (the main direction)

Goal: `pwow` becomes a real client of the local vmangos server, rendering
actual NPCs instead of only static terrain. See memory
`project-pwow-live-client-goal` / `project-vmangos-server` for the full
history, credentials and file:line references behind these items.

1. **Done**: parse `SMSG_UPDATE_OBJECT`/`SMSG_COMPRESSED_UPDATE_OBJECT` for
   creature spawn position + display id. New pengine module
   `wowauth/wowobject.h`/`.c`, dispatched by a new `pe_wowworld_poll()`
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
   `tools/resolve_creatures.c` (new, pengine-linked to reuse `wowdbc.h`) finds
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
2. **No per-instance animation state.** `Animation.time`/`.loop` live on the
   shared `Animation` struct inside `PSkin.animations`, not per playing
   instance — fine for pwow's one player character, but breaks the moment two
   instances of the same skeleton (two NPCs of the same race) need to play
   different clips at different times.
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
8. Movement→animation selection in `update_live_character()` (`main.c`) is
   just three states (Stand/Run/Walkbackwards) picked from raw input flags.
   No strafe-specific animation (WoW has `ShuffleLeft`/`ShuffleRight`), no
   speed-based Walk vs. Run distinction (player currently always moves at
   run speed).

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
   races/sexes are not.** `pengine`'s `wowauth/wowdbc.c` is a minimal WDBC
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
   knowing what is in each equipped slot, which live networking does not
   parse yet (same networking gap as live-client item 1) or a vmangos
   `character_inventory`/`item_instance` DB read as a static fallback; (b) an
   `ItemDisplayInfo.dbc` reader (`wowauth/wowdbc.c` generalizes to this, it is
   not CharSections-specific) to turn an item id into its model/texture and
   which geoset group it drives; (c) per-slot geoset selection and texture
   compositing onto the model the way WoWee's `entity_spawner_player.cpp`
   does it, which is the reference implementation to follow. Each piece is
   substantial on its own; this is a multi-session feature, not a tweak.

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
