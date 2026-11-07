#include "compiler/pipeline/check_program/topics/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_checker/fs_checker.h"
#include "compiler/pipeline/check_program/topics/fs/fs_error_types/fs_error_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

namespace tz {

FsPromisesChecker::FsPromisesChecker(CheckState &owner)
    : state(owner) {}

FsPromisesChecker::ResultPromiseFunction FsPromisesChecker::promiseFunction(llvm::StringRef fn) {
  if (fn == "constants") {
    std::string message = ErrorTextFsPromises::constants();

    return err(FsPromisesCheckerPromiseFunctionError{
        .code = FsPromisesCheckerPromiseFunctionErrorCode::NOT_A_FUNCTION,
        .message = message,
    });
  }
  if (fn == "open") {
    std::string message = ErrorTextFsPromises::open();

    return err(FsPromisesCheckerPromiseFunctionError{
        .code = FsPromisesCheckerPromiseFunctionErrorCode::NO_FILE_HANDLE,
        .message = message,
    });
  }

  return ok(fn);
}

Expr *FsPromisesChecker::checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  this->state.getProgramUse().markUsesAsync();
  call->type = Type::error();
  ResultPromiseFunction function = FsPromisesChecker::promiseFunction(fn);
  if (function.isErr()) {
    this->state.getDiagnostics().error(loc, function.error().message);
    for (Expr *&arg : call->args) {
      arg = ExprChecker(this->state).checkExpr(arg);
    }
    return call;
  }
  FileSystemStore &files = this->state.getFileSystemStore();
  files.enterPromiseCall();
  FsChecker(this->state).checkFsCall(call, fn, loc);
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
      const Type *error = FsErrorTypes(this->state).ofCall(call);
      call->target = BuiltinCall{.builtin = async};
      call->type = Type::promise(Type::result(call->type, error));
      return call;
    }
  }
  std::string message = ErrorTextFsPromises::notYet(fn);
  this->state.getDiagnostics().error(loc, message);
  call->type = Type::error();

  return call;
}

}
