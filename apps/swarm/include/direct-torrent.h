#pragma once

// The console's own connection as the network simple-lib-torrent talks through, for when the vpn is
// off or is down and the kill switch allows it. Traffic leaves under the console's real address.

void useConsoleForTorrents(void);
