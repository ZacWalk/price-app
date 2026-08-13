// header.h : include file for standard system include files,
// or project specific include files
//

#pragma once

#include "targetver.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers

#include <windows.h>
#include <windowsx.h>

#undef min
#undef max

#include <stdlib.h>
#include <malloc.h>
#include <memory.h>

#include <string>
#include <vector>
#include <fstream> 
#include <limits>
