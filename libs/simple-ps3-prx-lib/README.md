# simple-ps3-prx-lib

Static library for PS3 VSH plugins (PRX context). Compiled with `-fno-builtin-printf -nodefaultlibs` for safe use inside `vsh.self`.

## What it provides

- **printf** — drop-in `snprintf`/`vsnprintf` that doesn't depend on libc (from `simple-ps3-lib/src/printf.c`)

## Usage

Link with `-L"$(SolutionDir)libs\simple-ps3-prx-lib\bin\$(Configuration)" -lsimple-ps3-prx-lib` in your PRX project's additional linker options.

Include headers from `simple-ps3-lib/include/` as usual (`printf.h`, `dbg.h`, `vsh.h`, etc.).

## Design

This lib compiles context-independent source files from `simple-ps3-lib` with PRX-safe flags. Files that are only safe in app context (gfx, font, audio, etc.) are never included here.
