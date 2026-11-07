#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/types/utility_types/utility_types_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class UtilityTypes {
public:
  using ResultUtilityApplied = Result<const Type *, UtilityTypesResolveUtilityTypeError>;
  using ResultGenericIndexed = Result<Expr *, UtilityTypesCheckGenericIndexError>;

  explicit UtilityTypes(CheckState &owner);

  bool wantsLiteral(const Type *param) const;
  const Type *functionValueType(FuncSymbol *fn);
  ResultGenericIndexed checkGenericIndex(IndexExpr *index);
  ResultUtilityApplied resolveUtilityType(const TypeRef &ref);
  const Type *importedValueType(Module *from, llvm::StringRef name);

private:
  struct Utility {
    llvm::StringLiteral name;
    TypeOp op;
    int count;
  };

  static const Utility *utilityNamed(llvm::StringRef name);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  CheckState &state;
};

}
