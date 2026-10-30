#pragma once
#include <FS.h>
class LittleFSFS {
public:
  bool begin(bool = false) { return true; }
  File open(const char* p, const char* mode) {
    auto& f = host::files();
    if (mode[0] == 'r' && !f.count(p)) return File();
    if (mode[0] == 'w') f[p] = "";
    if (mode[0] == 'a' && !f.count(p)) f[p] = "";
    return File(p, mode[0] != 'r');
  }
  bool remove(const char* p) { return host::files().erase(p) > 0; }
  bool rename(const char* a, const char* b) { auto& f = host::files(); if (!f.count(a)) return false; f[b] = f[a]; f.erase(a); return true; }
};
extern LittleFSFS LittleFS;
