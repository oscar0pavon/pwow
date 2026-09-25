#!/bin/bash
# usage: prepare_tile.sh <map> <x> <y>
# turns one tile of the game data into what pwow reads: data/<map>_<x>_<y>.wot
# and .whm, and a PNG for each texture the tile uses
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}

map=$1
tile=${map}_$2_$3
here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

mkdir -p "$out"

textures=$("$here/adt2wot" "$GAME_DATA/world/maps/$map/$tile.adt" "$out/$tile")

while read -r png; do
  blp="${png%.png}.blp"
  mkdir -p "$out/$(dirname "$png")"
  cp "$GAME_DATA/$blp" "$out/$blp"
  "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
  rm "$out/$blp"
done <<< "$textures"

echo "prepared $out/$tile"
