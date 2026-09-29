# TODO

Future work for `pwow`/`pengine`, grouped by area. Anything here is unstarted
unless marked otherwise.

## Live client (the main direction)

Goal: `pwow` becomes a real client of the local vmangos server, rendering
actual NPCs instead of only static terrain. See memory
`project-pwow-live-client-goal` / `project-vmangos-server` for the full
history, credentials and file:line references behind these items.

1. **Parse `SMSG_UPDATE_OBJECT`/`SMSG_COMPRESSED_UPDATE_OBJECT`** to get
   creature spawn position + display id and render them. `pe_wowworld_read_packet`
   is the primitive; `pe_wowworld_player_login` currently reads and discards
   every packet between login and `SMSG_LOGIN_VERIFY_WORLD` (spells, action
   bars, reputation, the player's own object update) — that discard loop needs
   to become a real dispatch loop, since entity data is very likely already
   flying by unparsed in that window.
2. **Fallback/interim option**: skip live networking for now and read creature
   spawns directly from the vmangos world database (`creature`/`creature_template`
   tables, 66,243 spawns / 15,217 templates already loaded) for a static NPC
   pass into pwow.
3. Tune live-character feel by hand: `CHARACTER_MOVE_SPEED`,
   `CHARACTER_TURN_SPEED_DEGREES`, camera distance (top of `main.c`) — never
   actually played with, only reasoned about.
4. Character creation (`CMSG_CHAR_CREATE`) isn't ported — the one test
   character was made with the real WoWee client. Only needed if pwow should
   be able to create a character itself.
5. Everything else of WoWee's `game/` module (chat, spells, quests, inventory,
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
