#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <vector>

namespace tz {

class LiteralRules {
public:
  explicit LiteralRules(SemanticState &owner);

  void nameFunctionValue(ObjectLitExpr::Property &property) const;


  bool reportUnusableValue(const ObjectLitExpr::Property &property, const Type *type);


  ClassDecl *classOf(const Type *type) const;
  const Field *fieldNamed(const std::vector<Field> &fields, llvm::StringRef name) const;

private:
  using ResultUsableFieldValue = Result<const Type *, CodeLiteralsRegistryValueTypedError>;

  ResultUsableFieldValue checkValueTyped(const Type *type, llvm::StringRef what,
                                         llvm::StringRef name) const;

  SemanticState &state;
};

}
