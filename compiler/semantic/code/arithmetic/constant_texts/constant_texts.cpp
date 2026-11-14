#include "compiler/semantic/code/arithmetic/constant_texts/constant_texts.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/CheckedArithmetic.h"
#include <cstdio>
#include <string>

using llvm::dyn_cast;

namespace tz {

ConstantTexts::ConstantTexts(SemanticState &owner)
    : state(owner) {}

void ConstantTexts::constText(const ConstText &task) {
  Expr *checked = this->state.walk.tasks.takeChecked();
  const auto *type = this->constTextType(checked, task.expected);
  this->state.walk.tasks.pushChecked(checked);
  if (type != nullptr) {
    this->state.walk.tasks.pushTask(Convert{.to = type});
  }
}

std::optional<std::u16string> ConstantTexts::writtenText(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);

  if (literal != nullptr) {
    return literal->value;
  }
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

std::optional<std::vector<std::u16string>> ConstantTexts::choiceTexts(const Expr *expr) const {
  auto texts = std::vector<std::u16string>();
  for (const Expr *branch : ExprParts::choiceBranches(expr)) {
    auto text = this->writtenText(branch);
    if (!text.has_value()) {
      return std::nullopt;
    }
    if (!llvm::is_contained(texts, *text)) {
      texts.push_back(*text);
    }
  }

  return texts;
}

const Type *ConstantTexts::constTextType(const Expr *checked, const Type *expected) {
  auto *name = dyn_cast<NameExpr>(checked);
  bool isTextConstant = name != nullptr && name->var() != nullptr && name->var()->literalConst &&
                        checked->type->isString();
  if (!isTextConstant) {
    return nullptr;
  }
  const auto *written = this->state.code.constants.findConstLiteral(name->var());
  if (written == nullptr) {
    return nullptr;
  }
  auto texts = this->choiceTexts(written);
  if (!texts.has_value()) {
    return nullptr;
  }
  auto literals = std::vector<const Type *>();
  for (const std::u16string &text : *texts) {
    const Type *literal = this->textLiteralFor(expected, text);
    if (literal == nullptr) {
      return nullptr;
    }
    literals.push_back(literal);
  }

  return Type::unionOf(literals);
}

const Type *ConstantTexts::textLiteralFor(const Type *expected, const std::u16string &text) {
  if (expected == nullptr) {
    return Type::stringLiteral(text);
  }

  return this->state.types.literals.stringLiteralFor(expected, text);
}

}
