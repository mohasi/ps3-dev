# simple-cheat-menu

A VSH plugin that shows an in-game cheat menu over any running game and toggles
cheats on and off live, patching the game's memory through cobra. It also
downloads cheats for the running game and, optionally, lets you vote on which
cheats work — all from the console, no PC needed.

## Using it

Press the **PS button** during a game. A short press opens the cheat menu (a
dark overlay with a centered panel); a long press is left alone so the normal
quit/power menu still works. If the game has no cheats, a short press just shows
a small notice and hands the pad straight back.

| Button | Action |
|---|---|
| **D-pad up/down** | move the selection (hold to auto-repeat) |
| **Cross** | toggle the selected cheat on/off |
| **Triangle** | re-download this game's cheats (online modes only) |
| **Start** | mark the selected cheat as *working* (contribute mode) |
| **Select** | mark the selected cheat as *failed* (contribute mode) |
| **Circle** | back to the in-game XMB |
| **PS** | resume the game |

Each row shows the cheat name, an **AoB** tag if it patches by byte-pattern, and
a crowd **score %** coloured by how well it fits your exact game build: green =
proven to work here, amber = unknown, red = proven only on a different build.

## A note on risk

Cheats change a running game's memory, and the cheats come from the community, so
they aren't guaranteed correct for your exact game version. Most are harmless — a
wrong one usually just does nothing or crashes the game back to the XMB.

The exception is cheats that patch the game's **code** rather than a simple value
(many **AoB** cheats do this). If a code patch is wrong for your build it can, in
rare cases, freeze the whole console and need a hard restart — because the game
shares the graphics hardware with the XMB, so a game that hangs the graphics chip
takes the XMB down with it. The plugin can't tell in advance whether a code patch
is safe; it only confirms the patch was written correctly.

So the decision is yours, and the **score badge** is your guide: green means the
community has confirmed the cheat on your exact build, amber means it's unproven
(try it at your discretion), red means it's only worked on a different build. Mark
cheats **working** or **failed** so that signal gets better for everyone.

## How it works

- One VSH PRX does everything. It draws the overlay through PAF (the XMB's own
  UI framework, which composites on top of the game) and pokes the game's memory
  cross-process through cobra — nothing is injected into the game itself.
- The PS button is the only trigger. A PS overlay opening is detected by pad
  activity while in-game; the plugin reads the raw pad to tell a short press from
  a long one. While the menu is up, vsh's libpad is blinded and the plugin reads
  the pad directly through `sys_hid_manager_read`. Raw reads happen only while
  captured — polling them across a pad-ownership change hard-locks lv2, so the
  plugin always restores libpad before any transition.
- Four threads, no locks: the **menu** thread reads the pad and sets per-row
  intent; the **frame** hook (a `Framework_Begin` export detour) owns all the
  widgets and paints on vsh's compositor thread; a **worker** thread does the
  slow scan-and-poke off the menu; short-lived **fetch/vote** threads do the
  network calls. They coordinate through volatile flags and memory fences.
- Toggling a cheat snapshots the original bytes, pokes the new value, then reads
  it back to confirm it landed — a row only shows ON if the poke verified, else
  it rolls back and stays OFF. AoB cheats scan the game's real module segments
  for the pattern first. Leaving the menu cancels pending scans but keeps applied
  cheats running.
- Widget memory is one 64KB heap page taken on the first open and kept (never
  freed — freeing while vsh still references our widgets hard-locks lv2).

## Online sync

A companion GitHub repo (`game-cheats`) holds the cheats. Modes are set in
`settings.txt` (see below):

- **offline** — never touches the network.
- **fetch** — downloads this game's cheats in the background at launch, and
  Triangle re-downloads on demand.
- **contribute** — fetch, plus Start/Select send an anonymous vote (a pull
  request opened via a shipped token) recording that a cheat worked or failed on
  your build. A *working* vote also carries the live pre-write value so the
  server can judge which builds a cheat really fits.

HTTPS reuses the XMB's own cellHttp/cellSsl (no init, no extra memory).

## Cheat file format

One file per title at
`/dev_hdd0/tmp/simple-cheat-menu/cheats/<TITLEID>.txt` (named after the id
`game_plugin` reports, e.g. `BCES01742.txt`). Lines starting with `#` are
comments. Each cheat is a `name:` line followed by its code lines:

```
# God of War: Ascension
name: Infinite Health
score BCES01742 01.00: 92 11+ 1-
w32 004A1B2C 42C80000

name: Infinite Rage
aob 3C60000138630010 3C60000060000000
```

- `name:` — the display name (kept short; no game-name prefix, so it fits the row).
- `score <titleId> <version>: <confidence> <worked>+ <failed>-` — optional crowd
  evidence for one build. Only the line matching the running build is shown.
- Code lines: `w8`/`w16`/`w32 <addr> <val>` writes, or `aob <find> <repl>`
  byte-pattern patches. A `working-val=<hex>:<n>,...` suffix carries crowd
  evidence used to colour the score badge.

The plugin ignores any legacy `author:`/`version:`/`mode:` fields. Source cheats
live human-editable in `game-cheats/source/`; a host-side `compile.py` merges
votes and emits the compiled per-title files the console downloads.

## Build & install

Add `simple-cheat-menu.vcxproj` to the solution and build (Release|PS3). The
signed `.sprx` lands in `out/`. Install like the other plugins: copy to
`/dev_hdd0/plugins/simple-cheat-menu/` and add its path to
`/dev_hdd0/boot_plugins.txt`, then restart.

Cheat data lives under `/dev_hdd0/tmp/simple-cheat-menu/`: per-title files in
`cheats/`, and an optional `settings.txt` with `mode=` (`offline`, `fetch`, or
`contribute`; default `contribute`).

Logs use the `[cht]` tag (`/dev_hdd0/tmp/dbg.txt`, forwarded to the debug bridge).
