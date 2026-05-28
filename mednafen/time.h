// mednafen/time.h — forward to system <time.h>
//
// On Windows/MinGW, this file shadows the system <time.h> when mednafen/ is
// in the include path (-I mednafen). Without this stub, GCC finds no file here
// and falls through to the system header on Linux, but on Windows a generated
// C++ time.h can appear and break C compilation (trio, minilzo).
//
// #include_next skips this file and searches the remaining include path for
// the next <time.h>, which is always the real system header.
#pragma once
#include_next <time.h>
