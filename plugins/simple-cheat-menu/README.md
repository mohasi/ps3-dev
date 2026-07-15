# simple-cheat-menu

An in-game cheat menu for the PS3. It draws a menu over any running game, lets
you turn cheats on and off while you play, and patches the game's memory live to
make them work. It can also download cheats for the game you're in and, if you
let it, let you vote on which cheats actually work — all from the console, no PC
needed.

## What it is

- A single VSH plugin (a small program that loads into the console's menu, the
  XMB). Nothing is added into the game itself.
- The menu is drawn with PAF — the XMB's own on-screen toolkit — so it paints on
  top of the game the same way the quit/power menu does.
- Cheats are simple text files, one per game, matched to the game by its title id
  (the code on the disc, like `BCES01742`).

## Using it

Press the **PS button** during a game. A short press opens the cheat menu (a dark
overlay with a panel in the middle); a long press (holding PS) is left alone so
the normal quit/power menu still works. If the game has no cheats, a short press
just shows a small notice and hands the controller straight back.

| Button | Action |
|---|---|
| **D-pad up/down** | move the selection (hold to auto-repeat) |
| **Cross** | turn the selected cheat on or off |
| **Triangle** | re-download this game's cheats (online modes only) |
| **Start** | mark the selected cheat as *working* (contribute mode; must be on) |
| **Select** | mark the selected cheat as *failed* (contribute mode) |
| **Circle** | back to the in-game XMB |
| **PS** | resume the game |

Each row shows the cheat name, an **AoB** tag if it patches by searching for a
byte pattern rather than a fixed address, and a crowd **score %** coloured by how
well it fits your exact game version: green = confirmed working here, amber =
unknown, red = only worked on a different version.

## How it works

- **One plugin does everything.** It draws the overlay through PAF and writes the
  game's memory from outside the game (cross-process) through cobra — a low-level
  console service. Nothing is injected into the game.
- **The PS button is the only trigger.** While you play, the game owns the
  controller and the plugin sees nothing; the moment a PS overlay opens, the
  console reports controller activity, and that is the plugin's cue. It reads the
  raw controller to tell a short press from a long one. While the menu is up, the
  plugin takes the controller over (the XMB's normal controller reads are blinded)
  and reads it directly. It only ever reads the controller directly while the menu
  is up — doing so across a controller-ownership change freezes the console, so it
  always hands the controller back before any switch.
- **Four workers, no locks.** A **menu** worker reads the controller and records
  what you asked for; a **frame** hook owns every on-screen piece and repaints on
  the XMB's own drawing thread; a **worker** does the slow search-and-patch off to
  the side so the menu never stalls; short-lived **download/vote** workers do the
  network calls. They coordinate through simple shared flags, so nothing ever waits
  on a lock.
- **Toggling a cheat is safe by construction.** Turning one on first saves the
  original bytes, writes the new value, then reads it back to confirm it landed —
  a row only shows ON if that check passed, otherwise it undoes the write and stays
  OFF. AoB cheats search the game's real code and data areas for the pattern first,
  patch every match, and undo by writing the original pattern back. Leaving the
  menu cancels any search still running but leaves cheats you turned on running.
- **Memory stays tiny.** All the on-screen pieces and the cheat data live in one
  64KB block taken the first time you open the menu and then kept and reused for
  every game. Nothing is used until you first open the menu.

## Online cheat sync

The cheats come from a companion GitHub repository (`mohasi/game-cheats`). How far
the plugin reaches out is set by `mode=` in `settings.txt`:

- **offline** — never touches the network; only uses cheat files already on the
  console.
- **fetch** — downloads the current game's cheats in the background shortly after
  it launches, and Triangle re-downloads on demand.
- **contribute** — everything fetch does, plus Start/Select send an anonymous vote
  recording that a cheat worked or failed on your version. A *working* vote also
  carries the value that was in memory just before the patch, so the repo can work
  out which game versions a cheat really fits. Votes go up as a GitHub pull request
  (using a shared token built into the plugin) that a repo workflow checks and
  merges. The console remembers votes it already sent so it never sends the same
  one twice.

The default is **contribute**. Downloads and votes reuse the XMB's own secure-web
plumbing, so they cost no extra setup or memory.

## A note on risk

Cheats change a running game's memory, and they come from the community, so they
aren't guaranteed correct for your exact game version. Most are harmless — a wrong
one usually just does nothing, or crashes the game back to the XMB.

The exception is cheats that patch the game's **code** rather than a simple value
(many **AoB** cheats do). If a code patch is wrong for your version it can, in rare
cases, freeze the whole console and need a hard restart — because the game shares
the graphics chip with the XMB, so a game that hangs the graphics takes the XMB
down with it. The plugin can't tell in advance whether a code patch is safe; it
only confirms the patch was written correctly.

So the decision is yours, and the **score badge** is your guide: green means the
community confirmed the cheat on your exact version, amber means it's unproven (try
it at your discretion), red means it only worked on a different version. Mark
cheats **working** or **failed** so that signal gets better for everyone.

## Cheat file format

One file per game at
`/dev_hdd0/tmp/simple-cheat-menu/cheats/<TITLEID>.txt` (named after the id the XMB
reports, e.g. `BCES01742.txt`). Lines starting with `#` are comments. Each cheat is
a `name:` line followed by its code lines:

```
# God of War: Ascension
name: Infinite Health
score BCES01742 01.00: 92 11+ 1-
w32 004A1B2C 42C80000

name: Infinite Rage
aob 3C60000138630010 3C60000060000000
```

- `name:` — the display name (kept short, no game-name prefix, so it fits the row).
- `score <titleId> <version>: <confidence> ...` — optional crowd evidence for one
  game version. Only the line matching the running version is used; its first
  number is the percentage shown on the badge.
- Code lines:
  - `w8` / `w16` / `w32 <addr> <val>` — write 1, 2, or 4 bytes at a fixed address.
  - `aob <find> <repl>` — find the `<find>` byte pattern in the game and write
    `<repl>` over every match.
  - A trailing `working-val=<hex>:<n>,...` on a write line carries crowd evidence:
    the values seen in memory on versions where the cheat worked. The plugin
    compares those against your live memory to colour the score badge.

Source cheats are kept human-editable in the repo's `source/` folder; a host-side
script merges the votes and produces the compiled per-game files the console
downloads.

## Build & deploy

Build and deploy through the **ps3 MCP tool** (see the workspace `CLAUDE.md`):

1. `build` with kind `plugins`, name `simple-cheat-menu` — produces the signed
   plugin. Poll the job until it finishes with exit code 0.
2. `deploy` with name `simple-cheat-menu` and `restartXmb=true` to install it and
   load it straight away.

Cheat data lives under `/dev_hdd0/tmp/simple-cheat-menu/`: per-game files in
`cheats/`, and `settings.txt` with `mode=` (`offline`, `fetch`, or `contribute`).
The plugin creates that folder and a default `settings.txt` (mode `contribute`) on
first run, so there's a file to edit.

## Constraints & notes

- **A bad cheat must never lock the console.** The engine best-effort makes a cheat
  work, but a wrong or mis-ordered cheat may at worst do nothing or crash the game
  back to the XMB — it must never hard-lock the machine. Verified writes, undo on
  failure, and treating every downloaded cheat as untrusted input all serve that.
- **Memory is scarce in the XMB.** A VSH plugin has very little room, so everything
  lives in one 64KB block, taken only when first needed and never freed while the
  console runs.
- Logs use the `[cht]` tag in `/dev_hdd0/tmp/dbg.txt`, forwarded to the debug
  bridge.
</content>
</invoke>
