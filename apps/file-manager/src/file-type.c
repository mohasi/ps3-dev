// file-type - see file-type.h
#include "file-type.h"
#include "file.h"
#include "string-utilities.h"

static const char *names[FILE_TYPE_COUNT] = {
   [FILE_TYPE_FOLDER]     = "Folder",
   [FILE_TYPE_TEXT]       = "Text",
   [FILE_TYPE_AUDIO]      = "Audio",
   [FILE_TYPE_VIDEO]      = "Video",
   [FILE_TYPE_IMAGE]      = "Image",
   [FILE_TYPE_EXECUTABLE] = "Executable",
   [FILE_TYPE_COMPRESSED] = "Archive",
   [FILE_TYPE_DISC_ISO]   = "Disc Image",
   [FILE_TYPE_PACKAGE]    = "Package",
   [FILE_TYPE_DOCUMENT]   = "Document",
   [FILE_TYPE_DATABASE]   = "Database",
   [FILE_TYPE_GENERIC]    = "File",
};

static const int sprites[FILE_TYPE_COUNT] = {
   [FILE_TYPE_FOLDER]     = SPRITE_FOLDER,
   [FILE_TYPE_TEXT]       = SPRITE_TEXT,
   [FILE_TYPE_AUDIO]      = SPRITE_AUDIO,
   [FILE_TYPE_VIDEO]      = SPRITE_VIDEO,
   [FILE_TYPE_IMAGE]      = SPRITE_IMAGE,
   [FILE_TYPE_EXECUTABLE] = SPRITE_EXECUTABLE,
   [FILE_TYPE_COMPRESSED] = SPRITE_COMPRESSED,
   [FILE_TYPE_DISC_ISO]   = SPRITE_DISC_ISO,
   [FILE_TYPE_PACKAGE]    = SPRITE_PACKAGE,
   [FILE_TYPE_DOCUMENT]   = SPRITE_DOCUMENT,
   [FILE_TYPE_DATABASE]   = SPRITE_DATABASE,
   [FILE_TYPE_GENERIC]    = SPRITE_GENERIC,
};

// extension -> FileType. each row is a NULL-terminated list of extensions and
// the type they map to. order doesn't matter; first match wins.
static const struct {
   FileType    type;
   const char *exts[16];
} groups[] = {
   { FILE_TYPE_TEXT,       { "txt", "xml", "json", "ini", "cfg", "conf", "log", "md", "csv", "htm", "html", "yaml", "yml", NULL } },
   { FILE_TYPE_AUDIO,      { "mp3", "wav", "flac", "ogg", "aac", "wma", "at3", "m4a", NULL } },
   { FILE_TYPE_VIDEO,      { "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", NULL } },
   { FILE_TYPE_IMAGE,      { "png", "jpg", "jpeg", "bmp", "gif", "tga", "tiff", NULL } },
   { FILE_TYPE_EXECUTABLE, { "self", "elf", "bin", "sprx", "prx", NULL } },
   { FILE_TYPE_COMPRESSED, { "zip", "rar", "7z", "tar", "gz", "bz2", NULL } },
   { FILE_TYPE_DISC_ISO,   { "iso", "cso", "img", NULL } },
   { FILE_TYPE_PACKAGE,    { "pkg", NULL } },
   { FILE_TYPE_DOCUMENT,   { "pdf", "doc", "docx", "rtf", "xls", "xlsx", "ppt", "pptx", NULL } },
   { FILE_TYPE_DATABASE,   { "db", "sqlite", NULL } },
};

FileType classifyFileType(const char *name, int isDir)
{
   if (isDir) return FILE_TYPE_FOLDER;
   const char *ext = getExtension(name);
   if (!ext) return FILE_TYPE_GENERIC;

   for (int g = 0; g < (int)(sizeof(groups) / sizeof(groups[0])); g++)
      for (int i = 0; groups[g].exts[i]; i++)
         if (strCmpICase(ext, groups[g].exts[i]) == 0)
            return groups[g].type;

   return FILE_TYPE_GENERIC;
}

const char *getFileTypeName(FileType type)   { return names[type]; }
int         getFileTypeSprite(FileType type) { return sprites[type]; }
