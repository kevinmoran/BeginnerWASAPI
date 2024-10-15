@echo off

set COMMON_COMPILER_FLAGS=/nologo /EHsc- /GR- /Oi /W4 /Fm /FC

set DEBUG_FLAGS=/DDEBUG_BUILD /DDEBUG /Od /MTd /Zi
set RELEASE_FLAGS =/O2 /DNDEBUG

set COMPILER_FLAGS=%COMMON_COMPILER_FLAGS% %DEBUG_FLAGS%
REM set COMPILER_FLAGS=%COMMON_COMPILER_FLAGS% %RELEASE_FLAGS%

set SRC_FILES=../main.cpp ../LoadWavFile.cpp ../Win32LoadEntireFile.cpp

set LINKER_FLAGS=/INCREMENTAL:NO /opt:ref
set SYSTEM_LIBS=ole32.lib

set BUILD_DIR=".\build"
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
pushd %BUILD_DIR%

echo Building...
cl %COMPILER_FLAGS% %SRC_FILES% /link %LINKER_FLAGS% %SYSTEM_LIBS%
popd
echo Done
