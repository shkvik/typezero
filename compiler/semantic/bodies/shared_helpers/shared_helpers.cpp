#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

llvm::StringRef SharedHelpers::withoutParens(llvm::StringRef text) {
  while (text.size() >= 2 && text.front() == '(' && text.back() == ')' &&
         text.find_first_of("\"'`") == llvm::StringRef::npos) {
    int depth = 0;
    size_t close = 0;
    for (size_t i = 0; i < text.size() && !close; ++i) {
      depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
      if (depth == 0) {
        close = i;
      }
    }
    if (close != text.size() - 1) {
      break;
    }
    text = text.drop_front().drop_back().trim();
  }

  return text;
}

const Type *SharedHelpers::literalType(const Expr *value) {
  if (auto *unary = dyn_cast<UnaryExpr>(value); unary && unary->op == UnaryOp::Neg) {
    value = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(value)) {
    return lit->value < (uint64_t(1) << 63) ? Type::intTy() : Type::doubleTy();
  }
  if (isa<FloatLitExpr>(value)) {
    return Type::doubleTy();
  }
  if (isa<StringLitExpr>(value) || isa<TemplateExpr>(value)) {
    return Type::string();
  }
  if (isa<BoolLitExpr>(value)) {
    return Type::boolTy();
  }

  return nullptr;
}

}
