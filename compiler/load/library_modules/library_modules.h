#pragma once

#include "compiler/load/loader/loader.h"

namespace tz {

// The built-in modules of tz written in tz (library/): read once, and the globals
// of the web platform and the streams of node:fs they give to the files that name
// them.
class LibraryModules {
public:
  explicit LibraryModules(Loader &loader);

  // A built-in module of tz written in tz (Module::builtin): `file` of the
  // directory lib next to the compiler, known to the program as `name`;
  // read once. Null after reporting an error at `loc`.
  Module *libraryModule(llvm::StringRef file, llvm::StringRef name, llvm::SMLoc loc);
  // The globals of the web platform that tz has written in tz (see
  // kLibraryGlobals): EventTarget, AbortController and the others of
  // tz:web, URL of tz:url, fetch, Response and the others of tz:fetch. A file
  // that names one gets it as if it imported it, unless it declares or
  // imports that name itself.
  void importLibraryGlobals(Module *module, llvm::StringRef text);
  // The streams of node:fs and of process, written in tz
  // (library/stream/fs_stream.tz): the names createReadStream,
  // createWriteStream, ReadStream and WriteStream that a file imports from
  // node:fs come from there, and a file that uses process.stdin, process.stdout
  // or process.stderr as a stream loads it.
  void importFsStreams(Module *module, llvm::StringRef text);

private:
  Loader &loader_;
};

} // namespace tz
