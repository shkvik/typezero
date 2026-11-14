#include "compiler/semantic/flow_v2/narrowing/variable_facts/variable_facts.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::dyn_cast;

namespace tz {

VariableFacts::VariableFacts(SemanticState &owner)
    : state(owner) {}

const VarSymbol *VariableFacts::variableOf(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  bool isOptionalWrap =
      conversion != nullptr && conversion->isImplicit() && conversion->operand->type->isOptional();
  const Expr *inner = isOptionalWrap ? conversion->operand : expr;
  auto *name = dyn_cast<NameExpr>(inner);

  return name != nullptr ? name->var() : nullptr;
}

void VariableFacts::addVariableInstanceOfFacts(const BinaryExpr *binary, Flow &whenTrue) const {
  this->state.flow.defined.defineOptional(binary->lhs, whenTrue);
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

const Type *VariableFacts::instanceType(const Type *type) const {
  if (type != nullptr && type->isConstructor()) {
    return type->constructed();
  }

  return type;
}

bool VariableFacts::isSubclassOf(const Type *classType, const Type *own) const {
  bool isObjects = classType->isClass() && own->isObject();

  return isObjects && classType != own && classType->isSubtypeOf(own);
}

void VariableFacts::addVariableTypeOfFacts(const BinaryExpr *binary, Flow &whenTrue,
                                           Flow &whenFalse) const {
  Flow &whenEqual = binary->op == BinaryOp::Eq ? whenTrue : whenFalse;
  Flow &whenUnequal = binary->op == BinaryOp::Eq ? whenFalse : whenTrue;
  this->addSideTypeOfFacts(binary->lhs, binary->rhs, whenEqual, whenUnequal);
  this->addSideTypeOfFacts(binary->rhs, binary->lhs, whenEqual, whenUnequal);
}

void VariableFacts::addSideTypeOfFacts(const Expr *side, const Expr *other, Flow &whenEqual,
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
  const auto *name = this->state.flow.defined.optionalVariable(typeOf->operand);
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

void VariableFacts::addParamTypeOfFacts(const VarSymbol *variable, const std::u16string &text,
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

const Type *VariableFacts::paramTypeOfNarrowed(const Type *param,
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

bool VariableFacts::canBe(const Type *param, bool (Type::*test)() const) const {
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

const Type *VariableFacts::numberNamed(const Type *param, llvm::StringRef name) const {
  for (const Type *type : param->alternatives()) {
    if (type->isNumeric() && name == TypeSpelling::typeOfName(type)) {
      return this->numberParam(param, type);
    }
  }
  auto wideName = ToUtf16::toUtf16(name);
  const auto *named = TypeSpelling::narrowedByTypeOf(wideName);
  bool isNamedNumber = param->alternatives().empty() && named != nullptr && named->isNumeric();
  if (!isNamedNumber) {
    return nullptr;
  }

  return this->numberParam(param, named);
}

const Type *VariableFacts::otherAlternatives(const Type *param, const std::u16string &text) const {
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

llvm::SmallVector<const Type *, 4> VariableFacts::alternativesNotNamed(const Type *param,
                                                                       llvm::StringRef name) const {
  auto others = llvm::SmallVector<const Type *, 4>();
  for (const Type *type : param->alternatives()) {
    if (TypeSpelling::typeOfName(type) != name) {
      others.push_back(type);
    }
  }

  return others;
}

bool VariableFacts::areAllNamed(llvm::ArrayRef<const Type *> types, llvm::StringRef name) const {
  for (const Type *type : types) {
    if (TypeSpelling::typeOfName(type) != name) {
      return false;
    }
  }

  return true;
}

const Type *VariableFacts::numberParam(const Type *param, const Type *only) const {
  const auto *known = this->state.generics.parameters.findNumberParam(param, only);
  if (known != nullptr) {
    return known;
  }
  const Type *constraint = only != nullptr ? only : this->commonNumericAlternative(param);
  auto *type = Type::param(param->name());
  type->setConstraint(constraint != nullptr ? constraint : Type::doubleTy(), {});
  this->state.generics.parameters.rememberNumberParam(param, only, type);

  return type;
}

const Type *VariableFacts::commonNumericAlternative(const Type *param) const {
  const Type *common = nullptr;
  for (const Type *candidate : param->alternatives()) {
    if (candidate->isNumeric()) {
      common = common != nullptr ? this->state.types.relations.commonNumericType(common, candidate)
                                 : candidate;
    }
  }

  return common;
}

}
