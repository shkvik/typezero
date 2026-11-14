#include "compiler/semantic/types_v2/keys/types_keys_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/to_utf16/to_utf16.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr const char *kHiddenFieldPrefix = "%";

}

TypesKeysRegistry::TypesKeysRegistry(SemanticState &owner)
    : state(owner) {}

llvm::ArrayRef<const Type *> TypesKeysRegistry::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

const Type *TypesKeysRegistry::keysOf(const Type *type) const {
  const Type *plain = type->nonOptional();
  if (!plain->isUnion()) {
    return this->ownKeys(plain);
  }
  auto common = std::vector<const Type *>();
  bool isFirst = true;
  for (const Type *member : plain->members()) {
    const Type *keys = this->ownKeys(member->nonOptional());
    if (keys == nullptr) {
      return nullptr;
    }
    common = this->sharedKeys(common, keys, isFirst);
    isFirst = false;
  }
  if (common.empty()) {
    return Type::never();
  }

  return Type::unionOf(common);
}

std::vector<const Type *> TypesKeysRegistry::sharedKeys(llvm::ArrayRef<const Type *> common,
                                                        const Type *keys, bool isFirst) const {
  auto shared = std::vector<const Type *>();
  for (const Type *key : this->membersOf(keys)) {
    if (isFirst || llvm::is_contained(common, key)) {
      shared.push_back(key);
    }
  }

  return shared;
}

const Type *TypesKeysRegistry::ownKeys(const Type *type) const {
  if (type->isRecord()) {
    return type->keyType();
  }
  if (!type->isObject() || type->isTuple() || type->isAnyObject()) {
    return nullptr;
  }
  const ClassDecl *classDecl = type->classDecl();
  auto keys = std::vector<const Type *>();
  for (const Field &field : type->fields()) {
    if (this->isPublicKey(classDecl, field.name)) {
      auto text = ToUtf16::toUtf16(field.name);
      keys.push_back(Type::stringLiteral(text));
    }
  }
  if (keys.empty()) {
    return Type::never();
  }

  return Type::unionOf(keys);
}

bool TypesKeysRegistry::isPublicKey(const ClassDecl *classDecl, llvm::StringRef name) const {
  if (name.starts_with(kHiddenFieldPrefix)) {
    return false;
  }
  if (classDecl == nullptr) {
    return true;
  }
  const ClassField *declared = classDecl->findField(name);

  return declared == nullptr || declared->access == Access::Public;
}

void TypesKeysRegistry::evalKeyOf(const EvalKeyOf &task) {
  const Type *keys = this->keysOf(task.operand);
  const Type *answer = keys != nullptr ? keys : Type::error();
  this->state.walk.tasks.pushAnswer(answer);
}

void TypesKeysRegistry::evalIndexed(const EvalIndexed &task) {
  const Type *object = task.object;
  const Type *key = task.key;
  bool isList = object->isTuple() || object->isArray();
  if (isList && this->isLengthKey(key)) {
    const auto *length = this->lengthType(object);

    return this->state.walk.tasks.pushAnswer(length);
  }
  const auto *member = this->memberType(object, key);
  const Type *answer = member != nullptr ? member : Type::error();
  this->state.walk.tasks.pushAnswer(answer);
}

bool TypesKeysRegistry::isLengthKey(const Type *key) const {
  if (!key->isStringLiteral()) {
    return false;
  }
  auto text = ToUtf8::toUtf8(key->literalText());

  return BuiltinPropertyTable::isName(text, BuiltinProperty::LENGTH);
}

const Type *TypesKeysRegistry::lengthType(const Type *object) const {
  bool isFixed = object->isTuple() && object->tupleRest() == nullptr &&
                 object->tupleMin() == object->fields().size();
  if (isFixed) {
    return Type::numberLiteral(double(object->fields().size()), Type::intTy());
  }

  return Type::intTy();
}

const Type *TypesKeysRegistry::memberType(const Type *object, const Type *key) const {
  if (object->isTuple()) {
    return this->tupleMemberType(object, key);
  }
  if (object->isArray() && this->isNumericKey(key)) {
    return object->inner();
  }
  if (object->isObject()) {
    return this->objectMemberType(object, key);
  }
  if (object->isRecord() && key->isString()) {
    return Type::optional(object->valueType());
  }

  return nullptr;
}

bool TypesKeysRegistry::isNumericKey(const Type *key) const {
  return key->isNumeric() || (key->isLiteral() && key->literalBase()->isNumeric());
}

const Type *TypesKeysRegistry::tupleMemberType(const Type *tuple, const Type *key) const {
  llvm::ArrayRef<Field> fields = tuple->fields();
  if (key->isLiteral() && key->literalBase()->isNumeric()) {
    return this->tupleFieldAt(tuple, key->literalNumber());
  }
  if (!key->isNumeric()) {
    return nullptr;
  }
  auto types = std::vector<const Type *>();
  for (const Field &field : fields) {
    types.push_back(field.type);
  }
  if (tuple->tupleRest() != nullptr) {
    types.push_back(tuple->tupleRest()->inner());
  }
  if (types.empty()) {
    return nullptr;
  }

  return Type::unionOf(types);
}

const Type *TypesKeysRegistry::tupleFieldAt(const Type *tuple, double position) const {
  llvm::ArrayRef<Field> fields = tuple->fields();
  bool isInside =
      position >= 0 && position < double(fields.size()) && position == double(size_t(position));
  if (isInside) {
    return fields[size_t(position)].type;
  }
  if (tuple->tupleRest() == nullptr) {
    return nullptr;
  }

  return Type::optional(tuple->tupleRest()->inner());
}

const Type *TypesKeysRegistry::objectMemberType(const Type *object, const Type *key) const {
  llvm::ArrayRef<const Type *> names =
      key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
  auto types = std::vector<const Type *>();
  for (const Type *name : names) {
    if (!name->isStringLiteral()) {
      return nullptr;
    }
    auto fieldName = ToUtf8::toUtf8(name->literalText());
    const auto *field = object->field(fieldName);
    if (field == nullptr) {
      return nullptr;
    }
    types.push_back(field->type);
  }
  if (types.empty()) {
    return nullptr;
  }

  return Type::unionOf(types);
}

void TypesKeysRegistry::evalSpread(const EvalSpread &task) {
  llvm::ArrayRef<const Type *> operands = task.operands;
  auto elements = std::vector<const Type *>();
  auto spread = std::vector<bool>();
  for (size_t index = 0; index + 1 < operands.size(); index += 2) {
    spread.push_back(operands[index]->literalNumber() != 0);
    elements.push_back(operands[index + 1]);
  }
  auto why = std::string();
  const auto *tuple = ArgumentLists::spreadTuple(elements, spread, why);
  const Type *answer = tuple != nullptr ? tuple : Type::error();
  this->state.walk.tasks.pushAnswer(answer);
}

void TypesKeysRegistry::startKeyOf(const StartKeyOf &task) {
  const TypeRef &ref = *task.ref;
  this->state.walk.tasks.pushTask(AfterKeyOfOperand{.ref = &ref});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
}

void TypesKeysRegistry::afterKeyOfOperand(const AfterKeyOfOperand &task) {
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (type->isError()) {
    return this->state.walk.tasks.pushAnswer(type);
  }
  const auto *unevaluated = Type::typeOp(TypeOp::KeyOf, {type});
  this->state.walk.tasks.pushTask(AfterKeyOfEval{.ref = task.ref, .operand = type});
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = unevaluated, .operands = {type}});
}

void TypesKeysRegistry::afterKeyOfEval(const AfterKeyOfEval &task) {
  const Type *keys = this->state.walk.tasks.takeAnswer();
  if (keys->isError()) {
    auto message = ErrorTextKeys::keyOf(task.operand->name());
    this->state.diagnostics.error(task.ref->loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(keys);
}

void TypesKeysRegistry::startIndexed(const StartIndexed &task) {
  const TypeRef &ref = *task.ref;
  this->state.walk.tasks.pushTask(AfterIndexedOperands{.ref = &ref});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[1], .use = TypeUse::STANDALONE});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
}

void TypesKeysRegistry::afterIndexedOperands(const AfterIndexedOperands &task) {
  const Type *key = this->state.walk.tasks.takeAnswer();
  const Type *object = this->state.walk.tasks.takeAnswer();
  if (object->isError() || key->isError()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *unevaluated = Type::indexedAccess(object, key);
  this->state.walk.tasks.pushTask(AfterIndexedEval{.ref = task.ref, .object = object, .key = key});
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = unevaluated, .operands = {object, key}});
}

void TypesKeysRegistry::afterIndexedEval(const AfterIndexedEval &task) {
  const Type *member = this->state.walk.tasks.takeAnswer();
  if (member->isError()) {
    auto message = ErrorTextKeys::noMember(task.object->name(), task.key->name());
    this->state.diagnostics.error(task.ref->loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushAnswer(member);
}

}
