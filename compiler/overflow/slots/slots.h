#pragma once

#include "compiler/overflow/overflow_check/overflow_check.h"

namespace tz {

// Where values are kept while a function runs: variables, fields and elements.
class Slots {
public:
  explicit Slots(OverflowCheck &check);

  bool mergeSlot(OverflowCheck::Slot &into, const OverflowCheck::Slot *from, const Type *type, bool narrowing);
  const Reason *paramReason(const FunctionDecl *fn, size_t index);
  Val readVar(const VarSymbol *var, const State &st);
  void writeVar(const VarSymbol *var, Val v, State &st);
  Val readField(const Type *objectType, llvm::StringRef field, const Type *type);
  void storeField(const Type *objectType, llvm::StringRef field, const Type *type, const Val &v);
  Val elementsOf(const Expr *arrayExpr, const Val &array, const Type *arrayType);
  void storeElements(const Expr *arrayExpr, const Type *arrayType, const Val &v, State &st);
  bool isPrivateArray(const Expr *expr) const;

private:
  OverflowCheck &check_;
};

} // namespace tz
