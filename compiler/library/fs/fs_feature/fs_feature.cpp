#include "compiler/library/fs/fs_feature/fs_feature.h"
#include "compiler/library/fs/fs_exports/fs_exports.h"
#include "compiler/library/fs/fs_lowerer/fs_lowerer.h"
#include "compiler/lower/lowering/lowering.h"
#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_declarer/fs_declarer.h"
#include "compiler/pipeline/check_program/topics/fs/fs_error_types/fs_error_types.h"
#include "compiler/pipeline/check_program/topics/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/pipeline/check_program/topics/fs/readline_checker/readline_checker.h"

namespace tz {

void FsFeature::declare(CheckState &state) const {
  FsDeclarer(state).declareFsClasses();
  FsErrorTypes(state).declare();
}

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
  if (module == NodeModule::TzFs) {
    return tzFsExportProblem(exportName, path);
  }

  return std::nullopt;
}

Expr *FsFeature::checkModuleCall(CheckState &state, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module == NodeModule::Fs || (module == NodeModule::TzFs && fn == "replaceSync")) {
    Expr *checked = FsChecker(state).checkFsCall(call, fn, loc);
    if (auto *done = llvm::dyn_cast<CallExpr>(checked);
        done && isFsBuiltin(done->builtin()) && !done->type->isError()) {
      done->type = Type::result(done->type, FsErrorTypes(state).ofCall(done));
    }
    return checked;
  }
  if (module == NodeModule::FsPromises || (module == NodeModule::TzFs && fn == "replace")) {
    return FsPromisesChecker(state).checkFsPromisesCall(call, fn, loc);
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return ReadlineChecker(state).checkReadlineCall(call, module, fn, loc);
  }
  if (module == NodeModule::TzIo) {
    return FsChecker(state).checkTzIoCall(call, loc);
  }

  return nullptr;
}

Expr *FsFeature::checkModuleValue(CheckState &state, NodeModule module, llvm::StringRef exportName,
                                  Expr *expr) const {
  if (module != NodeModule::Fs && module != NodeModule::FsPromises) {
    return nullptr;
  }

  return FsChecker(state).checkFsValue(exportName, expr);
}

Expr *FsFeature::checkModuleMember(CheckState &state, MemberExpr *member) const {
  return FsChecker(state).checkFsConstant(member);
}

bool FsFeature::lowers(Builtin builtin) const { return isFsModuleBuiltin(builtin); }

llvm::Value *FsFeature::lowerCall(Lowering &lowering, CallExpr *call) const {
  return lowering.fsLowerer().emitFsResult(call);
}

}
