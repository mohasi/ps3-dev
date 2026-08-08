#include "wg-dns.h"

#include "string-utilities.h"
#include "wg-bytes.h"

#define DNS_HEADER_LENGTH  12
#define DNS_TYPE_ADDRESS   1
#define DNS_CLASS_INTERNET 1
#define DNS_LABEL_MAX      63
#define NAME_IS_POINTER    0xC0   // a name can point back into the message instead of repeating it

int buildDnsQuery(uint8_t *query, int capacity, uint16_t transactionId, const char *hostName)
{
   if (capacity < DNS_HEADER_LENGTH) return -1;

   memSet(query, 0, DNS_HEADER_LENGTH);
   store16be(query, transactionId);
   store16be(query + 2, 0x0100);   // ask the server to do the work of following referrals
   store16be(query + 4, 1);        // one question

   // the name becomes a run of labels: "www.example.com" as 3www7example3com then a zero
   int offset = DNS_HEADER_LENGTH;
   int labelStart = 0;
   for (int index = 0;; index++) {
      char character = hostName[index];
      if (character != '.' && character != 0) continue;

      int labelLength = index - labelStart;
      if (labelLength <= 0 || labelLength > DNS_LABEL_MAX) return -1;
      if (offset + 1 + labelLength + 5 > capacity) return -1;

      query[offset++] = (uint8_t)labelLength;
      memCopy(query + offset, hostName + labelStart, labelLength);
      offset += labelLength;

      labelStart = index + 1;
      if (character == 0) break;
   }

   query[offset++] = 0;
   store16be(query + offset, DNS_TYPE_ADDRESS);
   store16be(query + offset + 2, DNS_CLASS_INTERNET);
   return offset + 4;
}

// step over a name, whether spelled out or pointing back into the message. returns the offset
// after it, or -1 if it runs off the end.
static int skipName(const uint8_t *message, int length, int offset)
{
   while (offset < length) {
      uint8_t labelLength = message[offset];
      if (labelLength == 0) return offset + 1;
      if ((labelLength & NAME_IS_POINTER) == NAME_IS_POINTER) return offset + 2 <= length ? offset + 2 : -1;

      offset += 1 + labelLength;
   }
   return -1;
}

int readDnsAnswer(const uint8_t *answer, int length, uint16_t expectedTransactionId, uint32_t *address)
{
   if (length < DNS_HEADER_LENGTH) return -1;
   if (load16be(answer) != expectedTransactionId) return -1;

   uint16_t flags = load16be(answer + 2);
   if ((flags & 0x8000) == 0) return -1;   // not a reply
   if ((flags & 0x000F) != 0) return -1;   // the server reported an error

   int questionCount = load16be(answer + 4);
   int answerCount = load16be(answer + 6);
   if (answerCount <= 0) return -1;

   // the questions are echoed back before the answers
   int offset = DNS_HEADER_LENGTH;
   for (int index = 0; index < questionCount; index++) {
      offset = skipName(answer, length, offset);
      if (offset < 0 || offset + 4 > length) return -1;
      offset += 4;
   }

   // take the first address; a name that leads to another name gives records we skip over
   for (int index = 0; index < answerCount; index++) {
      offset = skipName(answer, length, offset);
      if (offset < 0 || offset + 10 > length) return -1;

      uint16_t recordType = load16be(answer + offset);
      uint16_t recordClass = load16be(answer + offset + 2);
      int dataLength = load16be(answer + offset + 8);
      offset += 10;
      if (offset + dataLength > length) return -1;

      if (recordType == DNS_TYPE_ADDRESS && recordClass == DNS_CLASS_INTERNET && dataLength == 4) {
         *address = load32be(answer + offset);
         return 0;
      }

      offset += dataLength;
   }

   return -1;
}
