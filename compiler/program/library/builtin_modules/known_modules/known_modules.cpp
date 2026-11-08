#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/ast/import_decl/import_decl.h"

namespace tz {

KnownModules::ResultFind KnownModules::find(llvm::StringRef path) {
  std::optional<NodeModule> which = KnownModules::moduleOf(path);
  if (which.has_value()) {
    NodeModule module = *which;

    return ok(module);
  }
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  bool known = path.starts_with("tz:")
                   ? path == "tz:string"
                   : BuiltinModulePaths::isBuiltinModule(bare) || path.starts_with("node:sea") ||
                         path == "node:sqlite" || path.starts_with("node:test");
  if (!known) {
    std::string message = ErrorTextBuiltinModule::none(path);

    return err(KnownModulesFindError{
        .code = KnownModulesFindErrorCode::NO_MODULE,
        .message = message,
    });
  }
  std::string message = ErrorTextBuiltinModule::notYet(path);

  return err(KnownModulesFindError{
      .code = KnownModulesFindErrorCode::NOT_YET,
      .message = message,
  });
}

std::optional<NodeModule> KnownModules::moduleOf(llvm::StringRef path) {
  if (path == "tz:io") {
    return NodeModule::TzIo;
  }
  if (path == "tz:fs") {
    return NodeModule::TzFs;
  }
  if (path == "tz:crypto") {
    return NodeModule::TzCrypto;
  }
  llvm::StringRef bare = path;
  bare.consume_front("node:");
  if (bare == "fs") {
    return NodeModule::Fs;
  }
  if (bare == "fs/promises") {
    return NodeModule::FsPromises;
  }
  if (bare == "timers/promises") {
    return NodeModule::TimersPromises;
  }
  if (bare == "readline") {
    return NodeModule::Readline;
  }
  if (bare == "readline/promises") {
    return NodeModule::ReadlinePromises;
  }
  if (bare == "path") {
    return NodeModule::Path;
  }
  if (bare == "path/posix") {
    return NodeModule::PathPosix;
  }
  if (bare == "path/win32") {
    return NodeModule::PathWin32;
  }
  if (bare == "os") {
    return NodeModule::Os;
  }
  if (bare == "process") {
    return NodeModule::Process;
  }
  if (bare == "buffer") {
    return NodeModule::Buffer;
  }
  if (bare == "child_process") {
    return NodeModule::ChildProcess;
  }
  if (bare == "crypto") {
    return NodeModule::Crypto;
  }

  return std::nullopt;
}

}
