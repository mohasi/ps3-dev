# yo-player

A native PlayStation 3 YouTube client. Watch YouTube on the console the way the official app should have: fast, no PSN sign-in, no ads.

## Why

The official PS3 YouTube app is sluggish, forces a PSN account, and is full of ads. yo-player talks to YouTube directly and only pulls the video and audio, so there are no ads and no account gate — and it stays snappy.

It reuses what this repo already has:
- **`simple-lib-av`** — H.264/AAC decode, the only codecs the PS3 decodes in hardware.
- **`simple-lib-app`** — the UI framework (screens, widgets, fonts) shared with `file-manager`.

## Status

Works on real hardware. Browse feeds, search, open a channel, and play in up to 1080p with sound — nothing is downloaded in full, video streams as it plays. It picks up where you left off: videos resume from your last watched spot. The app appears under the XMB **Video** column, matching the old YouTube app.

## Planned

- Add subscriptions from the UI (right now they live in a text file — see below).
- Download a video.
- Watch later / favourites.

## Install

Built as an NPDRM `.pkg`. Install it with any PS3 package installer; it appears under **Video**. `TITLE_ID` is `YOPLAYER1`.

## Screens & controls

The app boots into the **home** screen on the **Subscriptions** feed. Searching or opening a channel pushes the
**search** screen on top; playing pushes the player. **Circle** always backs out one level. Button hints along
the bottom use the console's own controller glyphs.

- **D-pad** — move the highlight; **X** — play the selected result.
- **In the player**: **Left / Right** — seek back/forward (a seek bar with current/total time shows while
  scrubbing and auto-hides); **X** — pause / resume (restarts the video if it had ended). Community
  **SponsorBlock** segments (sponsors, intros, self-promo, …) are skipped automatically and marked on the
  seek bar in their category colours.
- **Start** — open the keyboard to search.
- **Triangle** — open the highlighted video's **channel** (that channel's videos only).
- **L1 / R1** — (home) switch category (Subscriptions, Gaming, Live, Sports, Podcasts).
- **Select** — cycle the sort order (shown top-right): search = Relevance/Views, channel = Latest/Popular/Oldest.
- **Circle** — back out (channel → search results → home; player → list).
- **PS button** — exit.

**Subscriptions** aggregates the latest videos from the channels listed in
`/dev_hdd0/tmp/yo-player/subscriptions.txt` (one channel id per line, seeded with a few defaults on first run;
edit it to change them — a proper subscribe UI comes later).

History lives as plain text under `/dev_hdd0/tmp/yo-player/` (copy the folder to back it up): already-watched
videos are dimmed in the grid. The grid is windowed — only on-screen tiles hold a thumbnail — so feeds and
channels scroll far (up to 500 loaded) at a flat memory cost, and categories are cached per session for instant
switching.
