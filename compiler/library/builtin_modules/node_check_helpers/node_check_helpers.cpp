#include "compiler/library/builtin_modules/node_check_helpers/node_check_helpers.h"
#include "llvm/Support/ConvertUTF.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/TargetParser/Triple.h"

namespace tz {

std::string moduleLabel(NodeModule module) {
  switch (module) {
  case NodeModule::Fs:
    return "node:fs";
  case NodeModule::FsPromises:
    return "node:fs/promises";
  case NodeModule::TimersPromises:
    return "node:timers/promises";
  case NodeModule::Readline:
    return "node:readline";
  case NodeModule::ReadlinePromises:
    return "node:readline/promises";
  case NodeModule::Path:
    return "node:path";
  case NodeModule::PathPosix:
    return "node:path/posix";
  case NodeModule::PathWin32:
    return "node:path/win32";
  case NodeModule::Os:
    return "node:os";
  case NodeModule::Process:
    return "node:process";
  case NodeModule::Buffer:
    return "node:buffer";
  case NodeModule::TzIo:
    return "tz:io";
  case NodeModule::ChildProcess:
    return "node:child_process";
  case NodeModule::Crypto:
    return "node:crypto";
  }

  return "";
}

bool targetIsWindows() { return llvm::Triple(llvm::sys::getDefaultTargetTriple()).isOSWindows(); }

Expr *stringLiteral(AstContext &ctx, llvm::SMLoc loc, llvm::StringRef text) {
  llvm::SmallVector<llvm::UTF16, 32> units;
  llvm::convertUTF8ToUTF16String(text, units);
  auto *lit = ctx.make<StringLitExpr>(loc, std::u16string(units.begin(), units.end()));
  lit->type = Type::string();

  return lit;
}

}
