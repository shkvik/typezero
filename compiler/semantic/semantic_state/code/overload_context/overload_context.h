#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tz {

struct VariantChoice {
  FuncSymbol *function;
  bool isChecked;
  std::vector<const Type *> typeArgs;
  bool isReported;
  std::string message;
};

class OverloadContext {
public:
  void pushChoice(VariantChoice choice);
  void rememberSignatureVariants(const Type *interfaceType, llvm::StringRef name,
                                 std::vector<FunctionDecl *> variants);


  VariantChoice takeChoice();
  std::vector<FunctionDecl *> getSignatureVariants(const Type *interfaceType,
                                                   llvm::StringRef name) const;

private:
  std::vector<VariantChoice> choices;
  std::map<std::pair<const Type *, std::string>, std::vector<FunctionDecl *>> signatureVariants;
};

}
