# Patch Studio

A Windows tool for making PS3 texture patches. You dump a game's textures on the console, edit them on your PC, and send a patch back — the [simple-cheat-menu](../../plugins/simple-cheat-menu) plugin applies it in-game. Nothing is injected into the game; textures are swapped live in video memory and matched by content, so a patch keeps working across reboots and reloads.

## The workflow

1. **Dump on the console.** In a game with simple-cheat-menu running, open the menu, switch to the **Patches** tab (L1/R1), and press **Square** to dump the textures currently on screen. Play and dump as you go to build up the game's art.
2. **Fetch Dump.** Pick the game from the list; its dump is pulled into a project. If no project is open, one is created for that game automatically.
3. **Edit.** Double-click a texture to open it in your image editor (or the Windows viewer if none is set). Change the PNG, keep the same size, and save — it gets an *edited* badge.
4. **Build.** Writes a single `.patch` file containing only the textures you changed.
5. **Deploy.** Sends the patch to the console (building first if you've changed anything since).
6. **Apply on the console.** Back in the Patches tab, select the patch and press **✕**.

Only edited textures go into a patch, so the game keeps everything else exactly as it shipped.

## Files

- **`.patchproj`** — your project: the dumped textures plus your edits. Save it to keep working later.
- **`.patch`** — the built patch you deploy or share. It's what the console applies.

## Settings

On first run a `settings.txt` is created next to the exe with two keys:

- **`ps3ip`** — the console's IP address, for Fetch Dump and Deploy over FTP. The console needs an FTP server running (e.g. simple-ftp or webMAN).
- **`imageEditor`** — full path to the editor opened on double-click. Leave empty to use the Windows default for PNG.

## Building

Part of the `dev/ps3-dev.sln` solution (under **tools**), or build `patch-studio.csproj` directly with MSBuild. It's a .NET Framework 4.8 WPF app and runs on Windows only — it does not need the PS3 SDK.
