#!/bin/bash
# usage: prepare_character.sh
# turns the game data pwow's character rendering needs into data/: the
# CharSections.dbc that maps a race/sex/skin id to a body texture, and every
# Tauren Male body skin tone CharSections.dbc names (there is only one
# converted model, taurenmale.glb, made by hand with m22gltf - see TODO.md).
# a file already converted is left alone
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}
DBC_SOURCE=${DBC_SOURCE:-/data/sources/vmangos/run/bin/5875/dbc}

here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

mkdir -p "$out/dbc"

if [ ! -f "$out/dbc/CharSections.dbc" ]; then
  cp "$DBC_SOURCE/CharSections.dbc" "$out/dbc/CharSections.dbc"
fi

if [ ! -f "$out/dbc/ItemDisplayInfo.dbc" ]; then
  cp "$DBC_SOURCE/ItemDisplayInfo.dbc" "$out/dbc/ItemDisplayInfo.dbc"
fi

if [ ! -f "$out/dbc/CreatureDisplayInfoExtra.dbc" ]; then
  cp "$DBC_SOURCE/CreatureDisplayInfoExtra.dbc" "$out/dbc/CreatureDisplayInfoExtra.dbc"
fi

for id in $(seq 0 18); do
  png=$(printf "character/tauren/male/taurenmaleskin00_%02d.png" "$id")
  blp="${png%.png}.blp"

  if [ -f "$out/$png" ]; then
    continue
  fi
  mkdir -p "$out/$(dirname "$png")"
  cp "$GAME_DATA/$blp" "$out/$blp"
  "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
  rm "$out/$blp"
done

convert() {
  png=$1
  blp="${png%.png}.blp"

  if [ -f "$out/$png" ] || [ ! -f "$GAME_DATA/$blp" ]; then
    return
  fi
  mkdir -p "$out/$(dirname "$png")"
  cp "$GAME_DATA/$blp" "$out/$blp"
  "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
  rm "$out/$blp"
}

# the layers composited onto a body skin: the faces (5 shapes in every skin
# tone), the scalp of each hair colour and the underwear of each skin tone.
# a layer the game does not ship is skipped, the way the client does without it
for id in $(seq 0 18); do
  for face in $(seq 0 4); do
    convert "$(printf "character/tauren/male/taurenmalefacelower%02d_%02d.png" "$face" "$id")"
    convert "$(printf "character/tauren/male/taurenmalefaceupper%02d_%02d.png" "$face" "$id")"
  done
  convert "$(printf "character/tauren/male/taurenmalenakedpelvisskin00_%02d.png" "$id")"
  convert "$(printf "character/tauren/male/taurenmaleskin00_%02d_extra.png" "$id")"
done

for color in 0 1 2; do
  convert "$(printf "character/tauren/scalplowerhair00_%02d.png" "$color")"
  convert "$(printf "character/tauren/scalpupperhair00_%02d.png" "$color")"
done

echo "prepared character data"
