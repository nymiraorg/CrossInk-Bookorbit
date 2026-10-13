#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

// In-memory filesystem stub for the BookOrbitDownloadIndex unit test. Provides the
// read/write/fileSize FsFile API and the openFileForRead/openFileForWrite Storage API
// the index uses, backed by a plain std::map of byte vectors.

namespace fake {

struct Node {
  std::vector<uint8_t> bytes;
  bool directory = false;
};

inline std::map<std::string, std::shared_ptr<Node>> files;

inline void reset() { files.clear(); }
inline void add(const std::string& path, const std::string& content) {
  auto node = std::make_shared<Node>();
  node->bytes.assign(content.begin(), content.end());
  files[path] = node;
}

}  // namespace fake

class HalFile {
 public:
  std::shared_ptr<fake::Node> node;
  size_t pos = 0;

  explicit operator bool() const { return bool(node); }
  bool isOpen() const { return bool(node); }
  bool close() {
    node.reset();
    return true;
  }
  int read(void* out, size_t size) {
    if (!node) return 0;
    size = std::min(size, node->bytes.size() - std::min(pos, node->bytes.size()));
    std::memcpy(out, node->bytes.data() + std::min(pos, node->bytes.size()), size);
    pos += size;
    return static_cast<int>(size);
  }
  size_t write(const uint8_t* data, size_t size) {
    if (!node) return 0;
    node->bytes.resize(std::max(node->bytes.size(), pos + size));
    std::memcpy(node->bytes.data() + pos, data, size);
    pos += size;
    return size;
  }
  size_t write(const void* data, size_t size) { return write(static_cast<const uint8_t*>(data), size); }
  size_t fileSize() const { return node ? node->bytes.size() : 0; }
  bool isDirectory() const { return node && node->directory; }
};

using FsFile = HalFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  bool exists(const char* path) const { return fake::files.count(path) != 0; }
  bool mkdir(const char* path) {
    auto& node = fake::files[path];
    if (!node) node = std::make_shared<fake::Node>();
    node->directory = true;
    return true;
  }
  bool remove(const char* path) { return fake::files.erase(path) != 0; }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    const auto it = fake::files.find(path);
    if (it == fake::files.end()) return false;
    file.node = it->second;
    file.pos = 0;
    return true;
  }
  bool openFileForRead(const char* module, const std::string& path, HalFile& file) {
    return openFileForRead(module, path.c_str(), file);
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    auto& node = fake::files[path];
    if (!node) node = std::make_shared<fake::Node>();
    file.node = node;
    file.pos = 0;
    return true;
  }
  bool openFileForWrite(const char* module, const std::string& path, HalFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }
};

#define Storage HalStorage::getInstance()
