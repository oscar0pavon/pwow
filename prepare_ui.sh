#!/bin/bash
# converts the textures the interface frames use, from the game data into data/
#   ./prepare_ui.sh
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

#what the game's Lua picks and the XML does not name
extra_textures="interface/buttons/ui-quickslot.png"

{
  for png in $extra_textures; do echo "texture $png"; done
  "$here/xml2ui" "$GAME_DATA" "$here/ui_frames.c" PlayerFrame TargetFrame MainMenuBar $(seq -f ActionButton%g 1 12) 2>/dev/null
} |
  while read -r kind png; do
    [ "$kind" = texture ] || continue
    [ -f "$out/$png" ] && continue
    blp="${png%.png}.blp"
    mkdir -p "$out/$(dirname "$png")"
    cp "$GAME_DATA/$blp" "$out/$blp"
    "$BLP_CONVERT" --to-png "$out/$blp" > /dev/null
    rm "$out/$blp"
  done
