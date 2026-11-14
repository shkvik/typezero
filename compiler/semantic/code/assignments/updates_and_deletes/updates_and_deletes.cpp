#include "compiler/semantic/code/assignments/updates_and_deletes/updates_and_deletes.h"
#include "compiler/semantic/bodies/objects/object_check_helpers/object_check_helpers.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

namespace {

constexpr const char *kIncrementSpelling = "++";
constexpr const char *kDecrementSpelling = "--";
constexpr const char *kUnnamedArray = "a";
constexpr const char *kUnnamedIndex = "i";
constexpr const char *kObjectPlaceholder = "o";

}

UpdatesAndDeletes::UpdatesAndDeletes(SemanticState &owner, AssignmentRules &sharedAssignmentRules)
    : state(owner),
      assignmentRules(sharedAssignmentRules) {}

std::string UpdatesAndDeletes::fieldText(const Expr *object, llvm::StringRef field) const {
  auto chain = this->chainText(object);
  std::string text = chain.has_value() ? *chain : kObjectPlaceholder;
  bool isIdentifier =
      !field.empty() && !llvm::isDigit(field[0]) &&
      llvm::all_of(field, std::bind_front(&UpdatesAndDeletes::isIdentifierChar, this));

  return isIdentifier ? text + "." + field.str() : text + "[\"" + field.str() + "\"]";
}

bool UpdatesAndDeletes::isIdentifierChar(char letter) const {
  return llvm::isAlnum(letter) || letter == '_' || letter == '$';
}

std::optional<std::string> UpdatesAndDeletes::chainText(const Expr *expr) const {
  auto names = std::vector<llvm::StringRef>();
  const auto *link = ObjectCheckHelpers::unnarrowed(expr);
  auto *member = dyn_cast<MemberExpr>(link);
  while (member != nullptr) {
    if (member->optional) {
      return std::nullopt;
    }
    names.push_back(member->name);
    link = ObjectCheckHelpers::unnarrowed(member->object);
    member = dyn_cast<MemberExpr>(link);
  }
  auto *root = dyn_cast<NameExpr>(link);
  if (root == nullptr) {
    return std::nullopt;
  }
  std::string text = root->name.str();
  for (llvm::StringRef name : llvm::reverse(names)) {
    text += "." + name.str();
  }

  return text;
}

void UpdatesAndDeletes::checkUpdate(const CheckUpdate &task) {
  UpdateExpr *update = task.update;
  update->type = Type::error();
  this->state.walk.tasks.pushTask(AfterUpdateTarget{.update = update});

  return this->assignmentRules.startAssignTarget(&update->target, AssignForm::COMPOUND);
}

void UpdatesAndDeletes::afterUpdateTarget(const AfterUpdateTarget &task) {
  UpdateExpr *update = task.update;
  const Type *type = this->state.walk.tasks.takeAnswer();
  this->state.walk.tasks.pushChecked(update);
  if (type->isError()) {
    return;
  }
  auto numeric = this->checkUpdatable(update, type);
  if (numeric.isErr()) {
    return this->state.diagnostics.error(update->loc(), numeric.error().message);
  }
  update->type = type;
  auto *name = dyn_cast<NameExpr>(update->target);
  auto *index = dyn_cast<IndexExpr>(update->target);
  auto *member = dyn_cast<MemberExpr>(update->target);
  if (name != nullptr && name->var() != nullptr) {
    int64_t step = update->increment ? 1 : -1;
    return this->state.walk.tasks.pushTask(NoteIncremented{.variable = name->var(), .step = step});
  }
  if (index != nullptr && !index->keyed() && index->object->type->isTypedArray()) {
    return this->state.walk.tasks.pushTask(WalkBytesStore{.index = index, .value = nullptr});
  }
  if (index != nullptr && !index->keyed()) {
    auto within = this->assignmentRules.checkIndexProven(index);
    if (within.isErr()) {
      this->state.diagnostics.error(index->loc(), within.error().message);
    }
    return;
  }
  if (member != nullptr) {
    return this->state.walk.flow.forgetFieldsNamed(member->name);
  }
}

void UpdatesAndDeletes::checkDeleteValue(const CheckDeleteValue &task) {
  UnaryExpr *unary = task.unary;
  unary->type = Type::boolTy();
  auto *index = dyn_cast<IndexExpr>(unary->operand);
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  auto &tasks = this->state.walk.tasks;
  if (member != nullptr && !member->optional) {
    tasks.pushTask(AfterMemberDelete{.unary = unary});
    return tasks.pushTask(CheckExprSlot{.slot = &member->object, .expected = nullptr});
  }
  if (index == nullptr) {
    tasks.pushTask(AfterOtherDelete{.unary = unary});
    return tasks.pushTask(CheckExprSlot{.slot = &unary->operand, .expected = nullptr});
  }
  auto array = this->simpleText(index->object).value_or(kUnnamedArray);
  auto position = this->simpleText(index->index).value_or(kUnnamedIndex);
  tasks.pushTask(AfterIndexDeleteObject{.unary = unary, .array = array, .position = position});

  return tasks.pushTask(CheckExprSlot{.slot = &index->object, .expected = nullptr});
}

void UpdatesAndDeletes::afterMemberDelete(const AfterMemberDelete &task) {
  UnaryExpr *unary = task.unary;
  auto *member = cast<MemberExpr>(unary->operand);
  const Type *object = member->object->type;
  if (object->isDict()) {
    return this->state.walk.tasks.pushTask(CheckDictDelete{.unary = unary});
  }
  this->state.walk.tasks.pushChecked(unary);
  if (object->isError()) {
    return;
  }
  const auto *field = this->ownField(object, member->name);
  auto deleted = this->checkDeletedField(member->object, field);
  if (deleted.isErr()) {
    return this->state.diagnostics.error(unary->loc(), deleted.error().message);
  }
}

void UpdatesAndDeletes::afterOtherDelete(const AfterOtherDelete &task) {
  UnaryExpr *unary = task.unary;
  this->state.walk.tasks.pushChecked(unary);
  if (unary->operand->type->isError()) {
    return;
  }
  auto *member = dyn_cast<MemberExpr>(unary->operand);
  const auto *field =
      member != nullptr ? this->ownField(member->object->type, member->name) : nullptr;
  const Expr *object = member != nullptr ? member->object : nullptr;
  auto deleted = this->checkDeletedField(object, field);
  if (deleted.isErr()) {
    return this->state.diagnostics.error(unary->loc(), deleted.error().message);
  }
}

UpdatesAndDeletes::ResultDeleteIsPossible
UpdatesAndDeletes::checkDeletedField(const Expr *object, const Field *field) const {
  if (field != nullptr) {
    return this->checkFieldDelete(object, field);
  }
  auto message = ErrorTextDelete::notField();

  return err(CodeAssignmentsRegistryCheckDeleteError{
      .code = CodeAssignmentsRegistryCheckDeleteErrorCode::NOT_A_FIELD,
      .message = message,
  });
}

void UpdatesAndDeletes::afterIndexDeleteObject(const AfterIndexDeleteObject &task) {
  UnaryExpr *unary = task.unary;
  auto *index = cast<IndexExpr>(unary->operand);
  if (index->object->type->isDict()) {
    return this->state.walk.tasks.pushTask(CheckDictDelete{.unary = unary});
  }
  this->state.walk.tasks.pushTask(
      AfterIndexDeleteKey{.unary = unary, .array = task.array, .position = task.position});

  return this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &index->index, .expected = nullptr});
}

void UpdatesAndDeletes::afterIndexDeleteKey(const AfterIndexDeleteKey &task) {
  UnaryExpr *unary = task.unary;
  auto *index = cast<IndexExpr>(unary->operand);
  index->type = Type::error();
  this->state.walk.tasks.pushChecked(unary);
  auto deleted = this->checkKeyDelete(unary, index, task.array, task.position);
  if (deleted.isErr()) {
    return this->state.diagnostics.error(unary->loc(), deleted.error().message);
  }
}

UpdatesAndDeletes::ResultIncrementedValue
UpdatesAndDeletes::checkUpdatable(const UpdateExpr *update, const Type *type) const {
  if (!type->isNumeric() && !type->isDecimal()) {
    const char *op = update->increment ? kIncrementSpelling : kDecrementSpelling;
    auto message = ErrorTextUpdate::notNumber(op, type->name());

    return err(CodeAssignmentsRegistryUpdatableError{
        .code = CodeAssignmentsRegistryUpdatableErrorCode::NOT_NUMERIC,
        .message = message,
    });
  }

  return ok(type);
}

std::optional<std::string> UpdatesAndDeletes::simpleText(const Expr *expr) const {
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

UpdatesAndDeletes::ResultDeleteIsPossible
UpdatesAndDeletes::checkFieldDelete(const Expr *object, const Field *field) const {
  if (field->type->isOptional()) {
    auto text = this->fieldText(object, field->name);
    auto message = ErrorTextDelete::optional(field->name, text);

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::OPTIONAL_FIELD,
        .message = message,
    });
  }
  auto message = ErrorTextDelete::required(field->name);

  return err(CodeAssignmentsRegistryCheckDeleteError{
      .code = CodeAssignmentsRegistryCheckDeleteErrorCode::REQUIRED_FIELD,
      .message = message,
  });
}

const Field *UpdatesAndDeletes::ownField(const Type *object, llvm::StringRef name) const {
  const Type *own = object->nonOptional();
  if (!own->isObject()) {
    return nullptr;
  }

  return own->field(name);
}

UpdatesAndDeletes::ResultDeleteIsPossible
UpdatesAndDeletes::checkKeyDelete(UnaryExpr *unary, const IndexExpr *index,
                                  const std::string &array, const std::string &position) {
  const Type *type = index->object->type->nonOptional();
  if (type->isArray()) {
    auto message = ErrorTextDelete::hole(array, position);

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::ARRAY_HOLE,
        .message = message,
    });
  }
  if (type->isString()) {
    auto message = ErrorTextDelete::character();

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::STRING_CHARACTER,
        .message = message,
    });
  }
  if (type->isTuple()) {
    auto message = ErrorTextDelete::tupleHole();

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::TUPLE_HOLE,
        .message = message,
    });
  }
  const auto *field = this->keyedField(type, index);
  if (field != nullptr) {
    return this->checkFieldDelete(index->object, field);
  }
  if (type->isObject()) {
    auto message = ErrorTextDelete::objectFld();

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::OBJECT_FIELD,
        .message = message,
    });
  }
  if (!type->isError()) {
    auto message = ErrorTextDelete::notField();

    return err(CodeAssignmentsRegistryCheckDeleteError{
        .code = CodeAssignmentsRegistryCheckDeleteErrorCode::NOT_A_FIELD,
        .message = message,
    });
  }

  return ok(unary);
}

const Field *UpdatesAndDeletes::keyedField(const Type *type, const IndexExpr *index) const {
  if (!type->isObject()) {
    return nullptr;
  }
  auto key = this->state.code.constants.constantKey(index->index);
  if (!key.has_value()) {
    return nullptr;
  }

  return type->field(*key);
}

}
