#include "compiler/library/fs/fs_promises_checker/fs_promises_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/library/fs/fs_checker/fs_checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/SaveAndRestore.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;
using llvm::dyn_cast_or_null;

namespace tz {

FsPromisesChecker::FsPromisesChecker(Sema &sema) : sema_(sema) {}

Expr *FsPromisesChecker::checkFsPromisesCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc) {
  sema_.file_->usesAsync = true;
  call->type = Type::error();
  if (fn == "constants") {
    sema_.diags_.error(loc, "'constants' of 'node:fs/promises' is not a function");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  if (fn == "open") {
    sema_.diags_.error(loc, "'open' of 'node:fs/promises' is not supported yet: there is no FileHandle; "
                      "use 'readFile', 'writeFile' or 'appendFile', which read and write "
                      "Buffers too");
    for (Expr *&arg : call->args) {
      arg = sema_.exprChecker().checkExpr(arg);
    }
    return call;
  }
  {
    llvm::SaveAndRestore<bool> async(sema_.fsAsync_, true);
    sema_.fsChecker().checkFsCall(call, fn, loc);
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
      {Builtin::FsRealpath, Builtin::FspRealpath}};
  for (const auto &[sync, async] : waiting) {
    if (call->builtin == sync) {
      call->builtin = async;
      call->type = Type::promise(call->type);
      return call;
    }
  }
  sema_.diags_.error(loc, "'" + fn + "' of 'node:fs/promises' is not supported yet");
  call->type = Type::error();

  return call;
}

} // namespace tz
