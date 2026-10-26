#include "compiler/packages/package_cache/package_cache.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>

namespace tz::pkg {

namespace fs = llvm::sys::fs;
namespace path = llvm::sys::path;

std::string cacheDir() {
  if (std::optional<std::string> dir = llvm::sys::Process::GetEnv("TZ_CACHE");
      dir && !dir->empty()) {
    return *dir;
  }
  llvm::SmallString<256> dir;
  if (!path::cache_directory(dir)) {
    path::system_temp_directory(false, dir);
  }
#ifdef _WIN32
  path::append(dir, "tz", "cache");
#else
  path::append(dir, "tz");
#endif

  return std::string(dir);
}

std::string cachePath(std::initializer_list<llvm::StringRef> parts) {
  llvm::SmallString<256> result(cacheDir());
  for (llvm::StringRef part : parts) {
    path::append(result, part);
  }

  return std::string(result);
}

std::string sha256Hex(llvm::StringRef data) {
  llvm::SHA256 hasher;
  hasher.update(data);
  std::array<uint8_t, 32> digest = hasher.final();

  return llvm::toHex(digest, true);
}

std::string treeHash(std::vector<std::pair<std::string, std::string>> files) {
  std::sort(files.begin(), files.end());
  std::string list;
  for (const auto &[file, hex] : files) {
    list += file + " sha256-" + hex + "\n";
  }

  return "sha256-" + sha256Hex(list);
}

bool hashDirectory(llvm::StringRef dir, std::string &hash, std::string &error) {
  std::vector<std::pair<std::string, std::string>> files;
  std::error_code ec;
  for (fs::recursive_directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
    if (!fs::is_regular_file(it->path())) {
      continue;
    }
    auto buffer = llvm::MemoryBuffer::getFile(it->path(), false, false);
    if (!buffer) {
      error = "cannot read '" + it->path() + "': " + buffer.getError().message();
      return false;
    }
    llvm::StringRef relative = llvm::StringRef(it->path()).drop_front(dir.size());
    relative = relative.ltrim("/\\");
    files.push_back({path::convert_to_slash(relative), sha256Hex((*buffer)->getBuffer())});
  }
  if (ec) {
    error = "cannot read '" + dir.str() + "': " + ec.message();
    return false;
  }
  hash = treeHash(std::move(files));

  return true;
}

bool writeFileAtomically(llvm::StringRef file, llvm::StringRef data, std::string &error) {
  if (std::error_code ec = fs::create_directories(path::parent_path(file))) {
    error = "cannot create '" + path::parent_path(file).str() + "': " + ec.message();
    return false;
  }
  int fd;
  llvm::SmallString<256> temp;
  std::error_code ec = fs::createUniqueFile(file + ".tmp-%%%%%%", fd, temp);
  if (!ec) {
    llvm::raw_fd_ostream out(fd, true);
    out << data;
    out.close();
    if (out.has_error()) {
      ec = out.error();
      out.clear_error();
    }
  }
  if (!ec) {
    ec = fs::rename(temp, file);
  }
  if (ec) {
    if (!temp.empty()) {
      fs::remove(temp);
    }
    error = "cannot write '" + file.str() + "': " + ec.message();
    return false;
  }

  return true;
}

}
