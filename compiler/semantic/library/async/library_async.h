#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/async/library_async_registry_errors.h"
#include <string>

namespace tz {

class SemanticState;

class LibraryAsync {
public:
  using ResultPromiseHandled = Result<const Expr *, LibraryAsyncDiscardedPromiseError>;

  explicit LibraryAsync(SemanticState &owner);

  bool isMappingCall(const Expr *expr) const;
  bool isTakingElement(const Expr *expr) const;


  const Type *awaitedType(const Type *type) const;
  const Type *timerCallType(const Expr *init) const;


  ResultPromiseHandled checkDiscardedPromise(const Expr *expr, bool isCallback,
                                             llvm::SMLoc at) const;

private:
  bool hasPromiseMember(const Type *type) const;


  int depthChange(char symbol) const;
  std::string statementText(llvm::SMLoc at) const;
  const Type *awaitedPlain(const Type *type) const;

  SemanticState &state;
};

}
