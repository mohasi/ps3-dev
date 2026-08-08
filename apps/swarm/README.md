# Swarm

A torrent client for the PS3 that runs its own VPN. The WireGuard tunnel is spoken by the app itself
rather than by the console, so the trackers and the peers see the VPN's address and never the
console's.

## Setting up the VPN

1. Get an ordinary WireGuard `.conf` from your provider. It is the same file their desktop client
   uses, with an `[Interface]` and a `[Peer]` section.
2. Copy it to `/dev_hdd0/tmp/swarm/wireguard.conf` over FTP. The name and the place are fixed, so
   there is nothing to point at it.
3. Launch Swarm once and read the VPN line at the bottom of the left panel.

The file holds a private key, so treat it as a password: anyone who reads it can use your VPN
account. No part of it is ever written to the log, and with `logs=normal` neither is the server it
connects to.

An endpoint given as a name rather than an address is not read yet, so the `Endpoint` line has to be
an IP.

### The three ways it can run

Set both in `settings.txt`.

| `vpn` | `killswitch` | What happens |
|---|---|---|
| `off` | ignored | Everything goes over the console's own connection. The sidebar reads OFF / NOT PROTECTED. |
| `on` | `off` | The tunnel is used when it is up. If it cannot start, the console's own connection is used instead, and the sidebar says so. |
| `on` | `on` | Nothing reaches the network at all unless the tunnel is up. The sidebar reads NET BLOCKED and searching is refused. |

The middle one is the forgiving setting and the last one is the safe one.

## Searching

A site to search is one text file in `/dev_hdd0/tmp/swarm/sources`. Copy a file in to add a site,
delete it to remove one, or move it into `sources/disabled` to keep it without using it. They are
read at launch.

Two are created for you, Torrents-CSV and The Pirate Bay, both of which answer a search with data
rather than a page. A site that cannot be searched is skipped rather than listed, since it could only
ever show the same few newest torrents. `sources/README.md` in this repo says how to write a file for
another site.

Press Triangle to search. Results appear in their own view, and X on a result queues it.

## Using it

Left and right move between the list of views and the rows; up and down move within whichever has the
highlight. Along the bottom: Square starts or stops the highlighted torrent, Circle deletes it, and
Triangle searches. A button that would do nothing to whatever is highlighted is left out.

Deleting asks first, with three answers: from the app only, from the app and the disk, or cancel.

The Logs view shows the last couple of hundred lines the app wrote, so a download that gets stuck can
be looked at without a PC.

## Settings

`/dev_hdd0/tmp/swarm/settings.txt` is created with documented defaults on first launch. Edit it over
FTP; changes apply the next time the app starts.

| Key | Default | What it does |
|---|---|---|
| `vpn` | `on` | Whether traffic goes through the tunnel. |
| `killswitch` | `on` | With the VPN on, whether anything may go out while the tunnel is down. |
| `securedelete` | `off` | Whether deleting content from the disk writes over it first. |
| `logs` | `normal` | `full` also logs what is being downloaded and searched for, and every peer and packet. |

That is the whole file.

### Secure delete

Ordinary deleting removes the file's name and leaves its contents on the disk until something else
happens to write over them, which is why recovery tools work. With `securedelete=on`, every byte is
written over first, so there is nothing left to recover.

It costs the time of writing the file again. Measured on the console's own disk: 32 MB took 1303 ms
against 2 ms for an ordinary delete, so about 25 MB/s. A 1.5 GB film is about a minute. That is why
it is off by default.

## Where files go

All of these live in `/dev_hdd0/tmp/swarm`, outside the game folder, because installing the app wipes
that.

- `settings.txt` and `wireguard.conf` as above.
- `sources/` one file per site, plus `sources/disabled/`.
- `downloads/` finished content. A torrent still downloading sits under `downloads/incomplete` in the
  shape it will have when done, and moves up only once every piece is in, so a folder of content is
  never half a download.
- `downloads/resume/` one small file per torrent in the list, holding the link it was added from, so
  the app can take it up again after a restart. The file goes when its torrent is deleted, by either
  answer, and deleting the downloads folder takes the lot. Its name comes from the link rather than
  from the torrent, so the folder listing itself names nothing.

Stopping part way costs nothing. The next run reads what is on disk, checks each piece against its
hash and asks only for the rest.

## What it writes to the log

Lines go to `/dev_hdd0/tmp/dbg.txt` and to the bridge client's Logs tab. By default that is the app
starting, which build it is, whether the VPN came up, what the three settings are, errors, and a line
when a download finishes. It never says what was downloaded, what was searched for, which trackers or
peers were talked to, or which VPN server was used.

Everything else is off unless `logs=full` is set. That is for working out why something is not
working, and it does name torrents and search terms, so it is not worth leaving on: `dbg.txt` sits in
a folder the FTP plugin serves to the local network.

## Build

```
dev\vmbuild.ps1 libs simple-lib-wireguard
dev\vmbuild.ps1 libs simple-lib-torrent
dev\vmbuild.ps1 apps swarm
```

Install with the ps3 MCP `deploy` tool, then launch it from the XMB Games column. A launch request is
ignored while the app is already running, and the bridge still reports success, so wait for a run to
finish before starting another.

## Credits

- The WireGuard protocol is Jason A. Donenfeld's. The implementation here is written from the
  protocol paper and the RFCs it builds on; no code was taken from it.
- BitTorrent follows the BEP documents: BEP 3 for peers, BEP 9 and BEP 10 for magnet links, BEP 15
  for UDP trackers, BEP 20 for the client name.
- TLS is [BearSSL](https://bearssl.org) by Thomas Pornin, vendored in `simple-lib-https`.
- The interface icons are Font Awesome glyphs, packed into one font by Fontello.
