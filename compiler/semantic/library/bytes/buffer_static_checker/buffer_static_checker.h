#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/bytes/buffer_static_checker/buffer_static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class BufferStaticChecker {
public:
  explicit BufferStaticChecker(SemanticState &owner);

  Expr *checkBytesFromWith(CallExpr *call, const Type *type, const std::string &what);
  Expr *checkBufferStatic(CallExpr *call, MemberExpr *member);
  Expr *checkBytesStatic(CallExpr *call, MemberExpr *member);
  Expr *checkEmbedBytes(CallExpr *call, NameExpr *name);

private:
  using ResultSourceElement = Result<const Type *, BufferStaticCheckerSourceElementError>;
  using ResultConcatSources = Result<const Type *, BufferStaticCheckerConcatSourcesError>;

  static ResultConcatSources concatSources(const Type *list, const Type *buffer);
  static ResultSourceElement sourceElement(const Type *from, llvm::StringRef what);
  static std::optional<std::u16string> literalText(const Expr *e);

  SemanticState &state;
};

}
