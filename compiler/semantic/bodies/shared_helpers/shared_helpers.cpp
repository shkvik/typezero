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
    for (size_t index = 0; index < text.size() && !close; ++index) {
      depth += text[index] == '(' ? 1 : text[index] == ')' ? -1 : 0;
      if (depth == 0) {
        close = index;
      }
    }
    if (close != text.size() - 1) {
      break;
    }
    text = text.drop_front().drop_back().trim();
  }

  return text;
}

std::pair<llvm::StringRef, llvm::StringRef>
SharedHelpers::operandTexts(llvm::StringRef source, llvm::SMLoc op, size_t length) {
  const char *at = op.getPointer();
  if (source.empty() || at < source.begin() || at + length > source.end()) {
    return {};
  }
  auto before = llvm::StringRef(source.begin(), static_cast<size_t>(at - source.begin()));
  auto after = llvm::StringRef(at + length, static_cast<size_t>(source.end() - at - length));

  return {before.trim(), after.trim()};
}

const Type *SharedHelpers::literalType(const Expr *value) {
  auto *unary = dyn_cast<UnaryExpr>(value);
  if (unary != nullptr && unary->op == UnaryOp::Neg) {
    value = unary->operand;
  }
  auto *literal = dyn_cast<IntLitExpr>(value);
  if (literal != nullptr) {
    return literal->value < (uint64_t(1) << 63) ? Type::intTy() : Type::doubleTy();
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
