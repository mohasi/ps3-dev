# simple-lib-torrent

The parts of a BitTorrent client that do not depend on how the traffic gets out. An app supplies the
network, which for Swarm means everything goes through the WireGuard tunnel.

Status: early. The pieces below work and are checked on the console at every launch. A source's feed
is read, a torrent file is fetched from it and taken apart, a tracker is asked who else holds it, and
its content is downloaded to disk from several peers at once.

## Several peers, several pieces

Peers are held open together and eight pieces are collected at once, each in 16 KB blocks, so one
slow peer holds up only its own blocks. A peer that goes quiet costs the blocks it was holding, which
go straight back on offer to another.

It matters most before any data moves. A peer that is gone takes fifteen seconds to find that out
about, and a swarm is full of them, so trying them one after another spent half a minute doing
nothing. Held together, those waits overlap and the first peer that answers starts serving in about a
second.

## How many peers

A peer serves only a few of the clients it is talking to, so how many will serve us is not something
that can be asked for. It is found: sixteen connections are opened to begin with, eight more every
ten seconds while fewer than eight of them are actually sending, up to a ceiling of forty-eight, and
the growth stops as soon as the connections stop opening (a tunnel has a limited number of streams).
A peer that has kept us choked for forty-five seconds is dropped so its slot can try another address.

Measured on the console, on one swarm with about 480 seeders: twenty-four connections gave two peers
sending and 350 KB/s, and the same swarm with this gave five sending and 1.5 MB/s.

## Where content goes

An app says which folder to download into. While a torrent is unfinished everything sits under
`incomplete/` in the same shape it will have when done, and is moved up only once every piece is in,
so a folder of content is never half a download. A torrent of one file writes that file directly; one
of several gets a folder named after the torrent.

A run that stops part way loses nothing. The next one reads each piece back off disk and checks it
against its hash, so it asks only for what is missing or damaged.

A torrent says where its files go, and it is not trusted with that: every part of a path is checked
before it becomes a folder name, so a torrent cannot write outside its own folder.

## Magnet links

A magnet link says the hash, usually a name and usually some trackers, and nothing else: no file
list, no piece hashes. Those are asked of the first peer that will serve them, over the extension
protocol (BEP 9), in 16 KB pieces like everything else. What arrives has to hash to the name the link
gave, or it is thrown away, because nothing in an unverified description can be trusted.

A link with no tracker in it cannot be used yet. Finding peers without one needs the distributed
hash table, which is not written.

## Nothing waits

An app adds torrents and then calls `serviceTorrentEngine` every time round its own loop. Each call
does a slice of work and returns, so a screen stays responsive and progress can be read at any
moment. The peer and tracker code underneath works the same way. One torrent downloads at a time and
the rest wait their turn, which is what keeps the memory to a single piece.

## The network

Nothing here opens a socket. An app binds a `TorrentNetwork` of its own calls, which for Swarm are
the WireGuard tunnel's, so a tracker is asked through the VPN like everything else. The clock comes
from there too: every timeout and speed is measured against it rather than counted in loop passes,
which was wrong by a factor of five when it was tried the other way. A tracker's UDP
exchange is matched by a random number and the sender is checked, since UDP by itself gives no way
to tell a reply to our question from a packet someone else sent.

## What is here

| File | What it does |
|---|---|
| `sha1.c` | SHA-1 (RFC 3174), which names every piece and identifies every torrent |
| `bencode.c` | the format a .torrent file and a tracker's reply are written in |
| `torrent-source.c` | the sites to search, read from a file the user edits |
| `torrent-feed.c` | fetching a source's RSS and turning it into a list of torrents |
| `torrent-file.c` | the .torrent itself: trackers, size, pieces, and the hash that names it |
| `magnet.c` | reading a magnet link, which most sites hand out instead of a file |
| `tracker.c` | asking a tracker over UDP for the addresses of other holders (BEP 15) |
| `peer.c` | the exchange with one of those holders: handshake, ask, check what arrives |
| `torrent-storage.c` | where pieces land on disk, and what is already there |
| `torrent-engine.c` | the downloads, serviced a slice at a time |
| `torrent-net.c` | the network calls the app lends the library |
| `torrent-selftest.c` | the published vectors, run on the console |

## Sources

No site is built into the code. A source is one small text file, and a folder of them is the whole
list: copy a file in to add a site, delete it to remove one, move it into the `disabled` folder beside
them to keep it without using it. An app creates the folders on first launch with one file in them,
Academic Torrents, because everything on it is a freely published dataset or paper and it makes a
demo that needs no explaining.

```
name    = Academic Torrents
browse  = https://academictorrents.com/rss.xml
torrent = https://academictorrents.com/download/{hash}.torrent
format  = rss
```

`search` is the same as `browse` with `{query}` where the words go, escaped for a URL. `torrent` says
where the file itself lives, with `{hash}` from the results, and is only needed when the results do
not link straight to it. `format` is how to read the reply: a source asking for one this build does
not know is skipped and says so, so a later format can be added without breaking anyone's file.
`name` falls back to the file's own name.

A feed varies in how it spells things. The tags this reads are `title`, `link`, `size` or
`nyaa:size`, `seeders`, `leechers`, and the info hash as `infohash`, `infoHash` or `nyaa:infoHash`.
Anything missing is simply absent from the list rather than fatal.

## Nothing is copied

A bencode value is an offset and a length into the document the caller already holds. That is not
about saving memory: the info hash has to be the SHA-1 of the exact bytes of the `info` dictionary
as they arrived, and re-encoding them would change the answer.

## Verification

`runTorrentSelfTest()` checks SHA-1 against RFC 3174's own test cases, including the million
character one fed a thousand bytes at a time, which is what proves the running state a torrent needs
when it hashes a piece across many reads. Bencode is checked both ways: a document like a real
torrent is read apart correctly, and eight malformed ones are each refused rather than read past.
The same sample is then read as a torrent, including its info hash, which has to come from the info
dictionary alone rather than from the whole document.

A fetched torrent is checked against the feed that led to it: the hash computed from the file has to
be the one the feed advertised, which is what says the whole chain arrived intact.
