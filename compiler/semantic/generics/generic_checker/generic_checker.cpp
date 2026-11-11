#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/bodies/expr/concat_checker/concat_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

GenericChecker::GenericChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.getScopes();
  this->concatChecker = &all.getConcatChecker();
  this->typeRelations = &all.getTypeRelations();
}

const Type *GenericChecker::apparentType(const Type *type) {
  if (type->isParam() && type->constraint() && !type->isListParam()) {
    return type->constraint();
  }
  if (type->isLiteral()) {
    return type->literalBase();
  }
  if (type->isUnion() && llvm::all_of(type->members(), [&](const Type *m) {
        return m->isLiteral() && m->literalBase() == type->members().front()->literalBase();
      })) {
    return type->members().front()->literalBase();
  }
  if (type->isUnion() && llvm::any_of(type->members(), [](const Type *m) {
        return m->isParam() && m->constraint() && !m->isListParam();
      })) {
    std::vector<const Type *> members;
    for (const Type *m : type->members()) {
      bool bounded = m->isParam() && m->constraint() && !m->isListParam();
      const Type *seen = bounded ? m->constraint() : m;
      if (!seen->isObject() || seen->isClass()) {
        return type;
      }
      members.push_back(seen);
    }
    const Type *joined = Type::unionOf(members);

    return joined->isTagged() ? type : joined;
  }

  return type;
}

Expr *GenericChecker::apparent(Expr *expr) {
  const Type *type = GenericChecker::apparentType(expr->type);

  return type == expr->type ? expr : this->typeRelations->convert(expr, type);
}

FuncSymbol *GenericChecker::genericNamed(const NameExpr *name) const {
  ScopeEntry entry = this->scopes->lookup(name->name);

  return entry.func() && !entry.var() && entry.func()->isGeneric() ? entry.func() : nullptr;
}

const Type *GenericChecker::numberParam(const Type *param, const Type *only) const {
  const Type *number = this->state.getGenericStore().findNumberParam(param, only);
  if (!number) {
    const Type *constraint = only;
    for (const Type *t : only ? llvm::ArrayRef<const Type *>() : param->alternatives()) {
      if (t->isNumeric()) {
        constraint = constraint ? TypeRelations::commonNumericType(constraint, t) : t;
      }
    }
    Type *type = Type::param(param->name());
    type->setConstraint(constraint ? constraint : Type::doubleTy(), {});
    number = type;
    this->state.getGenericStore().rememberNumberParam(param, only, number);
  }

  return number;
}

const Type *GenericChecker::narrowedType(const VarSymbol *var) const {
  if (const Type *type = this->state.getFlowState().findNarrowed(var)) {
    return type;
  }

  return nullptr;
}

void GenericChecker::noteForced(const Expr *expr, const Type *to) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }
  this->state.getGenericStore().rememberForcedType(expr, to);
}

Expr *GenericChecker::forceConversion(Expr *expr, const Type *to) {
  const Type *type = expr->type;
  if (!type || type->isError() || type == to) {
    return expr;
  }
  if (to->isString()) {
    return this->concatChecker->toStringOperand(expr);
  }
  if (TypeRelations::isImplicitlyConvertible(type, to)) {
    return this->typeRelations->convert(expr, to);
  }

  return expr;
}

std::optional<bool> GenericChecker::constantCondition(const Expr *cond) const {
  std::vector<std::optional<bool>> answers;
  for (const Expr *node : ChildrenFirst::order(cond, GenericChecker::conditionParts)) {
    std::optional<bool> value = GenericChecker::nodeCondition(node, answers);
    answers.push_back(value);
  }

  return answers.back();
}

NodeChildren<const Expr *> GenericChecker::conditionParts(const Expr *cond) {
  if (auto *conv = dyn_cast<CastExpr>(cond); conv && GenericChecker::isBoolPass(conv)) {
    return {conv->operand};
  }
  if (auto *unary = dyn_cast<UnaryExpr>(cond); unary && unary->op == UnaryOp::Not) {
    return {unary->operand};
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  bool logic = bin != nullptr && (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr);
  if (logic) {
    return {bin->lhs, bin->rhs};
  }

  return {};
}

bool GenericChecker::isBoolPass(const CastExpr *conv) {
  return conv->isImplicit() && conv->type->isBool() && conv->operand->type->isBool();
}

std::optional<bool> GenericChecker::nodeCondition(const Expr *cond,
                                                  std::vector<std::optional<bool>> &answers) {
  auto *conv = dyn_cast<CastExpr>(cond);
  if (conv != nullptr && GenericChecker::isBoolPass(conv)) {
    return ChildrenFirst::takeAnswer(answers);
  }
  auto *lit = dyn_cast<BoolLitExpr>(cond);
  if (lit != nullptr) {
    return lit->value;
  }
  auto *unary = dyn_cast<UnaryExpr>(cond);
  if (unary != nullptr && unary->op == UnaryOp::Not) {
    std::optional<bool> value = ChildrenFirst::takeAnswer(answers);
    return value.has_value() ? std::optional<bool>(!*value) : std::nullopt;
  }
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::JsonIsJson) {
    return GenericChecker::isJsonCondition(call);
  }
  auto *bin = dyn_cast<BinaryExpr>(cond);
  if (bin == nullptr) {
    return std::nullopt;
  }
  if (bin->op == BinaryOp::LogAnd || bin->op == BinaryOp::LogOr) {
    return GenericChecker::logicalCondition(bin, answers);
  }
  if (bin->op != BinaryOp::Eq && bin->op != BinaryOp::Ne) {
    return std::nullopt;
  }
  std::optional<bool> undefinedCheck = GenericChecker::undefinedCondition(bin);
  if (undefinedCheck.has_value()) {
    return undefinedCheck;
  }

  return GenericChecker::typeOfCondition(bin);
}

std::optional<bool> GenericChecker::isJsonCondition(const CallExpr *call) {
  const Type *type = call->args[0]->type;
  if (type->isUnknown() || type->nonOptional()->isParam()) {
    return std::nullopt;
  }

  return type->nonOptional()->isJson();
}

std::optional<bool> GenericChecker::logicalCondition(const BinaryExpr *bin,
                                                     std::vector<std::optional<bool>> &answers) {
  std::optional<bool> r = ChildrenFirst::takeAnswer(answers);
  std::optional<bool> l = ChildrenFirst::takeAnswer(answers);
  bool isAnd = bin->op == BinaryOp::LogAnd;
  if (l.has_value() && *l != isAnd) {
    return l;
  }
  if (l.has_value()) {
    return r;
  }

  return std::nullopt;
}

std::optional<bool> GenericChecker::undefinedCondition(const BinaryExpr *bin) {
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    const Expr *operand = side;
    while (isa<CastExpr>(operand) && cast<CastExpr>(operand)->isImplicit()) {
      operand = cast<CastExpr>(operand)->operand;
    }
    const Type *type = operand->type;
    bool alwaysUndefined = type->isUndefined() || (type->isOptional() && type->inner()->isNever());
    if (isa<UndefinedExpr>(other) && isa<NameExpr>(operand) && alwaysUndefined) {
      return bin->op == BinaryOp::Eq;
    }
  }

  return std::nullopt;
}

std::optional<bool> GenericChecker::typeOfCondition(const BinaryExpr *bin) {
  for (auto [side, other] : {std::pair{bin->lhs, bin->rhs}, std::pair{bin->rhs, bin->lhs}}) {
    auto *typeOf = dyn_cast<UnaryExpr>(side);
    auto *text = dyn_cast<StringLitExpr>(other);
    if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
      continue;
    }
    const Expr *operand = typeOf->operand;
    while (isa<CastExpr>(operand) && cast<CastExpr>(operand)->isImplicit()) {
      operand = cast<CastExpr>(operand)->operand;
    }
    if (!isa<NameExpr>(operand)) {
      return std::nullopt;
    }
    return GenericChecker::typeOfAnswer(bin, typeOf->operand->type, text);
  }

  return std::nullopt;
}

std::optional<bool> GenericChecker::typeOfAnswer(const BinaryExpr *bin, const Type *type,
                                                 const StringLitExpr *text) {
  llvm::StringRef name = TypeSpelling::typeOfName(type->nonOptional());
  if (name.empty()) {
    return std::nullopt;
  }
  std::u16string wanted(name.begin(), name.end());
  bool equal = text->value == wanted;
  if (type->isOptional()) {
    if (text->value == u"undefined" || equal) {
      return std::nullopt;
    }
    equal = false;
  }

  return bin->op == BinaryOp::Eq ? equal : !equal;
}

const Type *GenericChecker::literalShape(const Expr *expr) {
  std::vector<const Type *> answers;
  for (const Expr *node : ChildrenFirst::order(expr, GenericChecker::shapeParts)) {
    const Type *shape = GenericChecker::nodeShape(node, answers);
    if (shape == nullptr) {
      return nullptr;
    }
    answers.push_back(shape);
  }

  return answers.back();
}

NodeChildren<const Expr *> GenericChecker::shapeParts(const Expr *expr) {
  if (SharedHelpers::literalType(expr) != nullptr) {
    return {};
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr); lit && !GenericChecker::hasGaps(lit)) {
    NodeChildren<const Expr *> parts;
    for (const ArrayLitExpr::Element &e : lit->elements) {
      parts.push_back(e.expr);
    }
    return parts;
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    NodeChildren<const Expr *> parts;
    for (const ObjectLitExpr::Property &p : lit->props) {
      parts.push_back(p.value);
    }
    return parts;
  }

  return {};
}

bool GenericChecker::hasGaps(const ArrayLitExpr *lit) {
  return llvm::any_of(lit->elements,
                      [](const ArrayLitExpr::Element &e) { return e.spread || e.hole; });
}

const Type *GenericChecker::nodeShape(const Expr *expr, std::vector<const Type *> &answers) {
  if (const Type *type = SharedHelpers::literalType(expr)) {
    return isa<TemplateExpr>(expr) ? nullptr : type;
  }
  if (isa<UndefinedExpr>(expr)) {
    return Type::undefinedTy();
  }
  if (auto *lit = dyn_cast<ArrayLitExpr>(expr)) {
    return GenericChecker::arrayShape(lit, answers);
  }
  if (auto *lit = dyn_cast<ObjectLitExpr>(expr)) {
    return GenericChecker::objectShape(lit, answers);
  }

  return nullptr;
}

const Type *GenericChecker::arrayShape(const ArrayLitExpr *lit,
                                       std::vector<const Type *> &answers) {
  if (GenericChecker::hasGaps(lit)) {
    return nullptr;
  }
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, lit->elements.size());
  const Type *element = nullptr;
  for (const Type *type : types) {
    element = element ? TypeRelations::unify(element, type) : type;
    if (!element) {
      return nullptr;
    }
  }

  return element && !element->isUndefined() ? Type::array(element) : nullptr;
}

const Type *GenericChecker::objectShape(const ObjectLitExpr *lit,
                                        std::vector<const Type *> &answers) {
  std::vector<const Type *> types = ChildrenFirst::takeAnswers(answers, lit->props.size());
  std::vector<Field> fields;
  for (size_t i = 0; i < types.size(); ++i) {
    const ObjectLitExpr::Property &p = lit->props[i];
    bool repeated = llvm::any_of(fields, [&](const Field &f) { return f.name == p.name; });
    if (types[i]->isUndefined() || repeated) {
      return nullptr;
    }
    fields.push_back({p.name.str(), types[i], false});
  }

  return Type::object(std::move(fields));
}

}
