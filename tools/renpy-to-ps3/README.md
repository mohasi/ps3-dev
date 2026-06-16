# renpy-to-ps3

Convert Ren'Py visual novel games to PlayStation 3 packages.

## Build

Open `ps3-dev.sln` in Visual Studio and build, or use MSBuild:

```powershell
msbuild tools\renpy-to-ps3\renpy-to-ps3.csproj /p:Configuration=Release
```

Output: `tools\renpy-to-ps3.exe`

## Usage

```
renpy-to-ps3 <command> [args...]

Commands:
  list <rpa-file>              List archive contents
  extract <rpa-file> <output>  Extract archive
  info <game-dir>              Show game information
```

## Status

**Current**: Skeleton created, commands are placeholders (TODO)

**Next**:
1. Implement RPA archive reading
2. Implement extraction
3. Implement asset conversion
4. Implement bytecode compiler
