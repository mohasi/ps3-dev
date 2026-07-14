// tree-walk - see tree-walk.h
#include "tree-walk.h"
#include "string-utilities.h"   // getStrLen / memCopy / strCopy (core links no libc)

#define WALK_DEPTH_MAX 64

void walkTree(const char *root, WalkVisit visit, void *ctx, volatile int *cancel)
{
   VfsStat st;
   if (statPath(root, &st) != 0 || !st.isDir) return;

   VfsDir dirStack[WALK_DEPTH_MAX];
   int    lenStack[WALK_DEPTH_MAX];
   char   path[MAX_PATH_LEN];

   strCopy(path, sizeof path, root);
   if (openDir(path, &dirStack[0]) != 0) return;
   lenStack[0] = getStrLen(path);
   int top = 1;

   char name[256];
   VfsEntryType type;
   while (top > 0) {
      if (cancel && *cancel) break;

      if (readDir(&dirStack[top - 1], name, sizeof name, &type) != 1) {
         closeDir(&dirStack[top - 1]);
         top--;
         if (top > 0) path[lenStack[top]] = '\0';
         continue;
      }
      if (name[0] == '.') continue;   // readDir drops "."/".."; skip other dotfiles too

      // append "/name" to the parent path (reusing the shared buffer)
      int parentLen  = lenStack[top - 1];
      path[parentLen] = '\0';
      int needsSlash = parentLen > 0 && path[parentLen - 1] != '/';
      int nameLen    = getStrLen(name);
      int childLen   = parentLen + (needsSlash ? 1 : 0) + nameLen;
      if (childLen >= MAX_PATH_LEN) { path[parentLen] = '\0'; continue; }
      if (needsSlash) path[parentLen++] = '/';
      memCopy(path + parentLen, name, nameLen);
      path[childLen] = '\0';

      if (visit(path, name, type, ctx) == WALK_STOP) break;

      // descend into directories (type comes from readDir, no stat); truncate back for anything else
      if (type == VFS_ENTRY_DIR && top < WALK_DEPTH_MAX && openDir(path, &dirStack[top]) == 0) {
         lenStack[top] = childLen;
         top++;
      } else {
         path[lenStack[top - 1]] = '\0';
      }
   }

   while (top > 0) closeDir(&dirStack[--top]);
}
