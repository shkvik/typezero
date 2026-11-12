#include "compiler/semantic/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/library/fs/fs_checker/fs_checker.h"
#include "compiler/semantic/library/fs/fs_errors/fs_errors.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

FsPromisesChecker::FsPromisesChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fsErrors = &all.fsErrors;
  this->fsChecker = &all.fsChecker;
  this->exprChecker = &all.exprChecker;
}

FsPromisesChecker::ResultPromiseFunction
FsPromisesChecker::checkPromiseFunction(llvm::StringRef functionName) const {
  if (functionName == "constants") {
    auto message = ErrorTextFsPromises::constants();

    return err(FsPromisesCheckerPromiseFunctionError{
        .code = FsPromisesCheckerPromiseFunctionErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (functionName == "open") {
    auto message = ErrorTextFsPromises::open();

    return err(FsPromisesCheckerPromiseFunctionError{
        .code = FsPromisesCheckerPromiseFunctionErrorCode::NO_FILE_HANDLE,
        .message = message,
    });
  }

  return ok(functionName);
}

Expr *FsPromisesChecker::checkFsPromisesCall(CallExpr *call, llvm::StringRef functionName,
                                             llvm::SMLoc loc) {
  this->state.getInventoryStore().markUsesAsync();
  call->type = Type::error();
  auto function = this->checkPromiseFunction(functionName);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    for (Expr *&arg : call->args) {
      arg = this->exprChecker->checkExpr(arg);
    }
    return call;
  }
  auto &files = this->state.getFileSystemStore();
  files.enterPromiseCall();
  this->fsChecker->checkFsCall(call, functionName, loc);
  files.leavePromiseCall();
  if (call->type->isError()) {
    return call;
  }
  static const std::pair<Builtin, Builtin> waiting[] = {
      {Builtin::FsReadFile, Builtin::FspReadFile}, {Builtin::FsWriteFile, Builtin::FspWriteFile},
      {Builtin::FsAccess, Builtin::FspAccess},     {Builtin::FsChmod, Builtin::FspChmod},
      {Builtin::FsTruncate, Builtin::FspTruncate}, {Builtin::FsStat, Builtin::FspStat},
      {Builtin::FsReaddir, Builtin::FspReaddir},   {Builtin::FsMkdir, Builtin::FspMkdir},
      {Builtin::FsMkdtemp, Builtin::FspMkdtemp},   {Builtin::FsRmdir, Builtin::FspRmdir},
      {Builtin::FsUnlink, Builtin::FspUnlink},     {Builtin::FsRm, Builtin::FspRm},
      {Builtin::FsRename, Builtin::FspRename},     {Builtin::FsCopyFile, Builtin::FspCopyFile},
      {Builtin::FsReplace, Builtin::FspReplace},   {Builtin::FsRealpath, Builtin::FspRealpath}};
  for (const auto &[sync, async] : waiting) {
    if (call->builtin() == sync) {
      const auto *error = this->fsErrors->ofCall(call);
      call->target = BuiltinCall{.builtin = async};
      call->type = Type::promise(Type::result(call->type, error));
      return call;
    }
  }
  auto message = ErrorTextFsPromises::notYet(functionName);
  this->state.getDiagnostics().error(loc, message);
  call->type = Type::error();

  return call;
}

}
