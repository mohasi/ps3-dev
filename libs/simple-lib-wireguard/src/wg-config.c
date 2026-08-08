#include "wg-config.h"

#include "dbg.h"
#include "string-utilities.h"
#include "vfs.h"

#define TAG "[wg] "

#define CONFIG_MAX_LENGTH 4096
#define LINE_MAX_LENGTH   512

// base64 decoding, only what a key needs: the standard alphabet with padding.
static int decodeBase64Digit(char character)
{
   if (character >= 'A' && character <= 'Z') return character - 'A';
   if (character >= 'a' && character <= 'z') return character - 'a' + 26;
   if (character >= '0' && character <= '9') return character - '0' + 52;
   if (character == '+') return 62;
   if (character == '/') return 63;
   return -1;
}

static int decodeBase64(const char *text, uint8_t *out, int outCapacity)
{
   int written = 0;
   uint32_t accumulator = 0;
   int bitsHeld = 0;

   for (int index = 0; text[index]; index++) {
      char character = text[index];
      if (character == '=') break;

      int digit = decodeBase64Digit(character);
      if (digit < 0) return -1;

      accumulator = (accumulator << 6) | (uint32_t)digit;
      bitsHeld += 6;
      if (bitsHeld < 8) continue;

      bitsHeld -= 8;
      if (written >= outCapacity) return -1;
      out[written++] = (uint8_t)(accumulator >> bitsHeld);
   }

   return written;
}

static int decodeWgKey(uint8_t key[WG_KEY_LENGTH], const char *text)
{
   return decodeBase64(text, key, WG_KEY_LENGTH) == WG_KEY_LENGTH ? 0 : -1;
}

static int isSpace(char character)
{
   return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

// trim in place, both ends
static char *trim(char *text)
{
   while (*text && isSpace(*text)) text++;

   int length = getStrLen(text);
   while (length > 0 && isSpace(text[length - 1])) text[--length] = 0;
   return text;
}

static int parseUnsigned(const char *text, uint32_t *value)
{
   if (*text < '0' || *text > '9') return -1;

   uint32_t result = 0;
   while (*text >= '0' && *text <= '9') {
      if (result > 429496728) return -1;   // one more digit would wrap round and pass a range check
      result = result * 10 + (uint32_t)(*text - '0');
      text++;
   }

   *value = result;
   return *text == 0 ? 0 : -1;
}

// "10.2.0.2/32" or "10.2.0.1". prefixLength may be NULL when no suffix is expected.
static int parseIpv4(const char *text, uint32_t *address, int *prefixLength)
{
   uint32_t result = 0;

   for (int part = 0; part < 4; part++) {
      if (*text < '0' || *text > '9') return -1;

      uint32_t octet = 0;
      while (*text >= '0' && *text <= '9') {
         octet = octet * 10 + (uint32_t)(*text - '0');
         if (octet > 255) return -1;
         text++;
      }
      result = (result << 8) | octet;

      if (part < 3) {
         if (*text != '.') return -1;
         text++;
      }
   }

   *address = result;
   if (prefixLength) *prefixLength = 32;

   if (*text == '/') {
      uint32_t bits = 0;
      if (parseUnsigned(text + 1, &bits) != 0 || bits > 32) return -1;
      if (prefixLength) *prefixLength = (int)bits;
      return 0;
   }

   return *text == 0 ? 0 : -1;
}

// "203.0.113.7:51820" or "vpn.example.com:51820". the host is kept as text and resolved later.
static int parseEndpoint(WgConfig *config, const char *text)
{
   int separator = -1;
   for (int index = 0; text[index]; index++)
      if (text[index] == ':') separator = index;

   if (separator <= 0 || separator >= WG_ENDPOINT_MAX_LENGTH) return -1;

   for (int index = 0; index < separator; index++) config->endpointHost[index] = text[index];
   config->endpointHost[separator] = 0;

   uint32_t port = 0;
   if (parseUnsigned(text + separator + 1, &port) != 0 || port == 0 || port > 65535) return -1;
   config->endpointPort = (uint16_t)port;
   return 0;
}

int parseIpv4Text(const char *text, uint32_t *address)
{
   return parseIpv4(text, address, 0);
}

// Address and DNS may both carry a comma separated list, sometimes with IPv6 entries. we take the
// first value only, which is the IPv4 one in every provider config seen so far.
static int parseFirstIpv4(const char *text, uint32_t *address, int *prefixLength)
{
   char firstItem[WG_ENDPOINT_MAX_LENGTH];

   int length = 0;
   while (text[length] && text[length] != ',' && length < (int)sizeof firstItem - 1) length++;
   for (int index = 0; index < length; index++) firstItem[index] = text[index];
   firstItem[length] = 0;

   return parseIpv4(trim(firstItem), address, prefixLength);
}

static int applySetting(WgConfig *config, int inPeerSection, const char *key, char *value)
{
   if (!inPeerSection) {
      if (strCmpICase(key, "PrivateKey") == 0) return decodeWgKey(config->privateKey, value);
      if (strCmpICase(key, "Address") == 0)
         return parseFirstIpv4(value, &config->tunnelAddress, &config->tunnelPrefixLength);
      if (strCmpICase(key, "DNS") == 0) return parseFirstIpv4(value, &config->dnsAddress, 0);
      return 0;   // an interface setting we do not need, such as MTU or Table
   }

   if (strCmpICase(key, "PublicKey") == 0) return decodeWgKey(config->peerPublicKey, value);
   if (strCmpICase(key, "PresharedKey") == 0) {
      if (decodeWgKey(config->presharedKey, value) != 0) return -1;
      config->hasPresharedKey = 1;
      return 0;
   }
   if (strCmpICase(key, "AllowedIPs") == 0) {
      config->routesAllTraffic = findBytes(value, getStrLen(value), "0.0.0.0/0", 9) >= 0;
      return 0;
   }
   if (strCmpICase(key, "Endpoint") == 0) return parseEndpoint(config, value);
   if (strCmpICase(key, "PersistentKeepalive") == 0) {
      uint32_t seconds = 0;
      if (parseUnsigned(value, &seconds) != 0) return -1;
      config->keepaliveSeconds = (int)seconds;
      return 0;
   }

   return 0;
}

int parseWgConfig(WgConfig *config, const char *text, int length)
{
   memSet(config, 0, sizeof *config);

   int inPeerSection = 0;
   int hasPrivateKey = 0, hasPeerPublicKey = 0;
   int lineStart = 0;

   for (int position = 0; position <= length; position++) {
      if (position < length && text[position] != '\n') continue;

      int lineLength = position - lineStart;
      const char *rawLine = text + lineStart;
      lineStart = position + 1;

      // an over-long line is skipped whole rather than parsed in part, so a truncated value can
      // never be mistaken for a complete one
      if (lineLength <= 0 || lineLength >= LINE_MAX_LENGTH) continue;

      char line[LINE_MAX_LENGTH];
      memCopy(line, rawLine, lineLength);
      line[lineLength] = 0;

      char *trimmed = trim(line);
      if (trimmed[0] == 0 || trimmed[0] == '#' || trimmed[0] == ';') continue;

      if (trimmed[0] == '[') {
         inPeerSection = strCmpICase(trimmed, "[Peer]") == 0;
         continue;
      }

      // split at the first '=' into name and value
      int separator = -1;
      for (int index = 0; trimmed[index]; index++)
         if (trimmed[index] == '=') { separator = index; break; }
      if (separator <= 0) continue;

      trimmed[separator] = 0;
      char *name = trim(trimmed);
      char *value = trim(trimmed + separator + 1);

      if (applySetting(config, inPeerSection, name, value) != 0) {
         logError(TAG "config: bad value for %s\n", name);   // the name only, never the value
         return -1;
      }

      if (!inPeerSection && strCmpICase(name, "PrivateKey") == 0) hasPrivateKey = 1;
      if (inPeerSection && strCmpICase(name, "PublicKey") == 0) hasPeerPublicKey = 1;
   }

   if (!hasPrivateKey) { logError(TAG "config: no PrivateKey\n"); return -1; }
   if (!hasPeerPublicKey) { logError(TAG "config: no peer PublicKey\n"); return -1; }
   if (config->endpointPort == 0) { logError(TAG "config: no Endpoint\n"); return -1; }
   return 0;
}

int loadWgConfig(WgConfig *config, const char *path)
{
   VfsFile file;
   if (openFs(path, VFS_O_RDONLY, &file) != 0) {
      logError(TAG "config: cannot open %s\n", path);
      return -1;
   }

   char text[CONFIG_MAX_LENGTH];
   int64_t read = readFs(&file, text, sizeof text - 1);
   closeFs(&file);

   if (read <= 0) {
      logError(TAG "config: %s is empty or unreadable\n", path);
      return -1;
   }

   // a file that fills the buffer may have been cut short, and half a config parsed as a whole one
   // is worse than no config at all
   if (read == (int64_t)(sizeof text - 1)) {
      logError(TAG "config: %s is larger than %d bytes\n", path, CONFIG_MAX_LENGTH);
      memSet(text, 0, sizeof text);
      return -1;
   }

   text[read] = 0;
   int result = parseWgConfig(config, text, (int)read);
   memSet(text, 0, sizeof text);   // the file held a private key
   return result;
}
