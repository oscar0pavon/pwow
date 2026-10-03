#!/bin/bash
# converts the textures the interface frames use, from the game data into data/
#   ./prepare_ui.sh
set -e

GAME_DATA=${GAME_DATA:-/root/sources/WoWee/Data/expansions/classic}
BLP_CONVERT=${BLP_CONVERT:-/root/sources/WoWee/build/bin/blp_convert}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/data

#what the game's Lua picks and the XML does not name
extra_textures="interface/buttons/ui-quickslot.png
  interface/containerframe/ui-backpackbackground.png
  interface/buttons/button-backpack-up.png
  interface/paperdoll/ui-paperdoll-slot-bag.png
  interface/tooltips/ui-tooltip-border.png
  interface/buttons/ui-panel-button-up.png
  interface/buttons/ui-panel-button-down.png
  interface/buttons/ui-panel-button-highlight.png
  interface/questframe/ui-quest-topleft.png
  interface/questframe/ui-quest-topright.png
  interface/questframe/ui-quest-botleft.png
  interface/questframe/ui-quest-botright.png
  interface/questframe/ui-questgreeting-topleft.png
  interface/questframe/ui-questgreeting-topright.png
  interface/questframe/ui-questgreeting-botleft.png
  interface/questframe/ui-questgreeting-botright.png
  interface/questframe/ui-horizontalbreak.png
  interface/questframe/ui-quest-bulletpoint.png
  interface/questframe/ui-questitemnameframe.png
  interface/questframe/ui-questtitlehighlight.png
  interface/questframe/ui-questitemhighlight.png
  interface/moneyframe/ui-moneyicons.png
  interface/gossipframe/availablequesticon.png
  interface/gossipframe/activequesticon.png
  interface/gossipframe/gossipgossipicon.png
  interface/gossipframe/bankergossipicon.png
  interface/gossipframe/battlemastergossipicon.png
  interface/gossipframe/bindergossipicon.png
  interface/gossipframe/healergossipicon.png
  interface/gossipframe/petitiongossipicon.png
  interface/gossipframe/tabardgossipicon.png
  interface/gossipframe/taxigossipicon.png
  interface/gossipframe/trainergossipicon.png
  interface/gossipframe/unlearngossipicon.png
  interface/gossipframe/vendorgossipicon.png"

{
  for png in $extra_textures; do echo "texture $png"; done
  "$here/xml2ui" "$GAME_DATA" "$here/ui_frames.c" $(make -s -C "$here" print-ui-frames) 2>/dev/null
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

# the spells of the action bar and their icons, from the same dbc files the server reads
DBC_SOURCE=${DBC_SOURCE:-/root/sources/vmangos/run/bin/5875/dbc}
mkdir -p "$out/dbc"
for dbc in Spell SpellIcon SpellShapeshiftForm; do
  [ -f "$out/dbc/$dbc.dbc" ] || cp "$DBC_SOURCE/$dbc.dbc" "$out/dbc/$dbc.dbc"
done

if [ ! -d "$out/interface/icons" ]; then
  mkdir -p "$out/interface/icons"
  cp "$GAME_DATA"/interface/icons/*.blp "$out/interface/icons/"
  "$BLP_CONVERT" --batch "$out/interface/icons" > /dev/null
  rm -f "$out"/interface/icons/*.blp
fi
