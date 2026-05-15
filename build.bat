@echo off
setlocal enabledelayedexpansion
rem build.bat <kind> <name> [Build|Rebuild|Clean] [Release|Debug]
rem   kind = plugins | apps | tools
rem run from inside the Win7 VM via SSH; output is streamed back over the pipe.

set "KIND=%~1"
set "NAME=%~2"
set "TARGET=%~3"
set "CFG=%~4"
if "%TARGET%"=="" set "TARGET=Build"
if "%CFG%"=="" set "CFG=Release"

if "%KIND%"=="" goto :usage
if "%NAME%"=="" goto :usage

set "ROOT=%~dp0"
set "EXT="
set "PLAT="
if /I "%KIND%"=="plugins" ( set "EXT=vcxproj" & set "PLAT=PS3" )
if /I "%KIND%"=="apps"    ( set "EXT=vcxproj" & set "PLAT=PS3" )
if /I "%KIND%"=="tools"   ( set "EXT=csproj"  & set "PLAT=AnyCPU" )
if "%EXT%"=="" (
   echo [build] unknown kind: %KIND% ^(must be plugins, apps, or tools^)
   exit /b 2
)

set "PROJ=%ROOT%%KIND%\%NAME%\%NAME%.%EXT%"
if not exist "%PROJ%" (
   echo [build] no such project: %PROJ%
   exit /b 2
)

call "C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC\vcvarsall.bat" x86 >nul
if errorlevel 1 (
   echo [build] failed to load vcvars
   exit /b 3
)

echo [build] %TARGET% %KIND%\%NAME% ^(%CFG%^|%PLAT%^)
msbuild "%PROJ%" /t:%TARGET% /p:Configuration=%CFG% /p:Platform=%PLAT% /p:SolutionDir=%ROOT% /v:minimal /nologo /clp:ForceConsoleColor=false;ShowTimestamp=false
exit /b %ERRORLEVEL%

:usage
echo usage: build.bat ^<plugins^|apps^|tools^> ^<name^> [Build^|Rebuild^|Clean] [Release^|Debug]
exit /b 2
