#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The functions of node:fs (docs/io.md).
class FsChecker {
public:
  explicit FsChecker(Sema &sema);

  Expr *checkFsCall(CallExpr *call, llvm::StringRef fn, llvm::SMLoc loc);
  // The properties of the literal of options at `call->args[index]` of the
  // function `fn`, which must be among `allowed`, by name; false after an
  // error.
  bool nodeOptions(CallExpr *call, size_t index, llvm::StringRef fn,
                   llvm::ArrayRef<llvm::StringRef> allowed, llvm::StringMap<Expr *> &options);
  // An encoding given to `fn`, which must be the literal "utf8"; false after
  // an error.
  // The encoding of a function of node:fs, a literal: its number (see
  // checkBufferEncoding), or null after an error. Only readFileSync and the
  // writes take the encodings of Buffer (`any`); the others give names in
  // utf8.
  Expr *checkEncoding(Expr *encoding, llvm::StringRef fn, bool any);

private:
  Sema &sema_;
};

} // namespace tz
