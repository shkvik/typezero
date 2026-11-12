#include "compiler/semantic/flow/condition_facts/condition_facts.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/library/json/json_checker/json_checker.h"
#include "compiler/semantic/library/result/result_checker/result_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/unknown_checker/unknown_checker.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::isa;

namespace tz {

ConditionFacts::ConditionFacts(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->fieldFacts = &all.fieldFacts;
  this->indexProofs = &all.indexProofs;
  this->jsonChecker = &all.jsonChecker;
  this->resultChecker = &all.resultChecker;
  this->genericChecker = &all.genericChecker;
  this->unionNarrowing = &all.unionNarrowing;
  this->unknownChecker = &all.unknownChecker;
  this->predicateChecker = &all.predicateChecker;
}

const VarSymbol *ConditionFacts::variableOf(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isOptionalWrap =
      conversion != nullptr && conversion->isImplicit() && conversion->operand->type->isOptional();
  const Expr *inner = isOptionalWrap ? conversion->operand : expr;
  auto *name = dyn_cast<NameExpr>(inner);

  return name != nullptr ? name->var() : nullptr;
}

const NameExpr *ConditionFacts::optionalVariable(const Expr *expr) const {
  auto *assign = dyn_cast<AssignExpr>(expr);
  bool isPlainAssign = assign != nullptr && !assign->op.has_value();
  const Expr *target = isPlainAssign ? assign->target : expr;
  auto *name = dyn_cast<NameExpr>(target);
  bool isOptional = name != nullptr && name->var() != nullptr && name->var()->type->isOptional();

  return isOptional ? name : nullptr;
}

void ConditionFacts::addLess(Facts &facts, const Linear &smaller, const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset);
}

void ConditionFacts::addAtMost(Facts &facts, const Linear &smaller, const Linear &larger) const {
  facts.add(smaller.term, larger.term, larger.offset - smaller.offset + 1);
}

bool ConditionFacts::isOptionalChain(const Expr *expr) const {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);

  return isa<OptionalChainExpr>(bare);
}

void ConditionFacts::defineChain(const Expr *expr, Flow &flow) const {
  const auto *bare = ExprParts::withoutImplicitCasts(expr);
  auto *chain = dyn_cast<OptionalChainExpr>(bare);
  if (chain == nullptr) {
    return;
  }
  auto whole = this->definedPath(chain->chain, flow);
  if (whole.has_value() && !whole->names.empty()) {
    this->fieldFacts->defineField(flow, *whole);
  }
}

std::optional<FieldPath> ConditionFacts::definedPath(const Expr *expr, Flow &flow) const {
  auto links = std::vector<const Expr *>();
  const Expr *root = expr;
  const auto *inner = this->pathInner(root);
  while (inner != nullptr) {
    links.push_back(root);
    root = inner;
    inner = this->pathInner(root);
  }
  auto path = this->pathRoot(root);
  if (!path.has_value()) {
    return std::nullopt;
  }
  for (const Expr *link : llvm::reverse(links)) {
    this->extendPath(link, *path, flow);
  }

  return path;
}

const Expr *ConditionFacts::pathInner(const Expr *expr) const {
  auto *link = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  if (link != nullptr) {
    bool isPassing = link->shortCircuit || link->isImplicit();

    return isPassing ? link->operand : nullptr;
  }
  if (member != nullptr) {
    return member->object;
  }

  return nullptr;
}

std::optional<FieldPath> ConditionFacts::pathRoot(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  if (name == nullptr || name->var() == nullptr) {
    return std::nullopt;
  }

  return FieldPath{.var = name->var()};
}

void ConditionFacts::extendPath(const Expr *link, FieldPath &path, Flow &flow) const {
  auto *member = dyn_cast<MemberExpr>(link);
  if (member != nullptr) {
    path.names.push_back(member->name);
    return;
  }
  bool isChecked = cast<CastExpr>(link)->shortCircuit;
  if (isChecked && path.names.empty()) {
    flow.defined.insert(path.var);
    return;
  }
  if (isChecked) {
    this->fieldFacts->defineField(flow, path);
  }
}

void ConditionFacts::addConditionFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  this->resultChecker->addResultFacts(cond, whenTrue, whenFalse);
  this->jsonChecker->addJsonFacts(cond, whenTrue, whenFalse);
  this->unionNarrowing->addUnionFacts(cond, whenTrue, whenFalse);
  this->unknownChecker->addUnknownFacts(cond, whenTrue, whenFalse);
  this->addOwnFacts(cond, whenTrue, whenFalse);
}

void ConditionFacts::addOwnFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call != nullptr && call->builtin() == Builtin::None) {
    this->predicateChecker->applyPredicate(call, whenTrue, whenFalse);
    return;
  }
  const auto *name = this->optionalVariable(cond);
  if (name != nullptr) {
    whenTrue.defined.insert(name->var());
    return;
  }
  auto path = this->optionalField(cond);
  if (path.has_value()) {
    this->fieldFacts->defineField(whenTrue, *path);
    return;
  }
  if (this->isOptionalChain(cond)) {
    this->defineChain(cond, whenTrue);
    return;
  }
  auto *binary = dyn_cast<BinaryExpr>(cond);
  if (binary != nullptr) {
    this->addBinaryFacts(binary, whenTrue, whenFalse);
  }
}

std::optional<FieldPath> ConditionFacts::optionalField(const Expr *expr) const {
  if (expr->type == nullptr || !expr->type->isOptional()) {
    return std::nullopt;
  }

  return this->fieldFacts->fieldPath(expr);
}

void ConditionFacts::defineOptional(const Expr *expr, Flow &flow) const {
  const auto *name = this->optionalVariable(expr);
  if (name != nullptr) {
    flow.defined.insert(name->var());
    return;
  }
  auto path = this->optionalField(expr);
  if (path.has_value()) {
    this->fieldFacts->defineField(flow, *path);
  }
}

void ConditionFacts::addBinaryFacts(const BinaryExpr *binary, Flow &whenTrue,
                                    Flow &whenFalse) const {
  BinaryOp op = binary->op;
  bool isEquality = op == BinaryOp::Eq || op == BinaryOp::Ne;
  bool isUndefinedTest =
      isEquality && (isa<UndefinedExpr>(binary->lhs) || isa<UndefinedExpr>(binary->rhs));
  bool isComparison = isEquality || op == BinaryOp::Lt || op == BinaryOp::Le ||
                      op == BinaryOp::Gt || op == BinaryOp::Ge;
  if (isEquality) {
    this->addChainFacts(binary, whenTrue, whenFalse);
  }
  if (op == BinaryOp::InstanceOf) {
    this->addInstanceOfFacts(binary, whenTrue);
    return;
  }
  if (isUndefinedTest) {
    this->addUndefinedFacts(binary, whenTrue, whenFalse);
    return;
  }
  if (isEquality) {
    this->addConvertedFacts(binary, whenTrue, whenFalse);
    this->addTypeOfFacts(binary, whenTrue, whenFalse);
  }
  if (isComparison) {
    this->addLinearFacts(binary, whenTrue, whenFalse);
  }
}

const Expr *ConditionFacts::withoutPlainCasts(const Expr *expr) const {
  const Expr *given = expr;
  auto *conversion = dyn_cast<CastExpr>(given);
  while (conversion != nullptr && conversion->isImplicit() && !conversion->shortCircuit) {
    given = conversion->operand;
    conversion = dyn_cast<CastExpr>(given);
  }

  return given;
}

bool ConditionFacts::isDefinedValue(const Type *type) const {
  return type != nullptr && !type->isOptional() && !type->isUndefined();
}

void ConditionFacts::addChainFacts(const BinaryExpr *binary, Flow &whenTrue,
                                   Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Ne ? whenTrue : whenFalse;
  this->addSideChainFacts(binary->lhs, binary->rhs, whenEqual, whenUnequal);
  this->addSideChainFacts(binary->rhs, binary->lhs, whenEqual, whenUnequal);
}

void ConditionFacts::addSideChainFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                                       Flow &whenUnequal) const {
  const auto *given = this->withoutPlainCasts(other);
  bool isUndefined = isa<UndefinedExpr>(given);
  if (isUndefined) {
    this->defineChain(side, whenUnequal);
    return;
  }
  if (this->isDefinedValue(given->type)) {
    this->defineChain(side, whenEqual);
  }
}

void ConditionFacts::addInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const {
  this->defineOptional(binary->lhs, whenTrue);
  const auto *classType = this->instanceType(binary->rhs->type);
  const auto *variable = this->variableOf(binary->lhs);
  if (variable == nullptr || classType == nullptr) {
    return;
  }
  const Type *own = variable->type->nonOptional();
  if (this->isSubclassOf(classType, own)) {
    whenTrue.narrowed.push_back({variable, classType});
  }
  if (classType->isBuffer() && own->bytes() == Type::Bytes::Uint8) {
    whenTrue.narrowed.push_back({variable, classType});
  }
}

const Type *ConditionFacts::instanceType(const Type *type) const {
  if (type != nullptr && type->isConstructor()) {
    return type->constructed();
  }

  return type;
}

bool ConditionFacts::isSubclassOf(const Type *classType, const Type *own) const {
  bool isObjects = classType->isClass() && own->isObject();

  return isObjects && classType != own && classType->isSubtypeOf(own);
}

void ConditionFacts::addUndefinedFacts(const BinaryExpr *binary, Flow &whenTrue,
                                       Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Ne ? whenTrue : whenFalse;
  const Expr *side = isa<UndefinedExpr>(binary->rhs) ? binary->lhs : binary->rhs;
  const auto *name = this->optionalVariable(side);
  if (name != nullptr) {
    whenUnequal.defined.insert(name->var());
    whenEqual.undefinedVars.insert(name->var());
    return;
  }
  auto path = this->optionalField(side);
  if (path.has_value()) {
    this->fieldFacts->defineField(whenUnequal, *path);
  }
}

void ConditionFacts::addConvertedFacts(const BinaryExpr *binary, Flow &whenTrue,
                                       Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  this->addSideConvertedFacts(binary->lhs, binary->rhs, whenEqual);
  this->addSideConvertedFacts(binary->rhs, binary->lhs, whenEqual);
}

void ConditionFacts::addSideConvertedFacts(const Expr *side, const Expr *other,
                                           Flow &whenEqual) const {
  auto *conversion = dyn_cast<CastExpr>(other);
  const auto *name = this->optionalVariable(side);
  bool isPlainValue = name != nullptr && conversion != nullptr && conversion->isImplicit() &&
                      this->isDefinedValue(conversion->operand->type);
  if (isPlainValue) {
    whenEqual.defined.insert(name->var());
  }
}

void ConditionFacts::addTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue,
                                    Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  this->addSideTypeOfFacts(binary->lhs, binary->rhs, whenEqual, whenUnequal);
  this->addSideTypeOfFacts(binary->rhs, binary->lhs, whenEqual, whenUnequal);
}

void ConditionFacts::addSideTypeOfFacts(const Expr *side, const Expr *other, Flow &whenEqual,
                                        Flow &whenUnequal) const {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
    return;
  }
  const auto *variable = this->variableOf(typeOf->operand);
  bool isParam = variable != nullptr && variable->type->nonOptional()->isParam();
  if (isParam) {
    this->addParamTypeOfFacts(variable, text->value, whenEqual, whenUnequal);
    return;
  }
  const auto *name = this->optionalVariable(typeOf->operand);
  if (name == nullptr) {
    return;
  }
  auto wanted = ToUtf8::toUtf8(text->value);
  auto wantedName = TypeOfNameTable::find(wanted);
  if (wantedName == TypeOfName::UNDEFINED_TYPE) {
    whenUnequal.defined.insert(name->var());
    whenEqual.undefinedVars.insert(name->var());
    return;
  }
  whenEqual.defined.insert(name->var());
}

void ConditionFacts::addParamTypeOfFacts(const VarSymbol *variable, const std::u16string &text,
                                         Flow &holds, Flow &differs) const {
  const Type *param = variable->type->nonOptional();
  const auto *narrowed = this->paramTypeOfNarrowed(param, text);
  bool isOptional = variable->type->isOptional();
  if (narrowed != nullptr) {
    holds.narrowed.push_back({variable, narrowed});
  }
  if (narrowed != nullptr && isOptional) {
    holds.defined.insert(variable);
  }
  if (isOptional) {
    return;
  }
  const auto *rest = this->otherAlternatives(param, text);
  if (rest != nullptr) {
    differs.narrowed.push_back({variable, rest});
  }
}

const Type *ConditionFacts::paramTypeOfNarrowed(const Type *param,
                                                const std::u16string &text) const {
  auto wanted = ToUtf8::toUtf8(text);
  auto wantedName = TypeOfNameTable::find(wanted);
  if (wantedName == TypeOfName::STRING && this->canBe(param, &Type::isString)) {
    return Type::string();
  }
  if (wantedName == TypeOfName::BOOLEAN && this->canBe(param, &Type::isBool)) {
    return Type::boolTy();
  }
  if (param->constraint() == nullptr) {
    return this->numberNamed(param, wanted);
  }

  return nullptr;
}

bool ConditionFacts::canBe(const Type *param, bool (Type::*test)() const) const {
  const Type *constraint = param->constraint();
  if (constraint != nullptr) {
    const Type *bound = constraint->nonOptional();

    return (bound->*test)();
  }
  llvm::ArrayRef<const Type *> alternatives = param->alternatives();
  if (alternatives.empty()) {
    return true;
  }
  for (const Type *type : alternatives) {
    if ((type->*test)()) {
      return true;
    }
  }

  return false;
}

const Type *ConditionFacts::numberNamed(const Type *param, llvm::StringRef name) const {
  for (const Type *type : param->alternatives()) {
    if (type->isNumeric() && name == TypeSpelling::typeOfName(type)) {
      return this->genericChecker->numberParam(param, type);
    }
  }
  auto wideName = ToUtf16::toUtf16(name);
  const auto *named = TypeSpelling::narrowedByTypeOf(wideName);
  bool isNamedNumber = param->alternatives().empty() && named != nullptr && named->isNumeric();
  if (!isNamedNumber) {
    return nullptr;
  }

  return this->genericChecker->numberParam(param, named);
}

const Type *ConditionFacts::otherAlternatives(const Type *param, const std::u16string &text) const {
  auto wanted = ToUtf8::toUtf8(text);
  auto others = this->alternativesNotNamed(param, wanted);
  if (others.empty()) {
    return nullptr;
  }
  llvm::StringRef kind = TypeSpelling::typeOfName(others.front());
  if (!this->areAllNamed(others, kind)) {
    return nullptr;
  }
  auto kindName = TypeOfNameTable::find(kind);
  if (kindName == TypeOfName::STRING) {
    return Type::string();
  }
  if (kindName == TypeOfName::BOOLEAN) {
    return Type::boolTy();
  }

  return this->numberNamed(param, kind);
}

llvm::SmallVector<const Type *, 4>
ConditionFacts::alternativesNotNamed(const Type *param, llvm::StringRef name) const {
  auto others = llvm::SmallVector<const Type *, 4>();
  for (const Type *type : param->alternatives()) {
    if (TypeSpelling::typeOfName(type) != name) {
      others.push_back(type);
    }
  }

  return others;
}

bool ConditionFacts::areAllNamed(llvm::ArrayRef<const Type *> types, llvm::StringRef name) const {
  for (const Type *type : types) {
    if (TypeSpelling::typeOfName(type) != name) {
      return false;
    }
  }

  return true;
}

void ConditionFacts::addLinearFacts(const BinaryExpr *binary, Flow &whenTrue,
                                    Flow &whenFalse) const {
  BinaryOp op = binary->op;
  auto left = this->indexProofs->linear(binary->lhs);
  auto right = this->indexProofs->linear(binary->rhs);
  if (!left.has_value() || !right.has_value()) {
    return;
  }
  if (left->term.isZero() && right->term.isZero()) {
    bool isHolding = this->isConstantHolding(op, left->offset, right->offset);
    Flow &impossible = isHolding ? whenFalse : whenTrue;
    impossible.unreachable = true;
    return;
  }
  bool isEquality = op == BinaryOp::Ne || op == BinaryOp::Eq;
  bool hasConstant = left->term.isZero() || right->term.isZero();
  if (isEquality && hasConstant) {
    this->addDifferFacts(op, *left, *right, whenTrue.facts, whenFalse.facts);
  }
  this->addOrderFacts(op, *left, *right, whenTrue.facts, whenFalse.facts);
}

bool ConditionFacts::isConstantHolding(BinaryOp op, int64_t left, int64_t right) const {
  if (op == BinaryOp::Lt) {
    return left < right;
  }
  if (op == BinaryOp::Le) {
    return left <= right;
  }
  if (op == BinaryOp::Gt) {
    return left > right;
  }
  if (op == BinaryOp::Ge) {
    return left >= right;
  }
  if (op == BinaryOp::Eq) {
    return left == right;
  }

  return left != right;
}

void ConditionFacts::addDifferFacts(BinaryOp op, const Linear &left, const Linear &right,
                                    Facts &trueFacts, Facts &falseFacts) const {
  const Linear &varying = left.term.isZero() ? right : left;
  const Linear &constant = left.term.isZero() ? left : right;
  Facts &differ = op == BinaryOp::Ne ? trueFacts : falseFacts;
  const auto &known = this->state.getFlowState().getFacts();
  int64_t aboveRoom = varying.offset - constant.offset + 1;
  if (known.canProve(constant.term, varying.term, aboveRoom)) {
    this->addLess(differ, constant, varying);
    return;
  }
  int64_t belowRoom = constant.offset - varying.offset + 1;
  if (known.canProve(varying.term, constant.term, belowRoom)) {
    this->addLess(differ, varying, constant);
  }
}

void ConditionFacts::addOrderFacts(BinaryOp op, const Linear &left, const Linear &right,
                                   Facts &trueFacts, Facts &falseFacts) const {
  if (op == BinaryOp::Lt) {
    this->addLess(trueFacts, left, right);
    this->addAtMost(falseFacts, right, left);
    return;
  }
  if (op == BinaryOp::Le) {
    this->addAtMost(trueFacts, left, right);
    this->addLess(falseFacts, right, left);
    return;
  }
  if (op == BinaryOp::Gt) {
    this->addLess(trueFacts, right, left);
    this->addAtMost(falseFacts, left, right);
    return;
  }
  if (op == BinaryOp::Ge) {
    this->addAtMost(trueFacts, right, left);
    this->addLess(falseFacts, left, right);
    return;
  }
  if (op == BinaryOp::Eq) {
    this->addAtMost(trueFacts, left, right);
    this->addAtMost(trueFacts, right, left);
    return;
  }
  if (op == BinaryOp::Ne) {
    this->addAtMost(falseFacts, left, right);
    this->addAtMost(falseFacts, right, left);
  }
}

}
