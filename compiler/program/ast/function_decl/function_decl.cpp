#include "compiler/program/ast/function_decl/function_decl.h"

namespace tz {

bool FunctionDecl::hasThisParam() const {
  return sym && sym->hasThis && !params.empty() && params[0].sym;
}

const FunctionDecl *FunctionDecl::thisSource() const {
  const FunctionDecl *function = this;
  while (function && function->isArrow && !function->takesThis()) {
    function = function->parent;
  }

  return function;
}

}
