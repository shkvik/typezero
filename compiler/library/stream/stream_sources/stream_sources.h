#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The streams of files and of the process, and Readable.from.
class StreamSources {
public:
  explicit StreamSources(Sema &sema);

  // fs.createReadStream(path, options?) and createWriteStream(), which
  // `decl` of library/stream/fs_stream.tz declares: an encoding gives a stream
  // of strings.
  Expr *checkFsStreamFunction(CallExpr *call, FunctionDecl *decl, llvm::SMLoc loc);
  // process.stdin, process.stdout or process.stderr read as a stream: the
  // stream of library/stream/fs_stream.tz; null if the program does not use
  // them as streams (the loader did not load it).
  Expr *processStream(MemberExpr *member);
  // What stands for process.stdin in a StreamView.
  VarSymbol *processStdinVar();
  // `Readable.from(iterable, options?)`.
  const Type *streamFromOptionsType();
  Expr *checkStreamFrom(CallExpr *call, MemberExpr *member);

private:
  Sema &sema_;
  VarSymbol *processStdin_ = nullptr;
};

} // namespace tz
