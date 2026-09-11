#pragma once

// Just enough JSON to read a web API's answer: objects holding a handful of named values, and
// arrays of those objects. It does not build a tree and allocates nothing, it walks the text the
// caller already holds.
//
// It is not a validator either. A document it cannot make sense of yields nothing rather than an
// error, which is what a reader of search results wants.

#include <stdint.h>

// Find the array to walk. key is the member holding it, or empty when the document is itself an
// array. Returns 0 and the bounds of what is inside the brackets, or -1.
int findJsonArray(const char *text, int length, const char *key, int *start, int *end);

// The next object between start and end. Returns the offset just past it, 0 when there are no more,
// and fills the object's own bounds. Pass the previous return as offset to walk the array.
int readJsonObject(const char *text, int end, int offset, int *objectStart, int *objectEnd);

// A member that is itself an object, so the members inside it can be read in turn. Fills the bounds
// of what is inside its braces. Returns 0, or -1. Members are only ever looked up at the top level
// of the text handed in, so this is how a nested value is reached.
int getJsonObject(const char *text, int length, const char *key, int *objectStart, int *objectEnd);

// A member of one object, as text. Numbers come back as they were written. Newlines and tabs in a
// string become spaces. Returns 0, or -1 when the object has no such member.
int getJsonText(const char *object, int length, const char *key, char *out, int capacity);

// The body of one string, text pointing just past its opening quote, up to the closing quote or
// length bytes. Escapes are turned back and \uXXXX becomes UTF-8 (a surrogate pair becomes its one
// character). Never cuts a multi-byte character at the capacity.
void decodeJsonString(const char *text, int length, char *out, int capacity);

int64_t getJsonNumber(const char *object, int length, const char *key);   // 0 when it is not there
