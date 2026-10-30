#include "compiler/load/library_files/library_files.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"

namespace tz {

namespace {

namespace fs = llvm::sys::fs;

namespace path = llvm::sys::path;

struct KnownFile {
  llvm::StringLiteral file;
  llvm::StringLiteral library;
  llvm::StringLiteral module;
};

const KnownFile kKnownFiles[] = {
    {"events.tz", "events.tz", "node:events"},
    {"web.tz", "web.tz", "tz:web"},
    {"http.tz", "http.tz", "tz:http"},
    {"http.d.tz", "http.tz", "tz:http"},
    {"fetch.tz", "fetch.tz", "tz:fetch"},
    {"url.tz", "url.tz", "tz:url"},
    {"websocket.tz", "websocket.tz", "tz:websocket"},
    {"intrinsics.tz", "intrinsics.tz", "tz:intrinsics"},
    {"net.tz", "net.tz", "node:net"},
    {"dgram.tz", "dgram.tz", "node:dgram"},
    {"packet.tz", "packet.tz", "tz:packet"},
    {"stream.tz", "stream.tz", "node:stream"},
    {"stream_duplex.tz", "stream.tz", "node:stream"},
    {"stream_pipeline.tz", "stream.tz", "node:stream"},
    {"stream_promises.tz", "stream_promises.tz", "node:stream/promises"},
    {"stream_consumers.tz", "stream_consumers.tz", "node:stream/consumers"},
    {"string_decoder.tz", "string_decoder.tz", "node:string_decoder"},
    {"fs_stream.tz", "fs_stream.tz", "node:internal/fs/streams"},
    {"signals.tz", "signals.tz", "node:internal/process/signals"},
    {"webcrypto.tz", "webcrypto.tz", "tz:webcrypto"},
    {"locale.tz", "locale.tz", "tz:locale"},
};

}

std::optional<LibraryFile> LibraryFiles::find(llvm::StringRef file) {
  llvm::StringRef name = path::filename(file);
  const KnownFile *known = nullptr;
  for (const KnownFile &candidate : kKnownFiles) {
    if (candidate.file == name) {
      known = &candidate;
    }
  }
  if (!known) {
    return std::nullopt;
  }
  llvm::SmallString<256> tree(path::parent_path(path::parent_path(file)));
  if (path::filename(tree) != "library") {
    return std::nullopt;
  }
  llvm::SmallString<256> marker(tree);
  path::append(marker, "CMakeLists.txt");
  llvm::SmallString<256> intrinsics(tree);
  path::append(intrinsics, "core", "intrinsics.tz");
  if (!fs::is_regular_file(marker) || !fs::is_regular_file(intrinsics)) {
    return std::nullopt;
  }

  return LibraryFile{std::string(tree), known->library.str(), known->module.str()};
}

std::string LibraryFiles::pathIn(llvm::StringRef tree, llvm::StringRef file) {
  std::error_code ec;
  for (fs::directory_iterator it(tree, ec), end; it != end && !ec; it.increment(ec)) {
    llvm::SmallString<256> candidate(it->path());
    path::append(candidate, file);
    if (fs::is_regular_file(candidate)) {
      return std::string(candidate);
    }
  }

  return "";
}

}
