#pragma once

// Lend simple-lib-torrent the tunnel, so trackers and peers are reached the same way everything else
// is. Call it once the tunnel is up.
void useTunnelForTorrents(void);
