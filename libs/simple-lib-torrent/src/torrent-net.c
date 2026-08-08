#include "torrent-net.h"

static const TorrentNetwork *boundNetwork;

void bindTorrentNetwork(const TorrentNetwork *network)
{
   boundNetwork = network;
}

const TorrentNetwork *getTorrentNetwork(void)
{
   return boundNetwork;
}
