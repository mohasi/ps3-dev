#pragma once

#include "torrent-source.h"   // SourceFile, the shape of a site the app ships

// swarm's own folder on the hard disk, holding settings.txt and the WireGuard config file.
// Kept out of the game folder because installing the app wipes and recreates that.

#define SWARM_DIR "/dev_hdd0/tmp/swarm"

// creates the folder and settings.txt on first launch, then reads it.
void loadSwarmSettings(void);

// full path of the WireGuard config file, which always sits beside settings.txt.
const char *getWgConfigPath(void);

// 1 when swarm should run its traffic through the tunnel. on unless settings.txt says off.
int isVpnEnabled(void);

// With it on, nothing goes out at all unless the tunnel is up. With it off, the console's own
// connection is used whenever the tunnel is not there.
int isKillSwitchOn(void);

// 1 when content deleted from the disk should be written over first, so it cannot be read back.
int isSecureDeleteOn(void);

// 1 when the log should also carry what is being downloaded and searched for, and every peer and
// packet. Off by default: an ordinary log says nothing about its owner.
int isDetailedLogWanted(void);

// 1 when results are shown exactly as the site sent them.
int isAdultAllowed(void);

#define SOURCE_MAX 8   // sites held at once

const char *getSourcesPath(void);     // the folder holding a file per site to search
const char *getDownloadsPath(void);   // where finished content goes

// the sites the folder is created with, as their files read
const SourceFile *getShippedSources(int *count);
