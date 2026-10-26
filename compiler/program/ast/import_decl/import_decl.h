#pragma once

#include "compiler/program/ast/module_name/module_name.h"

namespace tz {

struct Module;

struct ImportDecl {
  llvm::SMLoc loc;
  std::vector<ModuleName> names;
  std::string path;
  llvm::SMLoc pathLoc;
  Module *from = nullptr;
  llvm::StringRef whole;
  llvm::SMLoc wholeLoc;
  bool wholeIsDefault = false;
  bool implicit = false;
};

inline bool isBuiltinModule(llvm::StringRef path) {
  static constexpr llvm::StringLiteral bare[] = {"assert",
                                                 "assert/strict",
                                                 "async_hooks",
                                                 "buffer",
                                                 "child_process",
                                                 "cluster",
                                                 "console",
                                                 "constants",
                                                 "crypto",
                                                 "dgram",
                                                 "diagnostics_channel",
                                                 "dns",
                                                 "dns/promises",
                                                 "domain",
                                                 "events",
                                                 "fs",
                                                 "fs/promises",
                                                 "http",
                                                 "http2",
                                                 "https",
                                                 "inspector",
                                                 "inspector/promises",
                                                 "module",
                                                 "net",
                                                 "os",
                                                 "path",
                                                 "path/posix",
                                                 "path/win32",
                                                 "perf_hooks",
                                                 "process",
                                                 "punycode",
                                                 "querystring",
                                                 "readline",
                                                 "readline/promises",
                                                 "repl",
                                                 "stream",
                                                 "stream/consumers",
                                                 "stream/promises",
                                                 "stream/web",
                                                 "string_decoder",
                                                 "sys",
                                                 "timers",
                                                 "timers/promises",
                                                 "tls",
                                                 "trace_events",
                                                 "tty",
                                                 "url",
                                                 "util",
                                                 "util/types",
                                                 "v8",
                                                 "vm",
                                                 "wasi",
                                                 "worker_threads",
                                                 "zlib"};
  if (path.starts_with("node:") || path.starts_with("tz:")) {
    return true;
  }
  for (llvm::StringLiteral name : bare) {
    if (name == path) {
      return true;
    }
  }

  return false;
}

}
