#pragma once

#include "compiler/program/source_file/source_file.h"

namespace tz {

// A file of a program.
struct Module {
  // Where the file is: for the entry as given, for the others found from it.
  std::string path;
  // The file relative to the entry's directory, with '/': "geometry/index.tz".
  std::string name;
  SourceFile file;
  bool isEntry = false;
  // A built-in module of tz written in tz (library/), which the loader reads
  // from next to the compiler: node:events, tz:http, and the modules of the
  // globals of the web platform such as EventTarget, URL and fetch. Only its
  // code may declare and call the intrinsics `__tz_*`.
  bool builtin = false;
  unsigned order = 0; // in the order files load
  // The files that were loading when this one started to, from the entry
  // on, this one last: the chain an import cycle shows.
  std::vector<Module *> loadPath;
};

} // namespace tz
