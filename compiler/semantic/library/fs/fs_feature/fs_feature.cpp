#include "compiler/semantic/library/fs/fs_feature/fs_feature.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/fs/fs_declarer/fs_declarer.h"
#include "compiler/semantic/library/fs/fs_errors/fs_errors.h"
#include "compiler/semantic/library/fs/fs_exports/fs_exports.h"
#include "compiler/semantic/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/semantic/library/fs/readline_checker/readline_checker.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

void FsFeature::declare(SemanticState &state) const {
  FsDeclarer(state).declareFsClasses();
  FsErrors(state).declare();
}

FsFeature::ResultExportFound FsFeature::exportFound(NodeModule module, llvm::StringRef exportName,
                                                    llvm::StringRef path) const {
  if (module == NodeModule::Fs) {
    return FsExports::fsExport(exportName, path);
  }
  if (module == NodeModule::FsPromises) {
    return FsExports::promisesExport(exportName, path);
  }
  if (module == NodeModule::Readline || module == NodeModule::ReadlinePromises) {
    return FsExports::readlineExport(exportName, path, module == NodeModule::Readline);
  }
  if (module == NodeModule::TzIo) {
    return FsExports::tzIoExport(exportName, path);
  }
  if (module == NodeModule::TzFs) {
    return FsExports::tzFsExport(exportName, path);
  }

  return err(FeatureExportFoundError{
      .code = FeatureExportFoundErrorCode::NOT_MINE,
      .message = "",
  });
}

Expr *FsFeature::checkModuleCall(SemanticState &state, CallExpr *call, NodeModule module,
                                 llvm::StringRef fn, llvm::SMLoc loc, const Type *) const {
  if (module == NodeModule::Fs || (module == NodeModule::TzFs && fn == "replaceSync")) {
    Expr *checked = FsChecker(state).checkFsCall(call, fn, loc);
    if (auto *done = llvm::dyn_cast<CallExpr>(checked);
        done && BuiltinGroups::isFsBuiltin(done->builtin()) && !done->type->isError()) {
      done->type = Type::result(done->type, FsErrors(state).ofCall(done));
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

Expr *FsFeature::checkModuleValue(SemanticState &state, NodeModule module,
                                  llvm::StringRef exportName, Expr *expr) const {
  if (module != NodeModule::Fs && module != NodeModule::FsPromises) {
    return nullptr;
  }

  return FsChecker(state).checkFsValue(exportName, expr);
}

Expr *FsFeature::checkModuleMember(SemanticState &state, MemberExpr *member) const {
  return FsChecker(state).checkFsConstant(member);
}

}
