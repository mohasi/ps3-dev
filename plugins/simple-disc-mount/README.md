# simple-disc-mount

A minimal disc mount plugin for PlayStation 3 CFW, packaged as a VSH-injected PRX. It adds a "Mount Disc Image" submenu to the XMB Games column, listing every `.iso` in `/dev_hdd0/PS3ISO`. Selecting one mounts it as a virtual Blu-ray disc via Cobra.

## Why this exists

webMAN-MOD does this and a hundred other things. I wanted something that does one job — mount an ISO from the XMB — without the rest. No FTP server, no temperature monitor, no fan control, no web interface, no background scanning. Just a menu with my ISOs in it, and a single press to mount one.

## What it does

On boot the plugin waits for the XMB to be ready, then:

1. Auto-mounts the last ISO if one was previously mounted (persisted in `/dev_hdd0/tmp/sdm_last.txt`)
2. Mounts `/dev_blind` (writable `/dev_flash` mirror)
3. Generates `sdm.xml` — an XMBML file listing every `.iso` in `/dev_hdd0/PS3ISO`, sorted alphabetically
4. Patches `category_game.xml` to inject the "Mount Disc Image" submenu below "Package Manager" (one-time, with backup)
5. Starts an HTTP listener on `127.0.0.1:8947`

When you press X on an ISO in the menu, the XMB wakes Sony's `webrender_plugin` with a URL pointing at the local listener. The listener receives the request, mounts the disc via Cobra syscalls (eject → mount → insert), saves the path for auto-mount on next boot, and shows a notification.

## Installation

Copy `simple-disc-mount.sprx` into `/dev_hdd0/plugins/` on the console, then add a line to `/dev_hdd0/boot_plugins.txt`:

```
/dev_hdd0/plugins/simple-disc-mount.sprx
```

Reboot. The "Mount Disc Image" submenu will appear in the Games column after the first boot.

## Web browser note

The plugin uses the PS3's built-in web renderer as an entry point — it's the only mechanism available to trigger custom code from an XMB menu item. The browser opens briefly when you select an ISO, then attempts to close itself. By default the browser will show a "Do you want to close?" confirmation dialog. To avoid this:

1. Open the PS3 web browser manually
2. Go to Settings (Triangle → Tools)
3. Set "Confirm Browser Close" to Off (or equivalent)

After that, selecting an ISO will flash the browser for a moment and close it automatically without prompting.

## Adding ISOs

Place `.iso` files in `/dev_hdd0/PS3ISO/`. The menu regenerates on every boot, so newly added ISOs will appear after a reboot.

## Debug log

All activity is logged to `/dev_hdd0/tmp/dbg.txt` with timestamps. If mounting doesn't work or the menu doesn't appear, check that file.

## Requirements

- Cobra CFW (EVILNAT or equivalent) — needed for the disc mount syscalls
- No port conflict on `127.0.0.1:8947` — if another plugin uses that port, the listener will fail to bind
