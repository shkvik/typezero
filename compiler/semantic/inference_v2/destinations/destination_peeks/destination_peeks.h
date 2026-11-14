#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/inference_v2/destinations/destination_search/destination_search.h"
#include <vector>

namespace tz {

class SemanticState;

class DestinationPeeks {
public:
  explicit DestinationPeeks(SemanticState &owner);

  const Type *peekType(const Expr *expr, const DestinationSearch &search) const;
  const FuncSymbol *soleConstructor(const NewExpr *create, const DestinationSearch &search) const;
  std::vector<const Type *> calleeParams(const CallExpr *call,
                                         const DestinationSearch &search) const;

private:
  bool hasFunctionField(const Type *object, llvm::StringRef name) const;


  const Type *peekSubtle() const;
  const Type *elementPeekType(const Type *object) const;
  const Type *choicePeekType(std::vector<const Type *> &answers) const;
  const Type *memberPeekType(const MemberExpr *member, const Type *object) const;
  const Type *peekName(llvm::StringRef name, const DestinationSearch &search) const;
  const Type *namePeekType(const NameExpr *name, const DestinationSearch &search) const;
  const Type *resultHalf(const CallExpr *call, std::vector<const Type *> &answers) const;
  const Type *resultPeekType(const CallExpr *call, std::vector<const Type *> &answers) const;
  const Type *nodePeekType(const Expr *expr, const DestinationSearch &search,
                           std::vector<const Type *> &answers) const;


  std::vector<const Type *> functionTypeParams(const Type *type, size_t argCount) const;
  std::vector<const Type *> dictMethodParams(const Type *object, llvm::StringRef name) const;
  std::vector<const Type *> soleMethodParams(llvm::ArrayRef<FunctionDecl *> methods,
                                             size_t argCount) const;
  std::vector<const Type *> arrayMethodParams(const Type *array, llvm::StringRef name,
                                              size_t argCount) const;
  std::vector<const Type *> spreadParams(llvm::ArrayRef<const Type *> params, bool hasRest,
                                         size_t argCount) const;
  std::vector<const Type *> nameCalleeParams(const CallExpr *call, const NameExpr *name,
                                             const DestinationSearch &search) const;
  std::vector<const Type *> memberCalleeParams(const CallExpr *call, const MemberExpr *member,
                                               const DestinationSearch &search) const;


  NodeChildren<const Expr *> peekParts(const Expr *expr) const;
  const FuncSymbol *calledFunction(const ScopeEntry &entry) const;
  const ClassDecl *namedClass(const Expr *object, const DestinationSearch &search) const;

  SemanticState &state;
};

}
