#include "compiler/program/ast/name_expr/name_expr.h"

namespace tz {

VarSymbol *NameExpr::var() const {
  const NamedVar *named = this->binding.as<NamedVar>();

  return named ? named->var : nullptr;
}

FuncSymbol *NameExpr::func() const {
  const NamedFunction *named = this->binding.as<NamedFunction>();

  return named ? named->func : nullptr;
}

ClassDecl *NameExpr::cls() const {
  const NamedClass *named = this->binding.as<NamedClass>();

  return named ? named->cls : nullptr;
}

}
