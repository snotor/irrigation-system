#pragma once
#include <Arduino.h>
#include <map>
#include <memory>
#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"
namespace host { std::map<std::string, std::string>& files(); }
// In-memory file: reads copy, writes go straight to the map
class File : public Stream {
public:
  File() {}
  File(const std::string& path, bool write) : _path(path), _open(true), _write(write) {}
  explicit operator bool() const { return _open; }
  size_t size() const { return data().size(); }
  bool seek(size_t p) { _pos = p; return true; }
  int available() override { return _open && _pos < data().size() ? (int)(data().size() - _pos) : 0; }
  int read() override { return available() ? (uint8_t)data()[_pos++] : -1; }
  size_t read(uint8_t* b, size_t n) { size_t i = 0; while (i < n && available()) b[i++] = (uint8_t)data()[_pos++]; return i; }
  size_t write(uint8_t c) override { host::files()[_path] += (char)c; return 1; }
  size_t write(const uint8_t* b, size_t n) override { host::files()[_path].append((const char*)b, n); return n; }
  void close() { _open = false; }
private:
  const std::string& data() const { return host::files()[_path]; }
  std::string _path;
  bool _open = false, _write = false;
  size_t _pos = 0;
};
