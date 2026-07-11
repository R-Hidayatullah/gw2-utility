@echo off
setlocal EnableExtensions

REM ==========================================
REM Build gw2index.exe (MinGW)
REM ==========================================

REM Lokasi MinGW
set "MINGW=C:\Users\Ridwan Hidayatullah\Documents\codeblocks-25.03mingw-nosetup\MinGW"

REM Tambahkan ke PATH
set "PATH=%MINGW%\bin;%PATH%"

REM Pindah ke folder script ini
cd /d "%~dp0"

REM Folder native
set "NAT=..\gw2mcp\native"

REM Release flags
set "CFLAGS=-O3 -DNDEBUG -flto"
set "CXXFLAGS=-std=c++20 %CFLAGS%"
set "LDFLAGS=-flto -s -static -static-libgcc -static-libstdc++"

REM ====================================================
REM Compile sqlite3.c jika belum ada atau source lebih baru
REM ====================================================

set COMPILE_SQLITE=0

if not exist sqlite3.o (
    set COMPILE_SQLITE=1
) else (
    for %%F in (sqlite3.c) do set SRC=%%~tF
    for %%F in (sqlite3.o) do set OBJ=%%~tF

    if "!SRC!" GTR "!OBJ!" (
        set COMPILE_SQLITE=1
    )
)

if %COMPILE_SQLITE%==1 (
    echo Compiling sqlite3.c...

    gcc %CFLAGS% ^
        -DSQLITE_THREADSAFE=1 ^
        -DSQLITE_OMIT_LOAD_EXTENSION ^
        -c sqlite3.c ^
        -o sqlite3.o

    if errorlevel 1 goto :error
)

echo.
echo Building gw2index_released.exe...

g++ %CXXFLAGS% ^
    -I . ^
    -I "%NAT%\include" ^
    -I "%NAT%\third_party" ^
    gw2index.cpp ^
    "%NAT%\src\gw2dat.cpp" ^
    "%NAT%\src\BinaryParser.cpp" ^
    sqlite3.o ^
    -o gw2index_released.exe ^
    %LDFLAGS%

if errorlevel 1 goto :error

echo.
echo ======================================
echo Release build complete.
echo Output: gw2index_released.exe
echo ======================================
exit /b 0

:error
echo.
echo Build FAILED.
exit /b 1