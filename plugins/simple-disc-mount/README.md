# simple-disc-mount

A small disc-mount plugin for PlayStation 3 CFW, packaged as a VSH-injected PRX. It adds a "Mount Disc Image" submenu to the XMB Games column, listing every `.iso` in `/dev_hdd0/PS3ISO`. Selecting one mounts it as a virtual PS3 game disc via Cobra, so the game boots as if the disc were in the drive.

## Why this exists

webMAN-MOD does this and a hundred other things. I wanted something that does one job — mount a game ISO from the XMB — without the rest. No FTP server, no temperature monitor, no fan control, no web interface, no background scanning. Just a menu with my ISOs in it, and a single press to mount one.

## What it handles

Only PS3 game ISO images (`.iso` files) placed in `/dev_hdd0/PS3ISO`. Each is mounted through Cobra as an emulated PS3 Blu-ray disc (Cobra syscall 8, the "switch active PS3 disc" op). This is not a general disc-image tool — it does not mount DVD/BD video, PS1/PS2 images, or other formats, and it does not read any particular disc filesystem itself; Cobra owns the actual mount, the plugin just points it at the file.

## What it does

On boot the plugin waits for the XMB to be ready, then:

1. Auto-mounts the last ISO, but only if the disc drive is empty — a real disc always wins, and clears the memory (the remembered path lives in `/dev_hdd0/tmp/sdm_last.txt`)
2. Starts a small web listener on `127.0.0.1:8947` (loopback only — the PC never sees it), and a watcher that unmounts the image as soon as a real disc goes into the drive
3. Mounts `/dev_blind` (a writable mirror of `/dev_flash`, the console's internal system storage)
4. Generates `sdm.xml` — a menu file listing every `.iso` in `/dev_hdd0/PS3ISO`, sorted alphabetically
5. Patches `category_game.xml` once to inject the "Mount Disc Image" submenu just below "Package Manager" (with a one-time backup of the original)

When you press X on an ISO in the menu, the XMB wakes Sony's built-in web renderer with a link pointing at that local listener. The listener catches the request, tells Cobra to swap the disc (fake eject, mount, fake insert), remembers the path for auto-mount on the next boot, and shows a notification.

## Real discs take priority

A mounted image otherwise stays in the way of the drive: Cobra keeps reporting the image to the XMB until something unmounts it, so a disc you push in is simply ignored. The watcher checks every two seconds and unmounts the image once a real disc is present, leaving the drive to the disc. It waits for any running game to exit first, so a game booted from the image is never pulled out from under itself.

A real disc also clears the remembered image, so ejecting the disc later does not bring the image back — mounting one again is always an explicit pick from the menu. Note that the XMB's own Eject only reaches the physical drive, so it cannot clear a mounted image: inserting a real disc is what does that.

## Installation

Copy `simple-disc-mount.sprx` into `/dev_hdd0/plugins/` on the console, then add a line to `/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/simple-disc-mount.sprx
```

Reboot. The "Mount Disc Image" submenu appears in the Games column after the first boot.

## Adding ISOs

Place `.iso` files in `/dev_hdd0/PS3ISO/`. The menu regenerates on every boot, so newly added ISOs show up after a reboot.

## Web browser note

The plugin uses the PS3's built-in web renderer as an entry point — it's the only way to trigger custom code from an XMB menu item. The browser opens for a moment when you select an ISO, then closes itself. By default the PS3 shows a "Do you want to close?" confirmation. To skip it:

1. Open the PS3 web browser manually
2. Go to Settings (Triangle → Tools)
3. Set "Confirm Browser Close" to Off (or the equivalent)

After that, selecting an ISO flashes the browser briefly and closes without prompting.

## Building and deploying

Build and deploy through the ps3 MCP tool (`build` kind `plugins`, name `simple-disc-mount`; then `deploy`). The PS3's LAN address is 10.0.0.2 for FTP transfers.

## Debug log

All activity is logged via `dbg.h` (`logInfo` / `logWarn` / `logError`) with `[sdm]` tags. Each line is written to `/dev_hdd0/tmp/dbg.txt` and, if `simple-debug-bridge` is installed, forwarded live to the `debug-bridge-client` Logs tab on the PC. The plugin registers with the bridge from `_start()`, so startup chatter (XMB-wait, auto-mount, XML patching, listener bring-up) is buffered locally and sent as soon as the bridge link comes up.

## Requirements

- Cobra CFW (EVILNAT or equivalent) — needed for the disc-mount syscalls
- No other plugin using `127.0.0.1:8947` — if that port is taken, the listener can't start
