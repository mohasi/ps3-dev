#include "torrent-selftest.h"

#include "bencode.h"
#include "dbg.h"
#include "magnet.h"
#include "torrent-feed.h"
#include "sha1.h"
#include "string-utilities.h"
#include "torrent-file.h"

#define TAG "[bt] "

// RFC 3174 section 7.3, the first two test cases, plus the million-a case from the same document
static const char *sha1Messages[] = { "abc", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq" };
static const uint8_t sha1Expected[2][SHA1_LENGTH] = {
   { 0xA9, 0x99, 0x3E, 0x36, 0x47, 0x06, 0x81, 0x6A, 0xBA, 0x3E, 0x25, 0x71, 0x78, 0x50, 0xC2, 0x6C, 0x9C, 0xD0,
     0xD8, 0x9D },
   { 0x84, 0x98, 0x3E, 0x44, 0x1C, 0x3B, 0xD2, 0x6E, 0xBA, 0xAE, 0x4A, 0xA1, 0xF9, 0x51, 0x29, 0xE5, 0xE5, 0x46,
     0x70, 0xF1 }
};
static const uint8_t millionAsExpected[SHA1_LENGTH] = {
   0x34, 0xAA, 0x97, 0x3C, 0xD4, 0xC4, 0xDA, 0xA4, 0xF6, 0x1E, 0xEB, 0x2B, 0xDB, 0xAD, 0x27, 0x31, 0x65, 0x34,
   0x01, 0x6F
};

// a torrent's shape in miniature: the pieces a real one has, small enough to read
static const char sampleTorrent[] =
   "d8:announce30:udp://tracker.example.org:13374:infod6:lengthi1048576e4:name9:thing.iso"
   "12:piece lengthi262144e6:pieces20:0123456789abcdefghijee";

static int checkBytes(const char *name, const uint8_t *produced, const uint8_t *expected, int length)
{
   for (int index = 0; index < length; index++) {
      if (produced[index] == expected[index]) continue;

      logError(TAG "%s FAILED at byte %d, got 0x%02x expected 0x%02x\n", name, index, produced[index],
               expected[index]);
      return 1;
   }

   logTrace(TAG "%s ok\n", name);
   return 0;
}

static int checkSha1(void)
{
   int failures = 0;
   uint8_t hash[SHA1_LENGTH];

   for (int index = 0; index < 2; index++) {
      hashSha1(hash, sha1Messages[index], getStrLen(sha1Messages[index]));
      failures += checkBytes(index == 0 ? "sha1 abc" : "sha1 two block", hash, sha1Expected[index], SHA1_LENGTH);
   }

   // a million 'a's, fed a thousand at a time: this is what proves the running state, since a
   // torrent is hashed piece by piece rather than in one call
   Sha1 sha1;
   char thousandAs[1000];
   memSet(thousandAs, 'a', sizeof thousandAs);

   initSha1(&sha1);
   for (int round = 0; round < 1000; round++) updateSha1(&sha1, thousandAs, sizeof thousandAs);
   finishSha1(&sha1, hash);
   failures += checkBytes("sha1 a million times", hash, millionAsExpected, SHA1_LENGTH);

   return failures;
}

static int checkBencode(void)
{
   const uint8_t *document = (const uint8_t *)sampleTorrent;
   int length = (int)(sizeof sampleTorrent - 1);
   const char *wrongCase = 0;

   BencodeValue root;
   if (readBencode(document, length, 0, &root) != length || root.kind != BENCODE_DICTIONARY)
      wrongCase = "the whole document";

   BencodeValue announce, info, name, pieceLength, pieces, missing;
   if (!wrongCase && findBencodeMember(document, length, &root, "announce", &announce) != 0)
      wrongCase = "finding announce";
   if (!wrongCase && findBencodeMember(document, length, &root, "info", &info) != 0) wrongCase = "finding info";
   if (!wrongCase && findBencodeMember(document, length, &root, "nothing", &missing) == 0)
      wrongCase = "a member that is not there";

   char text[64];
   if (!wrongCase && copyBencodeString(document, &announce, text, sizeof text) != 0) wrongCase = "reading announce";
   if (!wrongCase && !strEq(text, "udp://tracker.example.org:1337")) wrongCase = "the announce address";

   if (!wrongCase && findBencodeMember(document, length, &info, "name", &name) != 0) wrongCase = "finding the name";
   if (!wrongCase && copyBencodeString(document, &name, text, sizeof text) != 0) wrongCase = "reading the name";
   if (!wrongCase && !strEq(text, "thing.iso")) wrongCase = "the name";

   if (!wrongCase && findBencodeMember(document, length, &info, "piece length", &pieceLength) != 0)
      wrongCase = "finding the piece length";
   if (!wrongCase && getBencodeInteger(document, &pieceLength) != 262144) wrongCase = "the piece length";

   // the pieces string holds raw bytes and its length is what says where it ends, not a terminator
   const uint8_t *piecesText = 0;
   if (!wrongCase && findBencodeMember(document, length, &info, "pieces", &pieces) != 0)
      wrongCase = "finding the pieces";
   if (!wrongCase && getBencodeString(document, &pieces, &piecesText) != 20) wrongCase = "the pieces length";

   // the info hash is the hash of the info dictionary exactly as it arrived
   if (!wrongCase && (info.start <= 0 || info.end <= info.start)) wrongCase = "the bounds of info";
   if (!wrongCase && document[info.start] != 'd') wrongCase = "where info starts";
   if (!wrongCase && document[info.end - 1] != 'e') wrongCase = "where info ends";

   if (wrongCase) {
      logError(TAG "bencode FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "bencode ok\n");
   return 0;
}

// a malformed document must be refused rather than read past
static int checkBencodeRefusals(void)
{
   static const char *bad[] = {
      "i42",            // no end
      "ie",             // no digits
      "5:abc",          // shorter than it claims
      "d3:key",         // a key with no value
      "l",              // never closed
      "x",              // not a kind at all
      "-1:a",           // a length that is not a number
      "llllllllllllllllllllle"   // nested past what we allow
   };

   for (int index = 0; index < (int)(sizeof bad / sizeof bad[0]); index++) {
      BencodeValue value;
      if (readBencode((const uint8_t *)bad[index], getStrLen(bad[index]), 0, &value) >= 0) {
         logError(TAG "bencode refusal FAILED, \"%s\" was accepted\n", bad[index]);
         return 1;
      }
   }

   logTrace(TAG "bencode refusals ok\n");
   return 0;
}

// the same sample read as a torrent rather than as bencode, so the facts a transfer needs are checked
static int checkTorrentFile(void)
{
   const uint8_t *document = (const uint8_t *)sampleTorrent;
   int length = (int)(sizeof sampleTorrent - 1);

   static TorrentMeta meta;   // its file list makes it far too large for the stack
   const char *wrongCase = 0;

   if (readTorrentFile(document, length, &meta) != 0) wrongCase = "reading it at all";
   if (!wrongCase && !strEq(meta.name, "thing.iso")) wrongCase = "the name";
   if (!wrongCase && (meta.trackerCount != 1 || !strEq(meta.trackers[0], "udp://tracker.example.org:1337")))
      wrongCase = "the tracker";
   if (!wrongCase && (meta.totalLength != 1048576 || meta.fileCount != 1)) wrongCase = "the size";
   if (!wrongCase && (meta.pieceLength != 262144 || meta.pieceCount != 1)) wrongCase = "the pieces";

   // the info hash has to be taken over the info dictionary alone, not the whole document
   uint8_t expected[SHA1_LENGTH];
   int infoStart = findBytes(sampleTorrent, length, "d6:length", 9);
   if (!wrongCase && infoStart < 0) wrongCase = "finding info in the sample";
   if (!wrongCase) {
      hashSha1(expected, document + infoStart, length - infoStart - 1);   // less the document's own 'e'
      if (checkBytes("torrent info hash", meta.infoHash, expected, SHA1_LENGTH) != 0) return 1;
   }

   if (wrongCase) {
      logError(TAG "torrent file FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "torrent file ok\n");
   return 0;
}

// a magnet link says the hash, and usually a name and some trackers, and nothing else
static int checkMagnetLink(void)
{
   static const char *hexLink = "magnet:?xt=urn:btih:a77baccadceb644bed72cdbfb7cca22a08b493f7"
                                "&dn=some%20thing&tr=udp%3A%2F%2Ftracker.example.org%3A1337%2Fannounce";
   static const char *base32Link = "magnet:?xt=urn:btih:U552ZSW45NSEX3LSZW73PTFCFIELJE7X";
   static const uint8_t expected[SHA1_LENGTH] = {
      0xA7, 0x7B, 0xAC, 0xCA, 0xDC, 0xEB, 0x64, 0x4B, 0xED, 0x72, 0xCD, 0xBF, 0xB7, 0xCC, 0xA2, 0x2A, 0x08, 0xB4,
      0x93, 0xF7
   };

   MagnetLink magnet;
   const char *wrongCase = 0;

   if (readMagnetLink(&magnet, hexLink) != 0) wrongCase = "reading one written in hex";
   if (!wrongCase && checkBytes("magnet hash", magnet.infoHash, expected, SHA1_LENGTH) != 0) return 1;
   if (!wrongCase && !strEq(magnet.name, "some thing")) wrongCase = "the name, which is escaped in the link";
   if (!wrongCase && (magnet.trackerCount != 1 ||
                      !strEq(magnet.trackers[0], "udp://tracker.example.org:1337/announce")))
      wrongCase = "the tracker, which is escaped as well";

   // the same hash written the other way round, which some sites still use
   if (!wrongCase && readMagnetLink(&magnet, base32Link) != 0) wrongCase = "reading one written in base32";
   if (!wrongCase && checkBytes("magnet base32 hash", magnet.infoHash, expected, SHA1_LENGTH) != 0) return 1;

   if (!wrongCase && readMagnetLink(&magnet, "magnet:?dn=no+hash+here") == 0) wrongCase = "refusing one with no hash";

   if (wrongCase) {
      logError(TAG "magnet FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "magnet ok\n");
   return 0;
}

// a peer sends the description on its own, without the file around it
static int checkTorrentInfo(void)
{
   const uint8_t *document = (const uint8_t *)sampleTorrent;
   int length = (int)(sizeof sampleTorrent - 1);

   int infoStart = findBytes(sampleTorrent, length, "d6:length", 9);
   if (infoStart < 0) {
      logError(TAG "torrent description FAILED, the sample has no info dictionary\n");
      return 1;
   }

   static TorrentMeta meta;   // its file list makes it far too large for the stack
   if (readTorrentInfo(document + infoStart, length - infoStart - 1, &meta) != 0 || !strEq(meta.name, "thing.iso") ||
       meta.pieceCount != 1 || meta.totalLength != 1048576) {
      logError(TAG "torrent description FAILED\n");
      return 1;
   }

   logTrace(TAG "torrent description ok\n");
   return 0;
}

// the two shapes a search site's answer comes in: an array inside an object, and one on its own
static int checkJsonResults(void)
{
   static const char *inObject =
      "{\"torrents\":[{\"name\":\"first thing\",\"size_bytes\":1048576,\"seeders\":4,\"leechers\":2,"
      "\"infohash\":\"a77baccadceb644bed72cdbfb7cca22a08b493f7\"},"
      "{\"name\":\"second &quot;thing&quot; by Rapha&euml;l\",\"size_bytes\":2097152,\"seeders\":0,\"leechers\":1,"
      "\"infohash\":\"0123456789abcdef0123456789abcdef01234567\"}]}";

   static const char *bareArray =
      "[{\"id\":\"1\",\"name\":\"only thing\",\"info_hash\":\"0123456789ABCDEF0123456789ABCDEF01234567\","
      "\"size\":\"3145728\",\"seeders\":\"7\",\"leechers\":\"3\"}]";

   TorrentSource source;
   TorrentItem items[4];
   const char *wrongCase = 0;

   memSet(&source, 0, sizeof source);
   source.format = SOURCE_FORMAT_JSON;
   strCopy(source.fields.list, SOURCE_FIELD_MAX, "torrents");
   strCopy(source.fields.title, SOURCE_FIELD_MAX, "name");
   strCopy(source.fields.hash, SOURCE_FIELD_MAX, "infohash");
   strCopy(source.fields.size, SOURCE_FIELD_MAX, "size_bytes");
   strCopy(source.fields.seeds, SOURCE_FIELD_MAX, "seeders");
   strCopy(source.fields.peers, SOURCE_FIELD_MAX, "leechers");

   int count = parseTorrentJson(&source, inObject, getStrLen(inObject), items, 4);
   if (count != 2) wrongCase = "an array inside an object";
   if (!wrongCase && !strEq(items[0].title, "first thing")) wrongCase = "the first title";
   if (!wrongCase && !strEq(items[0].size, "1.00 MB")) wrongCase = "the size, counted in bytes";
   if (!wrongCase && (items[0].seeders != 4 || items[0].leechers != 2)) wrongCase = "the seeders and leechers";
   if (!wrongCase && !items[0].hasInfoHash) wrongCase = "the hash";

   // sites escape their titles for the web and leave them that way in json
   if (!wrongCase && !strEq(items[1].title, "second \"thing\" by Rapha\xC3\xABl")) wrongCase = "an escaped title";

   // the other shape: no list name, values written as strings, hash in capitals
   strCopy(source.fields.list, SOURCE_FIELD_MAX, "");
   strCopy(source.fields.hash, SOURCE_FIELD_MAX, "info_hash");
   strCopy(source.fields.size, SOURCE_FIELD_MAX, "size");

   count = parseTorrentJson(&source, bareArray, getStrLen(bareArray), items, 4);
   if (!wrongCase && count != 1) wrongCase = "an array on its own";
   if (!wrongCase && (!strEq(items[0].title, "only thing") || items[0].seeders != 7)) wrongCase = "values as text";
   if (!wrongCase && !strEq(items[0].size, "3.00 MB")) wrongCase = "a size written as text";

   if (wrongCase) {
      logError(TAG "json FAILED on %s\n", wrongCase);
      return 1;
   }

   logTrace(TAG "json ok\n");
   return 0;
}

int runTorrentSelfTest(void)
{
   int failures = checkSha1() + checkBencode() + checkBencodeRefusals() + checkTorrentFile() + checkTorrentInfo() +
                  checkMagnetLink() + checkJsonResults();

   if (failures == 0) logInfo(TAG "self test passed\n");
   else logError(TAG "self test FAILED, %d case(s)\n", failures);
   return failures;
}
