// settings - swarm's settings.txt, created with documented defaults on first launch.
// Format and parsing are the shared simple-lib-core settings-file.h standard.

#include "settings.h"

#include "dbg.h"
#include "settings-file.h"
#include "string-utilities.h"
#include "torrent-source.h"   // SourceFile, the shape of a site the app ships
#include "vfs.h"

#define SETTINGS_PATH   SWARM_DIR "/settings.txt"
#define SOURCES_PATH    SWARM_DIR "/sources"
#define DOWNLOADS_PATH  SWARM_DIR "/downloads"
#define WG_CONFIG_PATH  SWARM_DIR "/wireguard.conf"

static const char *DEFAULT_SETTINGS =
   "# swarm settings - edit over FTP; changes apply on the next launch\n"
   "\n"
   "vpn=on\n"
   "# on  - all of swarm's traffic goes through the WireGuard tunnel.\n"
   "# off - swarm uses the console's ordinary network connection.\n"
   "\n"
   "# the tunnel is set up by the file wireguard.conf in this same folder. it is the ordinary\n"
   "# .conf a VPN provider hands out. it holds a private key, so treat it as a password: anyone\n"
   "# who reads it can use your VPN account.\n"
   "\n"
   "killswitch=on\n"
   "# on  - with the vpn on, nothing reaches the network at all while the tunnel is down.\n"
   "# off - the console's own connection is used whenever the tunnel is not up.\n"
   "\n"
   "securedelete=off\n"
   "# on  - deleting content from the disk writes over every byte of it first, so it cannot be\n"
   "#       read back. this takes as long as writing the files did in the first place.\n"
   "# off - the files are simply removed, which is immediate.\n"
   "\n"
   "logs=normal\n"
   "# normal - the log in /dev_hdd0/tmp/dbg.txt says that swarm started, whether the vpn came up,\n"
   "#          and anything that went wrong. it never says what was downloaded or searched for.\n"
   "# full   - it also names those, and every peer and packet. that is for working out why\n"
   "#          something is not working, and is not worth leaving on.\n";

// The two sites swarm is created with. Both answer with data rather than a page and give a hash
// rather than a file, so each result becomes a magnet built with the trackers named in its file.
static const char TORRENTS_CSV_SOURCE[] =
   "# Torrents-CSV, an index that answers with data rather than a page.\n"
   "#\n"
   "# a site is one file like this one. copy another in to add it, delete this to remove it, or move\n"
   "# it into the disabled folder beside it to keep it without using it. they are read at launch.\n"
   "# search must be there, with {query} where the words go: a site that cannot be searched is\n"
   "# skipped. see the app's README for what every line means.\n"
   "\n"
   "name    = Torrents-CSV\n"
   "search  = https://torrents-csv.com/service/search?size=40&q={query}\n"
   "format  = json\n"
   "list    = torrents\n"
   "title   = name\n"
   "hash    = infohash\n"
   "size    = size_bytes\n"
   "seeds   = seeders\n"
   "peers   = leechers\n"
   "tracker = udp://tracker.opentrackr.org:1337/announce\n"
   "tracker = udp://open.stealth.si:80/announce\n";

static const char PIRATE_BAY_SOURCE[] =
   "# The Pirate Bay, through the same address its own site reads.\n"
   "\n"
   "name    = The Pirate Bay\n"
   "search  = https://apibay.org/q.php?q={query}\n"
   "format  = json\n"
   "title   = name\n"
   "hash    = info_hash\n"
   "size    = size\n"
   "seeds   = seeders\n"
   "peers   = leechers\n"
   "category = category\n"
   "adultcategories = 500-599\n"
   "tracker = udp://tracker.opentrackr.org:1337/announce\n"
   "tracker = udp://open.stealth.si:80/announce\n"
   "tracker = udp://exodus.desync.com:6969/announce\n";

static const SourceFile SHIPPED_SOURCES[] = { { "torrents-csv.txt", TORRENTS_CSV_SOURCE },
                                              { "piratebay.txt", PIRATE_BAY_SOURCE } };

static int adultAllowed;
static int vpnEnabled = 1;
static int killSwitchOn = 1;
static int secureDeleteOn;
static int detailedLog;

void loadSwarmSettings(void)
{
   makeDirPath(SWARM_DIR);   // first launch: the folder has to exist before the file can be created

   char text[1024];
   if (loadSettingsFile(SETTINGS_PATH, DEFAULT_SETTINGS, text, sizeof text) == 1)
      logInfo("[swarm] created %s with defaults\n", SETTINGS_PATH);

   // vpn: anything other than "off" leaves it on, so a typo fails safe rather than silently
   // sending traffic outside the tunnel
   const char *vpnValue = findSettingValue(text, "vpn");
   vpnEnabled = !(vpnValue && settingValueEquals(vpnValue, "off"));

   // the kill switch reads the same way: only the word "off" turns it off
   const char *killValue = findSettingValue(text, "killswitch");
   killSwitchOn = !(killValue && settingValueEquals(killValue, "off"));

   // deleting for good is the slower answer, so it is only done when it is asked for by name
   const char *secureValue = findSettingValue(text, "securedelete");
   secureDeleteOn = secureValue && settingValueEquals(secureValue, "on");

   // the same reading: only the word "full" turns the detail on, so a typo keeps the quiet log
   const char *logValue = findSettingValue(text, "logs");
   detailedLog = logValue && settingValueEquals(logValue, "full");

   const char *adultValue = findSettingValue(text, "adult");
   adultAllowed = adultValue && settingValueEquals(adultValue, "on");

   logInfo("[swarm] settings: vpn %s, killswitch %s, secure delete %s, logs %s\n", vpnEnabled ? "on" : "off",
           killSwitchOn ? "on" : "off", secureDeleteOn ? "on" : "off", detailedLog ? "full" : "normal");
}

const char *getWgConfigPath(void)
{
   return WG_CONFIG_PATH;
}

const char *getSourcesPath(void)
{
   return SOURCES_PATH;
}

const SourceFile *getShippedSources(int *count)
{
   *count = (int)(sizeof SHIPPED_SOURCES / sizeof SHIPPED_SOURCES[0]);
   return SHIPPED_SOURCES;
}

const char *getDownloadsPath(void)
{
   return DOWNLOADS_PATH;
}

int isVpnEnabled(void)
{
   return vpnEnabled;
}

int isKillSwitchOn(void)
{
   return killSwitchOn;
}

int isSecureDeleteOn(void)
{
   return secureDeleteOn;
}

int isDetailedLogWanted(void)
{
   return detailedLog;
}

int isAdultAllowed(void)
{
   return adultAllowed;
}
