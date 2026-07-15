# renpy-to-ps3

Converts a Ren'Py visual-novel game into a single bundle the PS3 can play.
Ren'Py is a PC engine for visual novels; the PS3 can't run it directly, so this
tool reads the game's archives and compiled scripts, turns the script into a
compact bytecode, transcodes the images/audio/video to formats the console
decodes, and packs it all into one `.rpk` file. The on-console `renpy-player`
app loads that `.rpk`.

`ffmpeg.exe` is bundled (used for the asset transcoding) — no separate install.

## Build

Open `ps3-dev.sln` in Visual Studio and build, or use MSBuild:

```powershell
msbuild tools\renpy-to-ps3\renpy-to-ps3.csproj /p:Configuration=Release
```

Output: `tools\renpy-to-ps3.exe`. Targets .NET Framework 4.0.

## Usage

```
renpy-to-ps3 <command> [args...]

Commands:
  list <rpa-file>                         List archive contents
  extract <rpa-file> <output>             Extract an .rpa archive to a folder
  info <game-dir>                         Report which Ren'Py constructs the game
                                          uses and whether it is convertible
  compile <rpyc|game-dir> [out.rbc]       Compile scripts to intermediate form;
                                          write bytecode if out.rbc is given
  pack <game-dir> <out.rpk> [options]     Full convert: compile + transcode +
                                          bundle into one .rpk
  rpk <file>                              Inspect an .rpk bundle (contents + sizes)
```

`pack` options:

| Option | Meaning |
|---|---|
| `--max <px>` | cap the longest image edge (default 1920) so assets fit the target |
| `--ascii-text` | replace curly quotes / ellipsis with plain ASCII (system-font fallback) |
| `--ffmpeg <path>` | use a specific ffmpeg instead of the bundled one |
| `--no-cache` | re-encode every asset, ignoring the cache |
| `--clear-cache` | wipe the asset cache first, then pack and repopulate it |

`pack` also writes a `<out.rpk>.log` alongside the bundle with the full run
transcript. Repeated packs reuse cached encoded assets (keyed by content +
settings) so only changed files are re-encoded.

## How it works

1. **Read** — `.rpa` archives and `.rpyc` compiled scripts (Ren'Py stores the
   latter as Python pickles, which the tool parses without Python).
2. **Classify** (`info`) — walks the scripts and sorts every construct into
   supported / partial / deferred / unsupported buckets, then gives a verdict on
   whether the game converts cleanly.
3. **Compile** (`compile`) — lowers the linear story (say / menu / jump / call /
   if / python / show / scene / ...) into a small instruction set, then writes a
   verified `.rbc` bytecode. Reports anything it couldn't lower.
4. **Transcode + bundle** (`pack`) — resizes and re-encodes images, audio and
   video through ffmpeg and writes the bytecode plus all assets into one `.rpk`.

`ast`, `atldump` and `script` are extra diagnostic commands for inspecting a
single `.rpyc` (raw tree, ATL animation nodes, reconstructed script text).

## Status

Working end to end for linear-core games: read, classify, compile to verified
bytecode, transcode assets, and produce a playable `.rpk`. Screen-language GUI
and ATL transforms/animation are partially covered — `info` and `compile` flag
whatever a given game needs that isn't fully supported yet.
