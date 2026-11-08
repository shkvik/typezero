#pragma once

#include "compiler/program/library/builtin_modules/known_modules/known_modules_errors.h"
#include "compiler/result/result/result.h"
#include "llvm/ADT/StringRef.h"
#include <optional>

namespace tz {

enum class NodeModule {
  Fs,
  FsPromises,
  Path,
  PathPosix,
  PathWin32,
  Os,
  Process,
  Buffer,
  TzIo,
  TzFs,
  TzCrypto,
  TimersPromises,
  Readline,
  ReadlinePromises,
  ChildProcess,
  Crypto,
};

class KnownModules {
public:
  using ResultFind = Result<NodeModule, KnownModulesFindError>;

  static ResultFind find(llvm::StringRef path);
  static std::optional<NodeModule> moduleOf(llvm::StringRef path);
};

}
