#include "compiler/semantic/code/comparisons/equality_checks/equality_checks.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker_errors.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr size_t kMostShownNumberNames = 3;

}

EqualityChecks::EqualityChecks(SemanticState &owner, ComparisonRules &sharedComparisonRules)
    : state(owner),
      comparisonRules(sharedComparisonRules) {}

void EqualityChecks::checkEquality(const CheckEquality &task) {
  BinaryExpr *binary = task.binary;
  if (this->isNullName(binary->lhs) || this->isNullName(binary->rhs)) {
    return this->state.walk.tasks.pushTask(CheckNullEquality{.binary = binary});
  }
  this->state.walk.tasks.pushTask(AfterEqualityOperands{.binary = binary});

  return this->state.walk.tasks.pushTask(WalkComparisonOperands{.binary = binary});
}

bool EqualityChecks::isNullName(const Expr *expr) const {
  const auto *name = dyn_cast<NameExpr>(expr);

  return name != nullptr && KeywordTable::find(name->name) == Keyword::NULL_KEYWORD &&
         name->var() == nullptr && name->func() == nullptr;
}

void EqualityChecks::afterEqualityOperands(const AfterEqualityOperands &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  binary->type = Type::boolTy();
  this->walkTypeOfSide(binary->lhs, binary->rhs);
  this->walkTypeOfSide(binary->rhs, binary->lhs);
  if (leftType->isError() || rightType->isError()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->isVoid() || rightType->isVoid()) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = this->comparisonRules.operandTypesText(op, leftType, rightType);
    this->state.diagnostics.error(binary->loc(), message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->isUndefined() || rightType->isUndefined()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->nonOptional()->isJson() || rightType->nonOptional()->isJson()) {
    return this->state.walk.tasks.pushTask(CheckJsonEquality{.binary = binary});
  }
  auto plain = this->checkNoResults(binary);
  if (plain.isErr()) {
    this->state.diagnostics.error(binary->loc(), plain.error().message);
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (leftType->nonOptional()->isParam() || rightType->nonOptional()->isParam()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  this->state.walk.tasks.pushTask(FinishEqualOperands{.binary = binary});

  return this->comparisonRules.pushLiteralAdapting(binary);
}

void EqualityChecks::finishEqualOperands(const FinishEqualOperands &task) {
  BinaryExpr *binary = task.binary;
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isInteger() && rightType->isInteger()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  const auto *common = this->state.types.relations.comparisonType(leftType, rightType);
  if (common == nullptr) {
    this->state.walk.tasks.pushChecked(binary);
    return this->reportIncomparable(binary);
  }
  if (common->nonOptional()->isFunction()) {
    this->state.inventory.comparesFunctions = true;
  }
  this->state.walk.tasks.pushChecked(binary);
  this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->rhs, .to = common});

  return this->state.walk.tasks.pushTask(ConvertSlot{.slot = &binary->lhs, .to = common});
}

void EqualityChecks::reportIncomparable(BinaryExpr *binary) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->nonOptional()->isDecimal() || rightType->nonOptional()->isDecimal()) {
    return this->state.walk.tasks.pushTask(ReportDecimalMix{.binary = binary});
  }
  bool hasNumbers = leftType->nonOptional()->isNumeric() && rightType->nonOptional()->isNumeric() &&
                    !leftType->isOptional() && !rightType->isOptional();
  const char *op = Operators::opSpelling(binary->op);
  std::string message = hasNumbers
                            ? this->comparisonRules.mixedNumbersText(binary)
                            : this->comparisonRules.operandTypesText(op, leftType, rightType);

  return this->state.diagnostics.error(binary->loc(), message);
}

void EqualityChecks::walkTypeOfName(const WalkTypeOfName &task) {
  auto known = this->checkTypeOfName(task.operand, task.text);
  if (known.isErr() &&
      known.error().code != CodeComparisonsRegistryCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(task.text->loc(), known.error().message);
  }
}

std::string EqualityChecks::quotedNames(llvm::ArrayRef<llvm::StringRef> names) const {
  auto list = std::string();
  for (size_t index = 0; index < names.size(); ++index) {
    list += this->nameSeparator(index, names.size());
    list += "\"" + names[index].str() + "\"";
  }

  return list;
}

const char *EqualityChecks::nameSeparator(size_t index, size_t count) const {
  if (index == 0) {
    return "";
  }
  if (index + 1 == count) {
    return " or ";
  }

  return ", ";
}

EqualityChecks::ResultTypeOfNameKnown EqualityChecks::checkTypeOfName(const Expr *operand,
                                                                      const StringLitExpr *text) {
  auto name = ToUtf8::toUtf8(text->value);
  auto typeOf = TypeOfNameTable::find(name);
  if (typeOf == TypeOfName::BIGINT) {
    auto what = ErrorTextTypeOfName::bigint();
    auto bigint = ErrorTextBigInt::none(what);
    this->state.diagnostics.error(text->loc(), bigint);

    return err(CodeComparisonsRegistryCheckTypeOfNameError{
        .code = CodeComparisonsRegistryCheckTypeOfNameErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto known = this->checkKnownTypeOfName(operand, text, name);
  if (known.isErr()) {
    return err(known.error());
  }

  return this->checkTypeOfNameMatches(operand, text, name);
}

EqualityChecks::ResultTypeOfNameKnown
EqualityChecks::checkKnownTypeOfName(const Expr *operand, const StringLitExpr *text,
                                     const std::string &name) const {
  if (TypeSpelling::isTypeOfName(name)) {
    return ok(text);
  }
  auto typeOf = TypeOfNameTable::find(name);
  if (typeOf == TypeOfName::NUMBER) {
    auto message = this->noNumberText(operand);

    return err(CodeComparisonsRegistryCheckTypeOfNameError{
        .code = CodeComparisonsRegistryCheckTypeOfNameErrorCode::NO_NUMBER,
        .message = message,
    });
  }
  std::vector<llvm::StringRef> all(std::begin(kTypeOfNames), std::end(kTypeOfNames));
  auto list = this->quotedNames(all);
  if (typeOf == TypeOfName::SYMBOL) {
    auto message = ErrorTextTypeOfName::noSymbol(list);

    return err(CodeComparisonsRegistryCheckTypeOfNameError{
        .code = CodeComparisonsRegistryCheckTypeOfNameErrorCode::NO_SYMBOL,
        .message = message,
    });
  }
  auto message = ErrorTextTypeOfName::notName(name, list);

  return err(CodeComparisonsRegistryCheckTypeOfNameError{
      .code = CodeComparisonsRegistryCheckTypeOfNameErrorCode::NOT_A_NAME,
      .message = message,
  });
}

std::string EqualityChecks::noNumberText(const Expr *operand) const {
  auto here = this->numberNamesHere(operand);

  return ErrorTextTypeOfName::noNumber(here);
}

std::string EqualityChecks::numberNamesHere(const Expr *operand) const {
  auto numbers = this->numberTypeOfNames(operand->type);
  if (numbers.empty() || numbers.size() > kMostShownNumberNames) {
    return "";
  }
  auto quoted = this->quotedNames(numbers);

  return ErrorTextTypeOfName::forType(operand->type->name(), quoted);
}

std::vector<llvm::StringRef> EqualityChecks::numberTypeOfNames(const Type *type) const {
  auto numbers = std::vector<llvm::StringRef>();
  if (type == nullptr || type->isError()) {
    return numbers;
  }
  auto names = TypeSpelling::typeOfNames(type);
  for (llvm::StringRef possible : names) {
    if (TypeSpelling::isNumberTypeOfName(possible)) {
      numbers.push_back(possible);
    }
  }

  return numbers;
}

EqualityChecks::ResultTypeOfNameKnown
EqualityChecks::checkTypeOfNameMatches(const Expr *operand, const StringLitExpr *text,
                                       const std::string &name) const {
  const Type *type = operand->type;
  bool isUnchecked = type == nullptr || type->isError() ||
                     this->state.generics.copies.isCheckingInstance() ||
                     type->nonOptional()->isParam();
  if (isUnchecked) {
    return ok(text);
  }
  auto names = TypeSpelling::typeOfNames(type);
  bool isMatching = names.empty() || llvm::is_contained(names, llvm::StringRef(name));
  if (!isMatching) {
    auto quoted = this->quotedNames(names);
    auto message = ErrorTextTypeOfName::never(type->name(), quoted, name);

    return err(CodeComparisonsRegistryCheckTypeOfNameError{
        .code = CodeComparisonsRegistryCheckTypeOfNameErrorCode::NEVER_MATCHES,
        .message = message,
    });
  }

  return ok(text);
}

EqualityChecks::ResultNoResultOperand
EqualityChecks::checkNoResults(const BinaryExpr *binary) const {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->nonOptional()->isResult() || rightType->nonOptional()->isResult()) {
    const char *op = Operators::opSpelling(binary->op);
    auto message = ErrorTextResultEquality::compared(op);

    return err(CodeComparisonsRegistryNoResultsError{
        .code = CodeComparisonsRegistryNoResultsErrorCode::RESULTS_COMPARED,
        .message = message,
    });
  }

  return ok(binary);
}

void EqualityChecks::walkTypeOfSide(const Expr *side, const Expr *other) {
  auto *typeOf = dyn_cast<UnaryExpr>(side);
  auto *text = dyn_cast<StringLitExpr>(other);
  if (typeOf == nullptr || typeOf->op != UnaryOp::TypeOf || text == nullptr) {
    return;
  }
  auto known = this->checkTypeOfName(typeOf->operand, text);
  if (known.isErr() &&
      known.error().code != CodeComparisonsRegistryCheckTypeOfNameErrorCode::ALREADY_REPORTED) {
    this->state.diagnostics.error(text->loc(), known.error().message);
  }
}

}
