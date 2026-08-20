# simple-cd-info

Puts the album, artist and track names back on the XMB when you insert an audio CD. The PS3 used to fetch these from an online music database, but that service was shut down years ago, so on current consoles the CD screen just shows "Track 1, Track 2, ...". This plugin brings the feature back, automatically, with nothing to configure.

## What you get

Insert an audio CD, open it in the Music column, and the real track listing appears the same way it did when the console was new. It works for any disc that the gnudb database knows about, which covers the vast majority of commercial CDs.

## How it works

The original lookup talked to a server at `dmr.allmusic.com`. That server is dead, and the hostname no longer resolves, so the console's request fails before it goes anywhere. The plugin steps in at five points:

1. Redirect. When the console tries to look up `dmr.allmusic.com`, the plugin quietly points that one name at the console itself (loopback) instead. Every other name lookup on the system is left alone. This is what removes the need for the old manual proxy setting.

2. Move the port. The console would connect on port 80, which webMAN also uses, so only one of the two could run. The port is a fixed number inside the firmware's own lookup code, so the plugin rewrites it to 8790 and listens there. Port 80 is left alone.

3. Answer. The plugin runs a tiny web server on the console. The redirected request lands there instead of on the dead server.

4. Identify the disc. When a request comes in, the plugin reads the CD's table of contents straight from the drive (track start positions and total length) and turns that into a disc fingerprint, the same fingerprint the gnudb database is indexed by.

5. Look it up and reply. It asks gnudb for that disc, gets back the album and track names, and packs them into exactly the binary format the console's own display code expects. The console parses that reply and shows the names. If the disc is not found, or anything else fails, it replies with "no information", which the console handles cleanly.

## What had to be reverse-engineered

There is no documentation for any of this, and no server left to watch, so the format the console expects had to be recovered from the console's own firmware. The firmware modules were decrypted and disassembled to work out:

- The exact byte layout the display code reads back. It is a nested structure of length-prefixed records with no field names, so every field's meaning and position had to be found by reading the parser. Getting it slightly wrong was not harmless: a structurally valid but semantically wrong reply could lock the console, so the format was verified against the real firmware parser (run under emulation on a PC) before ever serving it to hardware.

- How the disc fingerprint is computed, and that the request itself does not contain the disc's track layout, which is why the plugin reads the table of contents from the drive directly.

- Where the port number lives. It is not part of the hostname or the address, it is a constant in the lookup module's code, stored into the request just before connecting. There are two copies, one per server name the module knows. The plugin finds them by the instruction pattern rather than a fixed position, so a firmware whose code sits elsewhere cannot end up with the wrong word rewritten.

- Where the console resolves the dead hostname, so the redirect could hook that one spot without disturbing anything else. The hostname lives in read-only memory, so the redirect writes to it through the same privileged path the plugin uses to install its hooks.

## Credits

- Track and album data comes from [gnudb](https://gnudb.org/), the community successor to the original CDDB database.
- The method for reading the disc's table of contents from the drive follows [webMAN-MOD](https://github.com/aldostools/webMAN-MOD).

## Notes for developers

The reply format has no field names, only numbered slots, and most slots are never displayed. What each one does was established on a real console by putting a different marker word in every candidate slot and reading them back off the TV. Everything known is below; "nothing" means a marker was placed there and never appeared on any screen.

### The album record — 30 slots

| Slot | What we send | What it does |
|---|---|---|
| 0 | *empty* | Chooses which artist the track screens use. Never displayed itself. See below. |
| 1 | album title | **Album** |
| 2, 3, 5, 8, 12, 19, 20, 22, 26, 27 | *empty* | Nothing. Tested with markers. |
| 4 | album artist | **Artist**, on the disc's icon |
| 6 | number: track count | Nothing. Sent 3 instead and no screen changed. |
| 7 | number: 1 | Unknown, never varied |
| 9 | genre from the record | **Genre**, on the track screens only. The disc icon's own Genre row comes from somewhere else we never found. |
| 10, 13, 14, 16, 17, 18 | *nothing at all* | **Hard-locks the console if given text.** See the warning below. |
| 11 | number: 1 | **Disc Number**, second half |
| 15 | the track group | the tracks. See below. |
| 21, 24, 25 | *nothing at all* | Never tested. Same kind as the locking slots, so treat as dangerous. |
| 23 | number: 1 | Unknown, never varied |
| 28 | number: 0 | Structural |
| 29 | number: 7 | Selects the track view. Must be above 6. |

### Slot 15 — the track group

Slot 15 wraps a record of three things: the list of track records, then two numbers. The first number is **Disc Number**'s first half, so it is sent as 1. The second shows nothing. Neither number sizes the track list, which carries its own count — sending the track count in the first is what used to make the console report a 30-track CD as "disc 30 of 30".

### A track record — 17 fields, one per track

| Field | What we send | What it does |
|---|---|---|
| 0, 2, 3, 4, 5, 7–14 | *nothing at all* | Never tested |
| 1 | track title | **Track**, on both the list and the properties screen |
| 6 | a group of 4 strings | position 1 = title (shows nothing, but it is what it has always held), **position 2 = that track's own artist**, positions 3 and 4 empty and harmless |
| 15, 16 | number: 0 | Required. Without them the console rejects the whole disc. |

There are two artists, at two levels, and album slot 0 chooses between them. Slot 0's own contents are never displayed. Fill it and the track screens show the album-wide artist from slot 4; leave it empty and they show each track's own artist instead, which is what compilations need. This plugin leaves it empty. Get this wrong and the symptom is confusing rather than obvious: the original bug wrote the track title into every position of slot 6, so the Artist row faithfully displayed the track name.

A slot sent as an empty string leaves whatever was already on screen untouched, so a blank slot looks like an old value lingering rather than a slot that was never filled. When testing, send a value that has never been used before; otherwise you cannot tell a fresh reply from a stale screen.

Do not put text in album slots 10, 13, 14, 16, 17, 18, 21, 24 or 25. Writing text into them hard-locks the console and needs a power cycle. Two separate attempts using different slots from that group both locked, so treat all nine as off limits. The reason is in `docs/ps3-firmware-re.md`: the console walks these replies with a decoder that follows offsets into nested records, and a wrong value sends it round that loop forever. Leaving a slot completely empty is handled cleanly, so "absent" is safe and "present but wrong" is not.

Two things are still missing from the display: the release year, which the lookup does return and nothing uses, and the Genre shown on the disc's own icon. Neither is in any slot we can safely reach. The more promising place to look is not these slots at all: a reply is a series of records, each with a one-letter tag, and the valid tags are `T A D G N P C O E`. This plugin only ever sends one `A` record. Those two missing values may well be whole records we never send rather than slots we never filled. `docs/ps3-firmware-re.md` also describes a much safer way to experiment than rebuilding and restarting the XMB each time, which is itself a documented lock-up risk.

The screen is not a reliable indicator of which build is running. The plugin logs the size of every reply it sends, so comparing that number against the change you just made is the dependable way to confirm the console is running your build.

The plugin stays resident and does not tear itself down, the same as the other XMB plugins here. The gethostbyname hook and the listener are simply dropped along with the rest of the XMB on a full power-off. Because the hook is a patch into the running XMB, always redeploy with a restart-xmb: that reloads the XMB from scratch, which clears the old patch and lets the fresh plugin reinstall it. Deploying without a restart-xmb would leave the previous hook pointing at stale memory.
