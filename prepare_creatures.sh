#!/bin/bash
# usage: prepare_creatures.sh
# converts every "simple" (non-humanoid) creature model CreatureDisplayInfo.dbc
# names - one whose look comes from its own model and texture rather than
# CreatureDisplayInfoExtra's race/gender/skin/face/hair/equipment, the same
# pipeline a player character uses and which this does not handle - into
# data/: model -> m22gltf -> .glb, texture -> blp_convert -> .png, same as
# prepare_tile.sh converts terrain. no arguments: resolve_creatures finds
# everything itself from the DBCs, a few hundred models total, so this
# converts them all in one bounded pass, same as prepare_character.sh. a
# model or texture already converted is left alone. m22gltf is given
# AnimationData.dbc so a clip is named after its sequence (Stand, Walk, Run):
# without it every clip is "anim_<n>" and creatures.c cannot tell them apart.
# a model converted before that has to be deleted to be converted again.
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}
DBC_SOURCE=${DBC_SOURCE:-/root/sources/vmangos/run/bin/5875/dbc}

here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

mkdir -p "$out/dbc"

for dbc in CreatureDisplayInfo CreatureModelData; do
  if [ ! -f "$out/dbc/$dbc.dbc" ]; then
    cp "$DBC_SOURCE/$dbc.dbc" "$out/dbc/$dbc.dbc"
  fi
done

convert_texture() {
  local png=$1
  local blp="${png%.png}.blp"

  if [ -f "$out/$png" ]; then
    return
  fi
  #a display row can name a texture variation the game data does not
  #actually have (a stale or removed DBC entry, seen in practice scanning
  #the whole table rather than one already-known-good tile) - skip it
  #rather than let a bulk pass over thousands of rows die on the first one
  if [ ! -f "$GAME_DATA/$blp" ]; then
    echo "skipped $blp: the game has no such texture" >&2
    return
  fi
  mkdir -p "$out/$(dirname "$png")"
  cp "$GAME_DATA/$blp" "$out/$blp"
  "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
  rm "$out/$blp"
}

# the converters print what they need done, one thing to a line
convert_textures_named_by() {
  local kind path

  while read -r kind path; do
    if [ "$kind" = texture ]; then
      convert_texture "$path"
    fi
  done <<< "$1"
}

convert_model() {
  local model=$1
  local glb="${model%.m2}.glb"
  local lines status=0

  if [ -f "$out/$glb" ]; then
    return
  fi

  lines=$("$here/m22gltf" "$GAME_DATA" "$model" "$out" "$DBC_SOURCE/AnimationData.dbc") || status=$?
  if [ "$status" -eq 3 ]; then
    return
  fi
  if [ "$status" -ne 0 ]; then
    echo "could not convert $model" >&2
    return
  fi

  convert_textures_named_by "$lines"
}

"$here/resolve_creatures" "$out/dbc" | while IFS=$'\t' read -r kind model texture; do
  [ "$kind" = creature ] || continue

  convert_model "$model"

  if [ "$texture" != "-" ]; then
    convert_texture "${texture%.blp}.png"
  fi
done

echo "prepared creatures"
