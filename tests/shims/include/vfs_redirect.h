// Force-included into the harness builds: file calls on the device's
// mount point (/sdcard/...) land in a host directory the test picked
// (shim::sd_root). The real <cstdio>/<dirent.h>/<algorithm> come first so the macros only
// rename call sites, never the library's own declarations.
#pragma once
#ifdef __cplusplus
#include <algorithm>  // declares std::remove(first, last, v) before the macro below
#include <cstdio>
#else
#include <stdio.h>
#endif
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif
FILE* shim_fopen(const char* path, const char* mode);
DIR* shim_opendir(const char* path);
int shim_remove(const char* path);
int shim_rename(const char* from, const char* to);
#ifdef __cplusplus
}
#endif

#define fopen(p, m) shim_fopen(p, m)
#define opendir(p) shim_opendir(p)
#define remove(p) shim_remove(p)
#define rename(a, b) shim_rename(a, b)
