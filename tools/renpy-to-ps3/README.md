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

Output: `tools\renpy-to-ps3\bin\renpy-to-ps3.exe`, a windowed app. Targets .NET Framework 4.0.

## Usage

Run `renpy-to-ps3.exe`, pick a task from the dropdown, fill in the paths and
press Run. Everything the tool reports appears in the log pane at the bottom.

Tasks:

| Task | What it does |
|---|---|
| Convert a game to a PS3 bundle | Compile + transcode + bundle into one `.rpk` |
| Check if a game is convertible | Report which Ren'Py constructs the game uses |
| Compile scripts to bytecode | Compile scripts; optionally write a `.rbc` file |
| List / extract an archive | Look inside or unpack an `.rpa` archive |
| Inspect a bundle | Show an `.rpk`'s contents and sizes |
| Script diagnostics | Show as text / raw tree / animation nodes for one `.rpyc` |

Convert options: max image size (longest edge, default 1920 px), plain
quotes/dots (replace curly quotes / ellipsis for fonts that lack them), and
no cache / clear cache (asset cache control). The bundled `ffmpeg.exe` is
used automatically.

Converting also writes a `<out.rpk>.log` alongside the bundle with the full
run transcript. Repeated conversions reuse cached encoded assets (keyed by
content + settings) so only changed files are re-encoded.

## How it works

1. **Read** — `.rpa` archives and `.rpyc` compiled scripts (Ren'Py stores the
   latter as Python pickles, which the tool parses without Python).
2. **Classify** — walks the scripts and sorts every construct into
   supported / partial / deferred / unsupported buckets, then gives a verdict on
   whether the game converts cleanly.
3. **Compile** — lowers the linear story (say / menu / jump / call /
   if / python / show / scene / ...) into a small instruction set, then writes a
   verified `.rbc` bytecode. Reports anything it couldn't lower.
4. **Transcode + bundle** — resizes and re-encodes images, audio and
   video through ffmpeg and writes the bytecode plus all assets into one `.rpk`.

The "show as text" / "raw tree" / "animation nodes" tasks are diagnostics for
inspecting a single `.rpyc`.

## Status

Working end to end for linear-core games: read, classify, compile to verified
bytecode, transcode assets, and produce a playable `.rpk`. Screen-language GUI
and ATL transforms/animation are partially covered — the check and compile
tasks flag whatever a given game needs that isn't fully supported yet.

## Credits

Bundles [FFmpeg](https://ffmpeg.org) (`ffmpeg.exe`) for the asset transcoding.
