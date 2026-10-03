# pwow

A camera that flies over real World of Warcraft **Classic 1.12** terrain,
built on the author's own Vulkan engine, [pengine](https://github.com/oscar0pavon/pengine).
It reads the user's own extracted game data — nothing here ships or
distributes Blizzard's assets.

A second, newer direction (`--live`) turns it into a real client of a local
vmangos server: a walking character and real NPCs, not just flown-over
ground. See `TODO.md` for what's done and what's missing.

## Dependencies
- make, gcc
- [pengine](https://github.com/oscar0pavon/pengine) (built and available as a sibling checkout `../pengine`, or pass `WORKDIR=/path/to/pengine` to `make`)
- OpenSSL and zlib (for the vmangos client in `wowauth/`)
- expat (`tools/xml2ui` reads the game's interface XML)
- [WoWee](https://github.com/wowserhq/wowee) (`blp_convert`, for the data pipeline)
- your own extracted Classic 1.12 game data

## Build
```
make -C ../pengine -j24        # the engine first
make                           # here: builds ./pwow and the tools/ converters
```

`pengine` is a static library; `main.c` and the other sources are only
compared against `libpengine.a`'s timestamp, so after an engine change
rebuild the engine and then `make -B pwow` here. Use `make pwow`, not a bare
`make`, to rebuild the program.

## Convert some terrain and run
```
./prepare_tile.sh kalimdor 36 32 2   # once: the 5x5 block around the Crossroads
./pwow                               # loads data/ by relative path
./prepare_tile.sh azeroth 31 49 2    # Goldshire, around its inn
./pwow azeroth 31 49                 # start over the middle of a tile of any map
```

With no arguments it starts over the Crossroads in the Barrens, looking
south. `data/` is gitignored: the textures and models come from your own
install and are converted locally, never committed.

## The interface
In live mode pwow draws the game's own interface from its FrameXML, without
Lua: the player and target frames, the main action bar with its spell icons,
the backpack and bags with their items and money, and tooltips. Convert its
textures, icons and dbc files once:
```
./prepare_ui.sh                      # needs make, expat and WoWee's blp_convert
```

## Live mode
With a vmangos server running (see `TODO.md`):
```
./pwow --live 127.0.0.1 3724 <account> <password>
```
It logs in as the account's first character and shows the real world around
it: creatures, the character's equipment and weapons, its health, level, XP,
action bar, bags and money.

## Controls
W A S D move, Space / C up and down (flying only), I K pitch, J L turn, Shift
is faster, Tab toggles between flying and walking. Walking only: A/D turn
while the right mouse button is up and strafe while it is held, Q/E always
strafe — the same as retail. In live mode: F held attacks, the mouse works the
interface (hover, click, right click an item to use or equip it, left click to
pick it up and drop it on another slot, Esc puts it down), the keys 1 to 9 and
0 are the action bar's slots, and B opens or shuts the bags.

## How it fits together
`main.c` fills a `PGame` and calls `pengine_run()`; nearly all terrain,
building and prop logic lives in pengine's `src/engine/terrain/`, and this
repo is the application — and the exercise — on top of it. `wowauth/` is the
one part that is pwow's alone rather than the engine's: login, realm and
world-protocol code for a real vmangos server, which pengine (a general
Vulkan engine) has no use for and cannot reuse for any other game.

See `CLAUDE.md` for the full data pipeline, coordinate conventions, and the
detail behind terrain, buildings, props and collision.
