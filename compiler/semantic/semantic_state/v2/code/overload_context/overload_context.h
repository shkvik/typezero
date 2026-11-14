#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace tz {

class OverloadContext {
public:
  void rememberSignatureVariants(const Type *interfaceType, llvm::StringRef name,
                                 std::vector<FunctionDecl *> variants);


  std::vector<FunctionDecl *> getSignatureVariants(const Type *interfaceType,
                                                   llvm::StringRef name) const;

private:
  std::map<std::pair<const Type *, std::string>, std::vector<FunctionDecl *>> signatureVariants;
};

}
