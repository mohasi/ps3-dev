# Adding a site to search

Swarm creates two sources on first launch, Torrents-CSV and The Pirate Bay. Anything else is added by
hand: write a file like the one below and copy it into `/dev_hdd0/tmp/swarm/sources` over FTP. Files
are read at launch. Delete one to remove that site, or move it into `sources/disabled` to keep it
without using it.

```
name    = What to call it
search  = https://example.com/rss?q={query}
torrent = https://example.com/download/{hash}.torrent
format  = rss
tracker = udp://tracker.example.org:1337/announce
```

- `name` what the results say they came from. The file's own name is used when this is missing.
- `search` the address to fetch, with `{query}` where the typed words go. A file without it is
  skipped: a site that cannot be given words to look for could only ever show the same few newest
  torrents, whatever was typed.
- `torrent` where the torrent file lives, with `{hash}` taken from the results. Leave it out when the
  results already link straight at the file.
- `format` how to read the reply. `rss` and `json` are what this build knows. Reading a site's own
  web pages is deliberately not supported: the markup changes and a text file cannot follow it.
- `tracker` may be repeated. A site that gives a hash and no file needs trackers named here, because
  a magnet built from a bare hash has nowhere to ask for peers. Use the trackers the site itself
  names.

Two kinds of site do not work. One that refuses anything but a browser answers 403 even on its feed
address. One that has no feed at all would need its pages read, which is the case above.

Jackett and Prowlarr would each cover many sites at once, and their answer is XML shaped like a feed,
so they are a small step once the Torznab format is read.
