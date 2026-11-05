#include "compiler/program/ast/ast_context/ast_context.h"

namespace tz {

AstContext::AstContext() {
  moduleFunction_ = make<FunctionDecl>(llvm::SMLoc(), "");
}

const FunctionDecl *AstContext::moduleFunction() const {
  return moduleFunction_;
}

int AstContext::functionCount() const {
  return functionCount_;
}

}
