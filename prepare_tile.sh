#!/bin/bash
# usage: prepare_tile.sh <map> <x> <y> [radius]
# turns a tile of the game data, and the tiles radius away from it, into what
# pwow reads: data/<map>_<x>_<y>.wot, .whm and .wwt, a .wwb for each building
# and each prop they place, and a PNG for each texture any of them use. a
# texture, building or prop already converted is left alone, and a tile the game
# does not have is skipped
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

  if [ -f "$out/$png" ]; then
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

#converts a building with wmo2wwb or a prop with m22wwb, which exit with 3, and
#say why on their own, for one they will not take
convert_model() {
  local converter=$1
  local source=$2
  local wwb="${source%.*}.wwb"
  local lines status=0

  if [ -f "$out/$wwb" ]; then
    return
  fi

  lines=$("$here/$converter" "$GAME_DATA" "$source" "$out") || status=$?
  if [ "$status" -eq 3 ]; then
    return
  fi
  [ "$status" -eq 0 ] || { echo "could not convert $source" >&2; return 1; }

  convert_textures_named_by "$lines"
}

for ((y = centre_y - radius; y <= centre_y + radius; y++)); do
  for ((x = centre_x - radius; x <= centre_x + radius; x++)); do
    tile=${map}_${x}_${y}
    adt=$GAME_DATA/world/maps/$map/$tile.adt

    if [ ! -f "$adt" ]; then
      echo "skipped $tile: the game has no such tile"
      continue
    fi

    lines=$("$here/adt2wot" "$adt" "$out/$tile")
    convert_textures_named_by "$lines"

    while read -r kind path; do
      if [ "$kind" = building ]; then
        convert_model wmo2wwb "$path"
      elif [ "$kind" = prop ]; then
        convert_model m22wwb "$path"
      fi
    done <<< "$lines"

    echo "prepared $tile"
  done
done
