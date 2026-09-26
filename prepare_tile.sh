#!/bin/bash
# usage: prepare_tile.sh <map> <x> <y> [radius]
# turns a tile of the game data, and the tiles radius away from it, into what
# pwow reads: data/<map>_<x>_<y>.wot and .whm, and a PNG for each texture they
# use. a texture already converted is left alone, and a tile the game does not
# have is skipped
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}

map=$1
centre_x=$2
centre_y=$3
radius=${4:-0}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

mkdir -p "$out"

convert_texture() {
  local png=$1
  local blp="${png%.png}.blp"

  [ -f "$out/$png" ] && return
  mkdir -p "$out/$(dirname "$png")"
  cp "$GAME_DATA/$blp" "$out/$blp"
  "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
  rm "$out/$blp"
}

for ((y = centre_y - radius; y <= centre_y + radius; y++)); do
  for ((x = centre_x - radius; x <= centre_x + radius; x++)); do
    tile=${map}_${x}_${y}
    adt=$GAME_DATA/world/maps/$map/$tile.adt

    if [ ! -f "$adt" ]; then
      echo "skipped $tile: the game has no such tile"
      continue
    fi

    textures=$("$here/adt2wot" "$adt" "$out/$tile")
    while read -r png; do
      convert_texture "$png"
    done <<< "$textures"
    echo "prepared $tile"
  done
done
