# yo-player

A native PlayStation 3 YouTube client. Watch YouTube on the console the way the official app should have: fast, no PSN sign-in, no ads.

## Why

The official PS3 YouTube app is sluggish, forces a PSN account, and is full of ads. yo-player talks to YouTube directly and only pulls the video and audio, so there are no ads and no account gate — and it stays snappy.

It reuses what this repo already has:
- **`simple-lib-av`** — H.264/AAC decode, the only codecs the PS3 decodes in hardware.
- **`simple-lib-app`** — the UI framework (screens, widgets, fonts) shared with `file-manager`.

## Features

- **Browse & search** — Subscriptions, Gaming, Sports, Podcasts and Watch Later feeds, plus text search
  (on-screen keyboard) and per-channel video lists. Sort orders for search (Relevance / Views) and channels
  (Latest / Popular / Oldest).
- **Playback up to 1080p with sound** — streamed as it plays (nothing downloaded in full), H.264 + AAC, the only
  codecs the PS3 decodes in hardware. Pause and seek with a scrub bar showing current / total time.
- **Live streams** — plays anything with a LIVE badge (sports, news), a few seconds behind the live edge.
- **Resume** — videos pick up from your last watched spot; watched videos are dimmed in the grid.
- **Subscriptions** — subscribe / unsubscribe from inside a channel; the Subscriptions feed aggregates their
  latest uploads.
- **Watch Later** — its own category; add or remove any video from the pad, tagged on the thumbnail.
- **Download** — combine a video's 1080p video + audio into one `.mkv` on the console, in the background, with a
  queue and a progress readout. Live streams excluded.
- **SponsorBlock** — community-marked sponsor / intro / self-promo segments are skipped automatically and shown
  on the seek bar in their category colours.
- **No PSN, no ads** — talks to YouTube directly; appears under the XMB **Video** column like the old app.

## Status

Works on real hardware. Everything above runs on the console. Storage is plain files under
`/dev_hdd0/tmp/yo-player/` (history, subscriptions, Watch Later); downloads go to `/dev_hdd0/VIDEOS`.


## Install

Built as an NPDRM `.pkg`. Install it with any PS3 package installer; it appears under **Video**. `TITLE_ID` is `YOPLAYER1`.

## Screens & controls

The app boots into the **home** screen on the **Subscriptions** feed. Searching or opening a channel pushes the
**search** screen on top; playing pushes the player. **Circle** always backs out one level. Button hints along
the bottom use the console's own controller glyphs.

- **D-pad** — move the highlight. Left / right wrap onto the previous / next row, so you can run through every
  tile with just left and right. **X** — play the selected result.
- **Square** — add the highlighted video to **Watch Later**, or remove it if it's already there. Videos in the
  list show a "Watch Later" tag on their thumbnail.
- **R3** (right stick click) — **download** the highlighted video: its video and audio are combined into one
  `.mkv` under `/dev_hdd0/VIDEOS`, named after the video. Downloads run in the background
  (keep browsing), queue up if you start several, and show progress in the top-right corner. Live streams
  can't be downloaded.
- **Triangle** — open the highlighted video's **channel** (that channel's videos only). Inside a channel,
  Triangle instead **subscribes / unsubscribes** from that channel.
- **In the player**: **Up / Down** — volume (a pill meter shows briefly, remembered across videos).
  **Left / Right** — seek back/forward (a seek bar with current/total time plus the video
  title shows when playback starts and while scrubbing, then auto-hides); **X** — pause / resume (restarts the
  video if it had ended). Community
  **SponsorBlock** segments (sponsors, intros, self-promo, …) are skipped automatically and marked on the
  seek bar in their category colours. Live streams have no seek bar (there's nothing to scrub).
  **Select** — pause and show the video **description** over the video; scroll it with up/down, close with
  Select or Circle (playback resumes unless it was already paused before opening).
- **Start** — open the keyboard to search.
- **L1 / R1** — (home) switch category (Subscriptions, Gaming, Sports, Podcasts, Watch Later).
- **Select** — cycle the sort order (shown top-right): search = Relevance/Views, channel = Latest/Popular/Oldest.
- **Circle** — back out (channel → search results → home; player → list).
- **PS button** — exit.

**Subscriptions** aggregates the latest videos from your subscribed channels. Subscribe or unsubscribe with
Triangle from inside a channel; the set is stored in `/dev_hdd0/tmp/yo-player/subscriptions.txt` (seeded with a
few defaults on first run, one channel id per line — you can still edit it by hand).

**Watch Later** is its own category, backed by `/dev_hdd0/tmp/yo-player/watchlater.txt`. It keeps the full
video details so the list opens instantly without re-fetching.

**Live** streams use a different delivery than normal videos (a rolling segment sequence rather than one file),
so they take a little longer to start and to exit, and can't be downloaded or seeked. Otherwise they play the
same, a few seconds behind the live edge.

History, subscriptions, and Watch Later live as plain files under `/dev_hdd0/tmp/yo-player/`
(copy the folder to back it up). Already-watched videos are dimmed in the grid. The grid is windowed — only
on-screen tiles hold a thumbnail — so feeds and channels scroll far (up to 500 loaded) at a flat memory cost,
and categories are cached per session for instant switching.
