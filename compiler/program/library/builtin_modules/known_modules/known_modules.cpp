#include "compiler/program/library/builtin_modules/known_modules/known_modules.h"
#include "compiler/program/ast/import_decl/import_decl.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kNodePrefix = "node:";
constexpr llvm::StringLiteral kTzPrefix = "tz:";

const KnownModuleInfo kModules[] = {
    {.path = "tz:io", .module = NodeModule::TzIo, .isNode = false, .typicalName = "readLine"},
    {.path = "tz:fs", .module = NodeModule::TzFs, .isNode = false, .typicalName = "replaceSync"},
    {.path = "tz:crypto",
     .module = NodeModule::TzCrypto,
     .isNode = false,
     .typicalName = "randomBytesAsync"},
    {.path = "fs", .module = NodeModule::Fs, .isNode = true, .typicalName = "readFileSync"},
    {.path = "fs/promises",
     .module = NodeModule::FsPromises,
     .isNode = true,
     .typicalName = "readFile"},
    {.path = "timers/promises",
     .module = NodeModule::TimersPromises,
     .isNode = true,
     .typicalName = "setTimeout"},
    {.path = "readline",
     .module = NodeModule::Readline,
     .isNode = true,
     .typicalName = "createInterface"},
    {.path = "readline/promises",
     .module = NodeModule::ReadlinePromises,
     .isNode = true,
     .typicalName = "createInterface"},
    {.path = "path", .module = NodeModule::Path, .isNode = true, .typicalName = "join"},
    {.path = "path/posix", .module = NodeModule::PathPosix, .isNode = true, .typicalName = "join"},
    {.path = "path/win32", .module = NodeModule::PathWin32, .isNode = true, .typicalName = "join"},
    {.path = "os", .module = NodeModule::Os, .isNode = true, .typicalName = "homedir"},
    {.path = "process", .module = NodeModule::Process, .isNode = true, .typicalName = "argv"},
    {.path = "buffer", .module = NodeModule::Buffer, .isNode = true, .typicalName = "Buffer"},
    {.path = "child_process",
     .module = NodeModule::ChildProcess,
     .isNode = true,
     .typicalName = "execFileSync"},
    {.path = "crypto", .module = NodeModule::Crypto, .isNode = true, .typicalName = "createHash"},
};

const SubmoduleInfo kSubmodules[] = {
    {.parent = NodeModule::Path, .name = "posix", .module = NodeModule::PathPosix},
    {.parent = NodeModule::Path, .name = "win32", .module = NodeModule::PathWin32},
    {.parent = NodeModule::PathPosix, .name = "posix", .module = NodeModule::PathPosix},
    {.parent = NodeModule::PathPosix, .name = "win32", .module = NodeModule::PathWin32},
    {.parent = NodeModule::PathWin32, .name = "posix", .module = NodeModule::PathPosix},
    {.parent = NodeModule::PathWin32, .name = "win32", .module = NodeModule::PathWin32},
    {.parent = NodeModule::Fs, .name = "promises", .module = NodeModule::FsPromises},
};

}

KnownModules::ResultFind KnownModules::find(llvm::StringRef path) {
  std::optional<NodeModule> which = KnownModules::moduleOf(path);
  if (which.has_value()) {
    NodeModule module = *which;

    return ok(module);
  }
  llvm::StringRef bare = path;
  bare.consume_front(kNodePrefix);
  bool known = path.starts_with(kTzPrefix)
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
  llvm::StringRef bare = path;
  bare.consume_front(kNodePrefix);
  for (const KnownModuleInfo &info : kModules) {
    bool isNamed = info.isNode ? bare == info.path : path == info.path;
    if (isNamed) {
      return info.module;
    }
  }

  return std::nullopt;
}

std::string KnownModules::getPath(NodeModule module) {
  for (const KnownModuleInfo &info : kModules) {
    if (info.module == module) {
      return info.isNode ? (kNodePrefix + info.path).str() : info.path.str();
    }
  }
  llvm_unreachable("module missing from the table");
}

llvm::StringRef KnownModules::getTypicalName(NodeModule module) {
  for (const KnownModuleInfo &info : kModules) {
    if (info.module == module) {
      return info.typicalName;
    }
  }
  llvm_unreachable("module missing from the table");
}

std::optional<NodeModule> KnownModules::findSubmodule(NodeModule parent, llvm::StringRef name) {
  for (const SubmoduleInfo &info : kSubmodules) {
    if (info.parent == parent && info.name == name) {
      return info.module;
    }
  }

  return std::nullopt;
}

}
