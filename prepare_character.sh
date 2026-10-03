#!/bin/bash
# usage: prepare_character.sh
# turns the game data pwow's character rendering needs into data/: the
# CharSections.dbc that maps a race/sex/skin id to a body texture, and every
# Tauren body skin tone, face, scalp and underwear it names, male and female
# and the two models, taurenmale.glb and taurenfemale.glb, through m22gltf with
# AnimationData.dbc so their clips carry the game's names (Stand, Walk, Run):
# without it a clip is just "anim_<n>" and nothing picks the right one.
# a file already converted is left alone
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}
DBC_SOURCE=${DBC_SOURCE:-/data/sources/vmangos/run/bin/5875/dbc}

here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

mkdir -p "$out/dbc"

for model in character/tauren/male/taurenmale character/tauren/female/taurenfemale; do
  if [ ! -f "$out/$model.glb" ] || [ ! -f "$out/$model.att" ]; then
    "$here/m22gltf" "$GAME_DATA" "$model.m2" "$out" "$DBC_SOURCE/AnimationData.dbc" > /dev/null
  fi
done

if [ ! -f "$out/dbc/CharSections.dbc" ]; then
  cp "$DBC_SOURCE/CharSections.dbc" "$out/dbc/CharSections.dbc"
fi

if [ ! -f "$out/dbc/ItemDisplayInfo.dbc" ]; then
  cp "$DBC_SOURCE/ItemDisplayInfo.dbc" "$out/dbc/ItemDisplayInfo.dbc"
fi

for dbc in CreatureDisplayInfoExtra HelmetGeosetVisData; do
  if [ ! -f "$out/dbc/$dbc.dbc" ]; then
    cp "$DBC_SOURCE/$dbc.dbc" "$out/dbc/$dbc.dbc"
  fi
done

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

# the female body: her skin tones, faces, underwear (pelvis and torso) and
# the mane sheet. the game ships only some faces per skin tone
for id in $(seq 0 18); do
  convert "$(printf "character/tauren/female/taurenfemaleskin00_%02d.png" "$id")"
  convert "$(printf "character/tauren/female/taurenfemaleskin00_%02d_extra.png" "$id")"
  convert "$(printf "character/tauren/female/taurenfemalenakedpelvisskin00_%02d.png" "$id")"
  convert "$(printf "character/tauren/female/taurenfemalenakedtorsoskin00_%02d.png" "$id")"
  for face in $(seq 0 3); do
    convert "$(printf "character/tauren/female/taurenfemalefacelower%02d_%02d.png" "$face" "$id")"
    convert "$(printf "character/tauren/female/taurenfemalefaceupper%02d_%02d.png" "$face" "$id")"
  done
done

for color in 0 1 2; do
  convert "$(printf "character/tauren/scalplowerhair00_%02d.png" "$color")"
  convert "$(printf "character/tauren/scalpupperhair00_%02d.png" "$color")"
done

echo "prepared character data"
