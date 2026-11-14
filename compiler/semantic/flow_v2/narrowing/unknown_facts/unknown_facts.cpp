#include "compiler/semantic/flow_v2/narrowing/unknown_facts/unknown_facts.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

UnknownFacts::UnknownFacts(SemanticState &owner)
    : state(owner) {}

const Type *UnknownFacts::instanceType(const Type *type) const {
  if (type != nullptr && type->isConstructor()) {
    return type->constructed();
  }

  return type;
}

const VarSymbol *UnknownFacts::checkedVariable(const Expr *expr) const {
  const Expr *bare = ExprParts::withoutImplicitCasts(expr);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->var() == nullptr || name->var()->type == nullptr) {
    return nullptr;
  }
  const VarSymbol *variable = name->var();
  bool isNarrowable = variable->type->isUnknown() || variable->type->nonOptional()->isAnyObject();
  if (!isNarrowable) {
    return nullptr;
  }

  return variable;
}

void UnknownFacts::addUnknownFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  auto *binary = dyn_cast<BinaryExpr>(cond);

  if (call != nullptr && call->builtin() == Builtin::ArrayIsArray) {
    const auto *unknownArray = Type::array(Type::unknown());

    return this->narrowChecked(call->args[0], unknownArray, whenTrue);
  }
  if (call != nullptr && call->builtin() == Builtin::JsonIsJson) {
    return this->narrowChecked(call->args[0], Type::json(), whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::InstanceOf) {
    return this->narrowByInstanceOf(binary, whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::Eq) {
    return this->narrowByTypeOf(binary, whenTrue);
  }
  if (binary != nullptr && binary->op == BinaryOp::Ne) {
    return this->narrowByTypeOf(binary, whenFalse);
  }
}

void UnknownFacts::narrowChecked(const Expr *checked, const Type *type, Flow &flow) const {
  const auto *variable = this->checkedVariable(checked);
  if (variable != nullptr) {
    this->replaceNarrowing(flow, variable, type);
  }
}

void UnknownFacts::narrowByInstanceOf(const BinaryExpr *binary, Flow &whenTrue) const {
  const Type *instance = this->instanceType(binary->rhs->type);
  bool isNarrowing = instance != nullptr && (instance->isObject() || instance->isParam() ||
                                             instance->isDate() || instance->isBytes());
  if (isNarrowing) {
    this->narrowChecked(binary->lhs, instance, whenTrue);
  }
}

void UnknownFacts::narrowByTypeOf(const BinaryExpr *binary, Flow &equal) const {
  this->narrowTypeOfSide(binary->lhs, binary->rhs, equal);
  this->narrowTypeOfSide(binary->rhs, binary->lhs, equal);
}

void UnknownFacts::narrowTypeOfSide(const Expr *side, const Expr *other, Flow &equal) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  bool isTypeOfTest = typeOf != nullptr && typeOf->op == UnaryOp::TypeOf && text != nullptr;
  if (!isTypeOfTest) {
    return;
  }
  const auto *variable = this->checkedVariable(typeOf->operand);
  if (variable == nullptr || !variable->type->isUnknown()) {
    return;
  }
  const auto *narrowed = TypeSpelling::narrowedByTypeOf(text->value);
  if (narrowed != nullptr) {
    this->replaceNarrowing(equal, variable, narrowed);
  }
}

void UnknownFacts::replaceNarrowing(Flow &flow, const VarSymbol *variable, const Type *type) const {
  auto kept = std::vector<std::pair<const VarSymbol *, const Type *>>();
  for (const auto &narrowing : flow.narrowed) {
    if (narrowing.first != variable) {
      kept.push_back(narrowing);
    }
  }
  kept.push_back({variable, type});
  flow.narrowed = kept;
}

}
