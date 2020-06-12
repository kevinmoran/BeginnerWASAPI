@echo off

set COMPILER_FLAGS=/MTd /nologo /Gm- /EHa- /GR- /fp:fast /Od /Oi /W4 /FC /Fm /Z7

set LINKER_FLAGS=/INCREMENTAL:NO /opt:ref
set SYSTEM_LIBS=ole32.lib
REM user32.lib gdi32.lib winmm.lib

set BUILD_DIR=".\build"
if not exist %BUILD_DIR% mkdir %BUILD_DIR%
pushd %BUILD_DIR%

cl %COMPILER_FLAGS% ../main.cpp /link %LINKER_FLAGS% %SYSTEM_LIBS%

popd
echo Done