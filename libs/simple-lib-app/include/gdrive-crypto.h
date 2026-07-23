#pragma once
//
// gdrive-crypto.h - console-bound obfuscation for the Google Drive credential bundle.
//
// Encrypts a short string with a key derived from THIS console's unique Open PSID,
// so the ciphertext is meaningless if settings.txt ever leaves the console (synced,
// posted, pulled over FTP to a PC). The output is hex text safe for one settings.txt
// line.
//
// This is not audited AES: it is a SHA-256 keystream (CTR-style) keyed on the PSID.
// It is enough to keep a leaked token unreadable off-console; it does not defend
// against code running ON the same console (which can read the PSID too).
//
int gdriveEncryptSecret(const char *plain, char *outHex, int outCap);   // 0 ok, -1 on failure
int gdriveDecryptSecret(const char *hex, char *outPlain, int outCap);   // 0 ok, -1 on failure
