#include "compiler/library/fs/fs_feature/fs_feature.h"
#include "compiler/check/checker/checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/library/fs/fs_exports/fs_exports.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/library/fs/readline_checker/readline_checker.h"
#include "compiler/lower/lowering/lowering.h"

namespace tz {

void FsFeature::declare(Checker &checker) const { checker.fsDeclarer().declareFsClasses(); }

std::optional<std::string> FsFeature::exportProblem(NodeModule module, llvm::StringRef exportName,
                                                    llvm::StringRef path) const {
  if (module == NodeModule::Fs) {
    return fsExportProblem(exportName, path);
  }
  if (module == NodeModule::FsPromises) {
    return fsPromisesExportProblem(exportName, path);
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return readlineExportProblem(exportName, path, module == NodeModule::Readline);
  }
  if (module == NodeModule::TzIo) {
    return tzIoExportProblem(exportName, path);
  }

  return std::nullopt;
}

Expr *FsFeature::checkModuleCall(Checker &checker, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module == NodeModule::Fs) {
    return checker.fsChecker().checkFsCall(call, fn, loc);
  }
  if (module == NodeModule::FsPromises) {
    return checker.fsPromisesChecker().checkFsPromisesCall(call, fn, loc);
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return checker.readlineChecker().checkReadlineCall(call, module, fn, loc);
  }
  if (module == NodeModule::TzIo) {
    return checker.fsChecker().checkTzIoCall(call, loc);
  }

  return nullptr;
}

Expr *FsFeature::checkModuleValue(Checker &checker, NodeModule module, llvm::StringRef exportName,
                                  Expr *expr) const {
  if (module != NodeModule::Fs && module != NodeModule::FsPromises) {
    return nullptr;
  }

  return checker.fsChecker().checkFsValue(exportName, expr);
}

Expr *FsFeature::checkModuleMember(Checker &checker, MemberExpr *member) const {
  return checker.fsChecker().checkFsConstant(member);
}

bool FsFeature::lowers(Builtin builtin) const { return isFsModuleBuiltin(builtin); }

llvm::Value *FsFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.fsLowerer().emitFsCall(call);
}

}
