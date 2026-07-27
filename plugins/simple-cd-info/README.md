# simple-cd-info

Puts the album, artist and track names back on the XMB when you insert an audio CD. The PS3 used to fetch these from an online music database, but that service was shut down years ago, so on current consoles the CD screen just shows "Track 1, Track 2, ...". This plugin brings the feature back, automatically, with nothing to configure.

## What you get

Insert an audio CD, open it in the Music column, and the real track listing appears the same way it did when the console was new. It works for any disc that the gnudb database knows about, which covers the vast majority of commercial CDs.

## How it works

The original lookup talked to a server at `dmr.allmusic.com`. That server is dead, and the hostname no longer resolves, so the console's request fails before it goes anywhere. The plugin steps in at four points:

1. Redirect. When the console tries to look up `dmr.allmusic.com`, the plugin quietly points that one name at the console itself (loopback) instead. Every other name lookup on the system is left alone. This is what removes the need for the old manual proxy setting.

2. Answer. The plugin runs a tiny web server on the console. The redirected request lands there instead of on the dead server.

3. Identify the disc. When a request comes in, the plugin reads the CD's table of contents straight from the drive (track start positions and total length) and turns that into a disc fingerprint, the same fingerprint the gnudb database is indexed by.

4. Look it up and reply. It asks gnudb for that disc, gets back the album and track names, and packs them into exactly the binary format the console's own display code expects. The console parses that reply and shows the names. If the disc is not found, or anything else fails, it replies with "no information", which the console handles cleanly.

## What had to be reverse-engineered

There is no documentation for any of this, and no server left to watch, so the format the console expects had to be recovered from the console's own firmware. The firmware modules were decrypted and disassembled to work out:

- The exact byte layout the display code reads back. It is a nested structure of length-prefixed records with no field names, so every field's meaning and position had to be found by reading the parser. Getting it slightly wrong was not harmless: a structurally valid but semantically wrong reply could lock the console, so the format was verified against the real firmware parser (run under emulation on a PC) before ever serving it to hardware.

- How the disc fingerprint is computed, and that the request itself does not contain the disc's track layout, which is why the plugin reads the table of contents from the drive directly.

- Where the console resolves the dead hostname, so the redirect could hook that one spot without disturbing anything else. The hostname lives in read-only memory, so the redirect writes to it through the same privileged path the plugin uses to install its hooks.

## Credits

- Track and album data comes from [gnudb](https://gnudb.org/), the community successor to the original CDDB database.
- The method for reading the disc's table of contents from the drive follows [webMAN-MOD](https://github.com/aldostools/webMAN-MOD).

## Notes for developers

The plugin stays resident and does not tear itself down, the same as the other XMB plugins here. The gethostbyname hook and the listener are simply dropped along with the rest of the XMB on a full power-off. Because the hook is a patch into the running XMB, always redeploy with a restart-xmb: that reloads the XMB from scratch, which clears the old patch and lets the fresh plugin reinstall it. Deploying without a restart-xmb would leave the previous hook pointing at stale memory.
