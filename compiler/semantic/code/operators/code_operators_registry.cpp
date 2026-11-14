#include "compiler/semantic/code/operators/code_operators_registry.h"
#include "compiler/program/library/builtin_names/type_of_name_table/type_of_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/program/types/type_spelling/type_spelling.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr const char *kUnnamedValue = "x";

}

CodeOperatorsRegistry::CodeOperatorsRegistry(SemanticState &owner)
    : state(owner) {}

void CodeOperatorsRegistry::checkBinary(const CheckBinary &task) {
  BinaryExpr *binary = task.binary;
  const Type *expected = task.expected != nullptr ? task.expected->nonOptional() : nullptr;
  auto &tasks = this->state.walk.tasks;
  switch (binary->op) {
    case BinaryOp::In:
      return tasks.pushTask(CheckInOperator{.binary = binary});
    case BinaryOp::InstanceOf:
      return tasks.pushTask(CheckInstanceOfOperator{.binary = binary});
    case BinaryOp::LogAnd:
    case BinaryOp::LogOr:
      return tasks.pushTask(CheckLogicalOperator{.binary = binary, .expected = expected});
    case BinaryOp::Nullish:
      return tasks.pushTask(StartNullish{.binary = binary, .expected = expected});
    case BinaryOp::Eq:
    case BinaryOp::Ne:
      return tasks.pushTask(CheckEquality{.binary = binary});
    case BinaryOp::Lt:
    case BinaryOp::Le:
    case BinaryOp::Gt:
    case BinaryOp::Ge:
      return tasks.pushTask(CheckComparison{.binary = binary});
    case BinaryOp::Shl:
    case BinaryOp::Shr:
    case BinaryOp::UShr:
      return tasks.pushTask(CheckShift{.binary = binary, .expected = expected});
    case BinaryOp::Add:
    case BinaryOp::Sub:
    case BinaryOp::Mul:
    case BinaryOp::Div:
    case BinaryOp::Rem:
    case BinaryOp::Pow:
    case BinaryOp::BitAnd:
    case BinaryOp::BitOr:
    case BinaryOp::BitXor:
      return tasks.pushTask(CheckArithmetic{.binary = binary, .expected = expected});
  }

  return tasks.pushChecked(binary);
}

void CodeOperatorsRegistry::checkUnary(const CheckUnary &task) {
  UnaryExpr *unary = task.unary;
  auto &tasks = this->state.walk.tasks;
  switch (unary->op) {
    case UnaryOp::Neg:
    case UnaryOp::Plus:
      return tasks.pushTask(CheckSign{.unary = unary, .expected = task.expected});
    case UnaryOp::Not:
      return tasks.pushTask(CheckNot{.unary = unary});
    case UnaryOp::BitNot:
      return tasks.pushTask(CheckBitNot{.unary = unary, .expected = task.expected});
    case UnaryOp::TypeOf:
      tasks.pushTask(FinishTypeOf{.unary = unary});
      return tasks.pushTask(CheckExpr{.expr = unary->operand, .expected = nullptr});
    case UnaryOp::Void:
      tasks.pushTask(FinishVoid{.unary = unary});
      return tasks.pushTask(CheckExpr{.expr = unary->operand, .expected = nullptr});
    case UnaryOp::Delete:
      return tasks.pushTask(CheckDeleteValue{.unary = unary});
    case UnaryOp::NonNull:
      tasks.pushTask(FinishNonNull{.unary = unary});
      return tasks.pushTask(CheckExpr{.expr = unary->operand, .expected = task.expected});
  }

  return tasks.pushChecked(unary);
}

void CodeOperatorsRegistry::finishTypeOf(const FinishTypeOf &task) {
  UnaryExpr *unary = task.unary;
  unary->operand = this->state.walk.tasks.takeChecked();
  unary->type = Type::string();

  return this->state.walk.tasks.pushChecked(unary);
}

void CodeOperatorsRegistry::finishVoid(const FinishVoid &task) {
  UnaryExpr *unary = task.unary;
  unary->operand = this->state.walk.tasks.takeChecked();
  unary->type = Type::undefinedTy();
  const Type *operandType = unary->operand->type;
  bool isPromised = operandType->isPromise();
  const Type *dropped = isPromised ? operandType->promised() : operandType;
  auto kept = this->checkResultKept(unary, dropped, isPromised);
  if (kept.isErr()) {
    this->state.diagnostics.error(unary->loc(), kept.error().message);
  }

  return this->state.walk.tasks.pushChecked(unary);
}

void CodeOperatorsRegistry::finishNonNull(const FinishNonNull &task) {
  UnaryExpr *unary = task.unary;
  unary->operand = this->state.walk.tasks.takeChecked();
  this->state.walk.tasks.pushChecked(unary);
  const Type *type = unary->operand->type;
  auto text = this->assertedText(unary->operand).value_or(kUnnamedValue);
  unary->type = type->isUndefined() ? Type::error() : type->nonOptional();
  if (type->isError()) {
    return;
  }
  auto asserted = this->checkNonNullOperand(type, text);
  if (asserted.isErr()) {
    this->state.diagnostics.error(unary->loc(), asserted.error().message);
  }
  bool isRefused =
      asserted.isErr() &&
      asserted.error().code != CodeOperatorsRegistryNonNullOperandErrorCode::NOT_SUPPORTED;
  if (isRefused) {
    return;
  }
  auto *name = dyn_cast<NameExpr>(unary->operand);
  if (name != nullptr && name->var() != nullptr) {
    this->state.walk.flow.markDefined(name->var());
  }
}

std::optional<std::string> CodeOperatorsRegistry::simpleText(const Expr *expr) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *literal = dyn_cast<IntLitExpr>(expr);
  if (name != nullptr) {
    return name->name.str();
  }
  if (literal != nullptr) {
    return std::to_string(literal->value);
  }

  return std::nullopt;
}

std::optional<std::string> CodeOperatorsRegistry::assertedText(const Expr *expr) const {
  auto answers = std::vector<std::string>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &CodeOperatorsRegistry::textParts)) {
    auto text = this->nodeText(node, answers);
    if (!text.has_value()) {
      return std::nullopt;
    }
    answers.push_back(*text);
  }

  return answers.back();
}

NodeChildren<const Expr *> CodeOperatorsRegistry::textParts(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  if (conversion != nullptr && conversion->isImplicit()) {
    return {conversion->operand};
  }
  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object, index->index};
  }

  return {};
}

std::optional<std::string>
CodeOperatorsRegistry::nodeText(const Expr *expr, std::vector<std::string> &answers) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *name = dyn_cast<NameExpr>(expr);
  if (conversion != nullptr && !conversion->isImplicit()) {
    return std::nullopt;
  }
  if (conversion != nullptr) {
    return ChildrenFirst::takeAnswer(answers);
  }
  if (member != nullptr) {
    return this->memberText(member, answers);
  }
  if (isa<IndexExpr>(expr)) {
    return this->indexText(answers);
  }
  if (name != nullptr && name->name.starts_with(kHiddenPrefix)) {
    return std::nullopt;
  }

  return this->simpleText(expr);
}

std::string CodeOperatorsRegistry::memberText(const MemberExpr *member,
                                              std::vector<std::string> &answers) const {
  auto object = ChildrenFirst::takeAnswer(answers);
  const char *dot = member->optional ? "?." : ".";

  return object + dot + member->name.str();
}

std::string CodeOperatorsRegistry::indexText(std::vector<std::string> &answers) const {
  auto position = ChildrenFirst::takeAnswer(answers);
  auto object = ChildrenFirst::takeAnswer(answers);

  return object + "[" + position + "]";
}

CodeOperatorsRegistry::ResultResultNotDropped
CodeOperatorsRegistry::checkResultKept(const UnaryExpr *unary, const Type *dropped,
                                       bool isPromised) const {
  bool isLibrary = this->state.library.declarations.isInsideLibrary(unary->loc());
  if (dropped->holdsResults() && !isLibrary) {
    std::string message = isPromised ? ErrorTextUnary::dropsAsync() : ErrorTextUnary::drops();

    return err(CodeOperatorsRegistryResultKeptError{
        .code = CodeOperatorsRegistryResultKeptErrorCode::DROPS_RESULT,
        .message = message,
    });
  }

  return ok(dropped);
}

CodeOperatorsRegistry::ResultNonNullAssertion
CodeOperatorsRegistry::checkNonNullOperand(const Type *type, llvm::StringRef text) const {
  if (type->isUndefined()) {
    auto message = ErrorTextUnary::allUndef();

    return err(CodeOperatorsRegistryNonNullOperandError{
        .code = CodeOperatorsRegistryNonNullOperandErrorCode::ALWAYS_UNDEFINED,
        .message = message,
    });
  }
  if (!type->isOptional()) {
    auto message = ErrorTextUnary::neverUndef(text);

    return err(CodeOperatorsRegistryNonNullOperandError{
        .code = CodeOperatorsRegistryNonNullOperandErrorCode::NEVER_UNDEFINED,
        .message = message,
    });
  }
  auto message = ErrorTextUnary::checkFirst(text);

  return err(CodeOperatorsRegistryNonNullOperandError{
      .code = CodeOperatorsRegistryNonNullOperandErrorCode::NOT_SUPPORTED,
      .message = message,
  });
}

}
