#include "compiler/semantic/code/comparisons/in_operators/in_operators.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

namespace {

constexpr const char *kUnnamedArray = "a";
constexpr const char *kUnnamedIndex = "i";
constexpr const char *kObjectPlaceholder = "o";

}

InOperators::InOperators(SemanticState &owner, ComparisonRules &sharedComparisonRules)
    : state(owner),
      comparisonRules(sharedComparisonRules) {}

void InOperators::checkInOperator(const CheckInOperator &task) {
  BinaryExpr *binary = task.binary;
  auto key = this->simpleText(binary->lhs);
  bool isLiteralKey = isa<IntLitExpr>(binary->lhs);
  auto array = this->simpleText(binary->rhs).value_or(kUnnamedArray);
  this->state.walk.tasks.pushTask(AfterInOperands{
      .binary = binary,
      .key = key,
      .isLiteralKey = isLiteralKey,
      .array = array,
  });
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->rhs, .expected = nullptr});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &binary->lhs, .expected = nullptr});
}

void InOperators::afterInOperands(const AfterInOperands &task) {
  BinaryExpr *binary = task.binary;
  binary->type = Type::boolTy();
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  if (leftType->isError() || rightType->isError()) {
    return this->state.walk.tasks.pushChecked(binary);
  }
  if (rightType->nonOptional()->isDict()) {
    return this->state.walk.tasks.pushTask(CheckDictIn{.binary = binary});
  }
  auto in = this->checkInTarget(binary, task.key, task.isLiteralKey, task.array);
  if (in.isErr()) {
    auto at = this->inErrorPlace(binary, in.error().code);
    this->state.diagnostics.error(at, in.error().message);
  }

  return this->state.walk.tasks.pushChecked(binary);
}

InOperators::ResultInOperatorSuited
InOperators::checkInTarget(BinaryExpr *binary, const std::optional<std::string> &key,
                           bool isLiteralKey, const std::string &array) {
  const Type *leftType = binary->lhs->type;
  const Type *rightType = binary->rhs->type;
  const Type *right = rightType->nonOptional();
  if (right->isUnion()) {
    return this->checkUnionIn(binary, rightType);
  }
  if (right->isObject()) {
    return this->checkObjectIn(binary, right);
  }
  if (!right->isArray()) {
    const char *op = Operators::opSpelling(BinaryOp::In);
    auto message = this->comparisonRules.operandTypesText(op, leftType, rightType);

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::OPERAND_TYPES,
        .message = message,
    });
  }
  bool isKnownIndex = leftType->isInteger() && key.has_value();
  std::string indexText = isKnownIndex ? *key : kUnnamedIndex;
  auto check = this->inBoundsText(indexText, array, isLiteralKey);
  auto message = ErrorTextIn::array(check);

  return err(CodeComparisonsRegistryCheckInError{
      .code = CodeComparisonsRegistryCheckInErrorCode::ARRAY_IN,
      .message = message,
  });
}

std::string InOperators::fieldText(const Expr *object, llvm::StringRef field) const {
  auto chain = this->chainText(object);
  std::string text = chain.has_value() ? *chain : kObjectPlaceholder;
  bool isIdentifier = !field.empty() && !llvm::isDigit(field[0]) &&
                      llvm::all_of(field, std::bind_front(&InOperators::isIdentifierChar, this));

  return isIdentifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

std::optional<std::string> InOperators::chainText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = ObjectCheckHelpers::unnarrowed(expr);
  const auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  const auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr) {
    return std::nullopt;
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

bool InOperators::isIdentifierChar(char letter) const {
  return llvm::isAlnum(letter) || letter == '_' || letter == '$';
}

llvm::SMLoc InOperators::inErrorPlace(const BinaryExpr *binary,
                                      CodeComparisonsRegistryCheckInErrorCode code) const {
  switch (code) {
    case CodeComparisonsRegistryCheckInErrorCode::DICT_UNDEFINED:
    case CodeComparisonsRegistryCheckInErrorCode::UNION_UNDEFINED:
      return binary->rhs->loc();
    case CodeComparisonsRegistryCheckInErrorCode::DICT_NUMBER_KEY:
      return binary->lhs->loc();
    case CodeComparisonsRegistryCheckInErrorCode::DICT_NOT_RECORD:
    case CodeComparisonsRegistryCheckInErrorCode::UNION_NO_KEY:
    case CodeComparisonsRegistryCheckInErrorCode::OPTIONAL_FIELD:
    case CodeComparisonsRegistryCheckInErrorCode::ALWAYS_SAME:
    case CodeComparisonsRegistryCheckInErrorCode::TUPLE_IN:
    case CodeComparisonsRegistryCheckInErrorCode::OBJECT_OPTIONAL:
    case CodeComparisonsRegistryCheckInErrorCode::OBJECT_ALWAYS_TRUE:
    case CodeComparisonsRegistryCheckInErrorCode::OBJECT_ALWAYS_FALSE:
    case CodeComparisonsRegistryCheckInErrorCode::OBJECT_IN:
    case CodeComparisonsRegistryCheckInErrorCode::OPERAND_TYPES:
    case CodeComparisonsRegistryCheckInErrorCode::ARRAY_IN:
      return binary->loc();
  }

  return binary->loc();
}

std::optional<std::string> InOperators::simpleText(const Expr *expr) const {
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

std::string InOperators::inBoundsText(const std::string &indexText, const std::string &array,
                                      bool isLiteralKey) const {
  auto below = indexText + " < " + array + ".length";
  if (isLiteralKey) {
    return below;
  }

  return indexText + " >= 0 && " + below;
}

InOperators::ResultInOperatorSuited InOperators::checkUnionIn(BinaryExpr *binary,
                                                              const Type *rightType) {
  if (rightType->isOptional()) {
    auto message = ErrorTextIn::undefined();

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::UNION_UNDEFINED,
        .message = message,
    });
  }
  const Type *options = rightType->nonOptional();
  auto *text = dyn_cast<StringLitExpr>(binary->lhs);
  auto name = text != nullptr ? this->state.code.constants.constantKey(binary->lhs) : std::nullopt;
  const auto *objects = options->slotPart(UnionSlot::Object);
  if (!name.has_value() || objects == nullptr) {
    auto message = ErrorTextIn::byLiteral();

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::UNION_NO_KEY,
        .message = message,
    });
  }

  return this->checkUnionKeyIn(binary, options, objects, *name);
}

std::vector<const Type *> InOperators::objectMembers(const Type *objects) const {
  if (!objects->isUnion()) {
    return {objects};
  }
  auto members = objects->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

InOperators::ResultInOperatorSuited InOperators::checkUnionKeyIn(BinaryExpr *binary,
                                                                 const Type *options,
                                                                 const Type *objects,
                                                                 const std::string &name) const {
  auto members = this->objectMembers(objects);
  for (const Type *member : members) {
    auto required = this->checkFieldRequired(binary, member, name);
    if (required.isErr()) {
      return err(required.error());
    }
  }
  size_t withField = this->countWithField(members, name);
  bool isAlwaysSame = withField == 0 || (withField == members.size() && objects == options);
  if (isAlwaysSame) {
    auto message = ErrorTextIn::always(withField != 0, options->name(), name);

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::ALWAYS_SAME,
        .message = message,
    });
  }

  return ok(binary);
}

InOperators::ResultInOperatorSuited InOperators::checkFieldRequired(BinaryExpr *binary,
                                                                    const Type *member,
                                                                    const std::string &name) const {
  const auto *field = member->field(name);
  if (field != nullptr && field->type->isOptional()) {
    auto shown = this->fieldText(binary->rhs, name);
    auto message = ErrorTextIn::maybeUndef(name, member->name(), shown);

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::OPTIONAL_FIELD,
        .message = message,
    });
  }

  return ok(binary);
}

size_t InOperators::countWithField(llvm::ArrayRef<const Type *> members,
                                   const std::string &name) const {
  size_t count = 0;
  for (const Type *member : members) {
    if (member->field(name) != nullptr) {
      count += 1;
    }
  }

  return count;
}

InOperators::ResultInOperatorSuited InOperators::checkObjectIn(BinaryExpr *binary,
                                                               const Type *object) {
  if (object->isTuple()) {
    auto message = ErrorTextIn::tuple();

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::TUPLE_IN,
        .message = message,
    });
  }
  auto name = this->state.code.constants.constantKey(binary->lhs);
  const Field *field = name.has_value() ? object->field(*name) : nullptr;
  if (field != nullptr && field->type->isOptional()) {
    auto text = this->fieldText(binary->rhs, *name);
    auto message = ErrorTextIn::objMaybe(text);

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::OBJECT_OPTIONAL,
        .message = message,
    });
  }
  if (field != nullptr) {
    auto message = ErrorTextIn::objTrue();

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::OBJECT_ALWAYS_TRUE,
        .message = message,
    });
  }
  if (name.has_value()) {
    auto message = ErrorTextIn::objFalse(object->name(), *name);

    return err(CodeComparisonsRegistryCheckInError{
        .code = CodeComparisonsRegistryCheckInErrorCode::OBJECT_ALWAYS_FALSE,
        .message = message,
    });
  }
  auto message = ErrorTextIn::objPlain();

  return err(CodeComparisonsRegistryCheckInError{
      .code = CodeComparisonsRegistryCheckInErrorCode::OBJECT_IN,
      .message = message,
  });
}

}
