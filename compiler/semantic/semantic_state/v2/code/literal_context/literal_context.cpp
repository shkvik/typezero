#include "compiler/semantic/semantic_state/v2/code/literal_context/literal_context.h"

namespace tz {

LiteralContext::LiteralContext(const SourceFile &loaded)
    : file(loaded) {}

void LiteralContext::markPrechecked(const Expr *expr) {
  this->prechecked.insert(expr);
}

bool LiteralContext::isPrechecked(const Expr *expr) const {
  return this->prechecked.contains(expr);
}

void LiteralContext::forgetPrechecked(const Expr *expr) {
  this->prechecked.erase(expr);
}

void LiteralContext::markPairLiteral(const Expr *literal) {
  this->pairLiterals.insert(literal);
}

bool LiteralContext::isPairLiteral(const Expr *literal) const {
  return this->pairLiterals.contains(literal);
}

void LiteralContext::forgetPairLiteral(const Expr *literal) {
  this->pairLiterals.erase(literal);
}

void LiteralContext::addFieldName(llvm::StringRef name) {
  this->fieldNames.insert(name);
}

bool LiteralContext::hasFieldName(llvm::StringRef name) const {
  return this->fieldNames.contains(name) || this->file.fieldNames.contains(name);
}

}
