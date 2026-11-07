#include "compiler/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "compiler/library/fs/fs_error_types/fs_error_types.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"


namespace tz {

FsPromisesChecker::FsPromisesChecker(Checker &owner)
    : checker(owner) {}

Expr *FsPromisesChecker::checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  this->checker.file_->usesAsync = true;
  call->type = Type::error();
  if (fn == "constants") {
    this->checker.diags_.error(loc, "'constants' of 'node:fs/promises' is not a function");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (fn == "open") {
    this->checker.diags_.error(
        loc, "'open' of 'node:fs/promises' is not supported yet: there is no FileHandle; "
             "use 'readFile', 'writeFile' or 'appendFile', which read and write "
             "Buffers too");
    for (Expr *&arg : call->args) {
      arg = this->checker.exprChecker().checkExpr(arg);
    }
    return call;
  }
  {
    llvm::SaveAndRestore<bool> async(this->checker.fsAsync_, true);
    this->checker.fsChecker().checkFsCall(call, fn, loc);
  }
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
      const Type *error = this->checker.fsErrorTypes().ofCall(call);
      call->target = BuiltinCall{.builtin = async};
      call->type = Type::promise(Type::result(call->type, error));
      return call;
    }
  }
  this->checker.diags_.error(loc, "'" + fn + "' of 'node:fs/promises' is not supported yet");
  call->type = Type::error();

  return call;
}

}
