# yo-player

A native PlayStation 3 YouTube client. Watch YouTube on the console the way the official app should have:
fast, no PSN sign-in, no ads.

## Why

The official PS3 YouTube app is sluggish, forces a PSN account, and is full of ads. yo-player talks to YouTube
directly and only pulls the video and audio, so there are no ads and no account gate — and it stays snappy.

It reuses what this repo already has:
- **`simple-lib-av`** — H.264/AAC decode and playback, the only codecs the PS3 decodes in hardware.
- **`simple-lib-app`** — the UI framework (screens, widgets, fonts) shared with `file-manager`.
- **`simple-lib-tls`** — a modern TLS stack so the app can reach YouTube's servers, which the console's own
  networking can no longer handshake with.

## Features

- **Browse & search** — a home screen with Subscriptions, Gaming, Sports, Podcasts and Watch Later feeds, plus
  text search (on-screen keyboard) and per-channel video lists. Search has Relevance / Views sort orders;
  channel views have Latest / Popular / Oldest. Pasting a YouTube link into the search box plays it straight away.
- **Playback up to 1080p with sound** — streamed as it plays (nothing is downloaded in full first), H.264 + AAC.
  Picks 1080p at 30 frames per second, or 720p when the video is 60 fps (the console can't keep up with 1080p60).
  Pause and seek with a scrub bar showing current / total time.
- **Live streams** — plays anything with a LIVE badge (sports, news), a few seconds behind the live edge.
- **Resume** — videos pick up from where you last stopped; already-watched videos are dimmed in the grid.
- **Subscriptions** — subscribe / unsubscribe from inside a channel; the Subscriptions feed gathers the latest
  uploads from every channel you follow, newest first.
- **Watch Later** — its own home category; add or remove any video from the pad, tagged on the thumbnail.
- **Download** — combine a video's video + audio into one `.mp4` file, in the background, with a queue and a
  progress readout. Finished downloads are registered with the XMB and show up in its Video column.
- **Volume** — adjust in the player with a pill meter; the level is remembered across videos.
- **Description** — read the full video description over the playing video, scrollable, without pausing.
- **Chapters** — jump around by chapter. Chapters come from the description's timestamps, falling back to
  YouTube's own chapter list when the description has none. Chapter boundaries are notched onto the seek bar.
- **Subtitles** — cycle through the video's available subtitle languages (off by default); the chosen language
  shows in the player, and lines are drawn above the seek bar.
- **SponsorBlock** — community-marked sponsor / intro / self-promo segments are skipped automatically and shown
  on the seek bar in their category colours. How much gets skipped is a setting (see below).
- **No PSN, no ads** — talks to YouTube directly; appears under the XMB **Video** column like the old app.

## How it works (high level)

1. On launch it brings up the network, the modern TLS transport, graphics, audio, fonts and the pad, then loads
   the home screen on the **Subscriptions** feed.
2. Feeds, searches and channel pages are fetched from YouTube on demand and shown as a grid of thumbnails. Only
   the tiles currently on screen hold a thumbnail image, so a feed can hold up to 500 videos at a flat memory
   cost, and each category is cached for the session so switching back to it is instant.
3. Playing a video pulls the picture and sound streams and decodes them in hardware through `simple-lib-av`,
   streaming as it goes rather than waiting for a full download. The picture and sound streams open in parallel
   so the video starts roughly twice as fast.
4. Downloads run on a background worker: it reads the same picture and sound streams and remuxes them (repackages
   them, no re-encoding) into a single `.mp4` file on disk, while you keep browsing.

## Screens & controls

The app boots into the **home** screen on the **Subscriptions** feed. Searching or opening a channel pushes the
**search** screen on top; playing pushes the player. **Circle** always backs out one level. Button hints along
the bottom use the console's own controller glyphs.

### Home / search / channel lists

- **D-pad** — move the highlight. Left / right wrap onto the previous / next row, so you can run through every
  tile with just left and right.
- **X** — play the highlighted video.
- **Square** — add the highlighted video to **Watch Later**, or remove it if it's already there. Videos in the
  list show a "Watch Later" tag on their thumbnail.
- **R3** (right stick click) — **download** the highlighted video (see Downloads below). Live streams can't be
  downloaded.
- **Triangle** — open the highlighted video's **channel** (that channel's videos only). Inside a channel,
  Triangle instead **subscribes / unsubscribes** from that channel.
- **Start** — open the keyboard to search. Typing a YouTube link plays it directly.
- **L1 / R1** (home only) — switch category: Subscriptions, Gaming, Sports, Podcasts, Watch Later.
- **Select** — cycle the sort order (shown top-right): search = Relevance / Views, channel = Latest / Popular /
  Oldest.
- **Circle** — back out (channel → search results → home).
- **PS button** — exit.

### In the player

- **Up / Down** — volume (a pill meter shows briefly, remembered across videos).
- **Left / Right** — seek back / forward. A seek bar with current / total time plus the video title shows when
  playback starts and while scrubbing, then auto-hides.
- **X** — pause / resume (restarts the video if it had ended).
- **Select** — show the video **description** over the video (playback carries on); scroll with up / down, close
  with Select or Circle.
- **Triangle** — cycle **subtitles** (off by default) through the video's available languages; the current
  language shows while the seek bar is up.
- **Start** — open the **chapter** list; up / down select, X jumps there, Start or Circle closes.
- **Circle** — back to the list.

Community **SponsorBlock** segments are skipped automatically during playback and marked on the seek bar in
their category colours. Live streams have no seek bar (there's nothing to scrub).

## Downloads

Pressing **R3** on a video queues a download. The picture and sound streams are combined into one `.mp4` file.
Downloads run in the background so you can keep browsing, queue up if you start several, and show progress in
the top-right corner. Live streams can't be downloaded.

A finished download is handed to the system with `cellVideoExport`, which moves it out of the app's folder and
registers it in the console's media database. That is what makes it appear in the XMB's Video column, named
after the video, next to anything you would normally copy over from a USB stick. Copying a file into
`/dev_hdd0/video` by hand does not work: the XMB lists videos from that database, not from the folder.

Two constraints came out of getting that working, and both shape the code. The export only accepts files from
the directory the game content utility hands back, so downloads are written straight into the app's own
`USRDIR`. And it rejects filenames containing spaces or brackets, so the file is staged under the video id;
the name you actually see comes from the title passed to the export.

## Settings

User settings live in `/dev_hdd0/tmp/yo-player/settings.txt`, created with defaults on first launch. It's a
plain `key=value` text file you can edit over FTP; changes apply on the next launch, and a bad value quietly
falls back to its default.

- **`sponsorblock-mode`** — how much SponsorBlock skips:
  - `off` — never skip anything (segments aren't even fetched).
  - `ads` (default) — skip paid promotions only: sponsors, self-promo, like / subscribe reminders.
  - `all` — also skip intros, outros, filler and non-music sections.
- **`theme`** — which colour theme to start in, naming one of the blocks in `themes.txt` (see below).
  Defaults to `youtube`.

## Themes

Every colour the app draws comes from one theme, and the shipped one is YouTube's own dark palette:
`#0F0F0F` background, `#212121` surfaces, `#AAAAAA` secondary text, YouTube red for the seek bar, LIVE
badges and the volume meter, and a white ring around the selected thumbnail. Everything is drawn flat
from plain rectangles and text — there are no sprite sheets.

`/dev_hdd0/tmp/yo-player/themes.txt` is written on first launch with the full YouTube block, so every key
is there to edit over FTP. Change a colour in that block to restyle the app, or add a `[Name]` block of
your own and point `settings.txt` at it with `theme=<name>` (lower case, spaces become hyphens). A new
block inherits YouTube, so it only has to list what it changes. Colours are `#RRGGBB`, or `#RRGGBBAA` for
transparency. Changes apply on the next launch; delete the file to get the shipped theme back.

## Storage

Everything is plain files under `/dev_hdd0/tmp/yo-player/` — no account binding, so copy the folder to back it up
or move it to another console:

- `settings.txt` — user settings (above).
- `subscriptions.txt` — subscribed channel ids, one per line (seeded with a few defaults on first run; editable
  by hand). Subscriptions are also toggled with Triangle inside a channel.
- `watchlater.txt` — the Watch Later queue. It keeps full video details so the list opens instantly without
  re-fetching.
- watch history and last-position files — used to dim already-watched tiles and to resume playback.

## Build & deploy

Built as an NPDRM `.pkg`. All builds go through the **ps3 MCP tool** (or the build VM directly, which is faster):

1. `mcp__ps3__list` — confirm the app name.
2. `mcp__ps3__build` with kind `apps`, name `yo-player` — returns a job id.
3. `mcp__ps3__poll` the job id until it finishes; exit code 0 means success.

Install the resulting `.pkg` with any PS3 package installer. It appears on the XMB under **Video**. Note: after
editing any shared library it links against, do a full **Rebuild** of the app, since an incremental build can
keep the old library code.

`TITLE_ID` is `YOPLAYER1`.

## Known limitations

- **Live streams** use a different delivery than normal videos (a rolling sequence of segments rather than one
  file), so they take a little longer to start and to exit, and can't be downloaded or seeked. Otherwise they
  play the same, a few seconds behind the live edge.
- **1080p60** videos aren't played at 60 fps — the console's hardware decoder can't keep up, so those fall back
  to 720p.
- The Subscriptions feed is a merged list, so it can't page past the first batch it gathers from each channel.
