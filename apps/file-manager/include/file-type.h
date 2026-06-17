#pragma once

// file-type - classifies filenames into a fixed set of types and maps each
// type to a display name and sprite. extension lists live in file-type.c.

#include "sprite-regions.h"

typedef enum {
   FILE_TYPE_FOLDER,
   FILE_TYPE_TEXT,
   FILE_TYPE_AUDIO,
   FILE_TYPE_VIDEO,
   FILE_TYPE_IMAGE,
   FILE_TYPE_EXECUTABLE,
   FILE_TYPE_COMPRESSED,
   FILE_TYPE_DISC_ISO,
   FILE_TYPE_PACKAGE,
   FILE_TYPE_DOCUMENT,
   FILE_TYPE_DATABASE,
   FILE_TYPE_GENERIC,
   FILE_TYPE_COUNT
} FileType;

FileType    classifyFileType(const char *name, int isDir);
const char *getFileTypeName(FileType type);
int         getFileTypeSprite(FileType type);
