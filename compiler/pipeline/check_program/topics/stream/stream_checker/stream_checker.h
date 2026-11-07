#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class StreamChecker {
public:
  explicit StreamChecker(CheckState &owner);

  Expr *checkStreamIntrinsic(CallExpr *call, NameExpr *name, const Type *typeArg);
  const Type *streamCallbackType();
  Expr *streamChunkArg(Expr *arg, const Type *chunk, bool eof, bool writing, llvm::StringRef method,
                       bool checked);
  Expr *checkStreamCall(CallExpr *call, MemberExpr *member, ClassDecl *cls);

private:
  static std::string utf8(const std::u16string &text);
  static bool isByteChunk(const Type *type);

  CheckState &state;
};

}
