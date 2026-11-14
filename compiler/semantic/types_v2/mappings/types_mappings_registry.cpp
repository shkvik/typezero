#include "compiler/semantic/types_v2/mappings/types_mappings_registry.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/types/typeops_check_helpers/typeops_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

TypesMappingsRegistry::TypesMappingsRegistry(SemanticState &owner)
    : state(owner) {}

llvm::ArrayRef<const Type *> TypesMappingsRegistry::membersOf(const Type *&type) const {
  return type->isUnion() ? type->members() : llvm::ArrayRef<const Type *>(type);
}

void TypesMappingsRegistry::evalMapped(const EvalMapped &task) {
  llvm::ArrayRef<const Type *> operands = task.operands;
  auto mapped = MappedOperands{
      .key = operands[0],
      .keys = operands[1],
      .value = operands[2],
      .source = operands[3],
      .rename = operands.size() > 4 ? operands[4] : nullptr,
      .optional = task.op->mappedOptional(),
      .readonly = task.op->mappedReadonly(),
  };
  if (this->isOpenMapped(mapped)) {
    const auto *open = Type::mapped(mapped.key, mapped.keys, mapped.value, mapped.optional,
                                    mapped.readonly, mapped.source, mapped.rename);

    return this->state.walk.tasks.pushAnswer(open);
  }
  const Type *keys = mapped.keys;
  if (keys->isNever()) {
    const auto *empty = Type::object({});

    return this->state.walk.tasks.pushAnswer(empty);
  }
  auto members = this->membersOf(keys);
  auto draft = MappedDraft{
      .mapped = mapped,
      .keys = members.vec(),
      .next = 0,
      .fields = {},
      .fieldNames = {},
      .type = nullptr,
      .isReadonly = false,
  };

  return this->nextMappedKey(NextMappedKey{.draft = draft});
}

bool TypesMappingsRegistry::isOpenMapped(const MappedOperands &mapped) const {
  auto &relations = this->state.types.relations;

  return TypeQueries::mentionsParam(mapped.keys) ||
         (mapped.source != nullptr && TypeQueries::mentionsParam(mapped.source)) ||
         relations.isMentioningParamBesides(mapped.value, {mapped.key}) ||
         (mapped.rename != nullptr &&
          relations.isMentioningParamBesides(mapped.rename, {mapped.key}));
}

void TypesMappingsRegistry::nextMappedKey(const NextMappedKey &task) {
  const MappedDraft &draft = task.draft;
  if (draft.next == draft.keys.size()) {
    const auto *object = Type::object(draft.fields);

    return this->state.walk.tasks.pushAnswer(object);
  }
  const Type *name = draft.keys[draft.next];
  if (!name->isStringLiteral()) {
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const MappedOperands &mapped = draft.mapped;
  this->state.walk.tasks.pushTask(AfterMappedValue{.draft = draft});
  this->state.walk.tasks.pushTask(
      SubstituteType{.type = mapped.value, .params = {mapped.key}, .args = {name}});
}

void TypesMappingsRegistry::afterMappedValue(const AfterMappedValue &task) {
  MappedDraft draft = task.draft;
  const MappedOperands &mapped = draft.mapped;
  const Type *name = draft.keys[draft.next];
  const Type *value = this->state.walk.tasks.takeAnswer();
  if (value->isError()) {
    return this->state.walk.tasks.pushAnswer(value);
  }
  auto text = ToUtf8::toUtf8(name->literalText());
  draft.type = this->mappedFieldType(mapped, value);
  draft.isReadonly = this->isMappedReadonly(mapped, text);
  draft.fieldNames = {text};
  if (mapped.rename == nullptr) {
    return this->addMappedFields(draft);
  }
  this->state.walk.tasks.pushTask(AfterMappedRename{.draft = draft});
  this->state.walk.tasks.pushTask(
      SubstituteType{.type = mapped.rename, .params = {mapped.key}, .args = {name}});
}

void TypesMappingsRegistry::afterMappedRename(const AfterMappedRename &task) {
  MappedDraft draft = task.draft;
  const Type *renamed = this->state.walk.tasks.takeAnswer();
  if (renamed->isError()) {
    return this->state.walk.tasks.pushAnswer(renamed);
  }
  draft.fieldNames.clear();
  for (const Type *renamedTo : TypeopsCheckHelpers::distributedMembers(renamed)) {
    if (!renamedTo->isStringLiteral()) {
      return this->state.walk.tasks.pushAnswer(Type::error());
    }
    draft.fieldNames.push_back(ToUtf8::toUtf8(renamedTo->literalText()));
  }

  return this->addMappedFields(draft);
}

void TypesMappingsRegistry::addMappedFields(MappedDraft draft) {
  for (const std::string &field : draft.fieldNames) {
    this->mergeMappedField(draft.fields, field, draft.type, draft.isReadonly);
  }
  draft.next += 1;

  return this->nextMappedKey(NextMappedKey{.draft = draft});
}

const Type *TypesMappingsRegistry::mappedFieldType(const MappedOperands &mapped,
                                                   const Type *value) const {
  if (mapped.optional > 0) {
    return Type::optional(value);
  }
  if (mapped.optional < 0) {
    return value->nonOptional();
  }

  return value;
}

bool TypesMappingsRegistry::isMappedReadonly(const MappedOperands &mapped,
                                             llvm::StringRef name) const {
  if (mapped.readonly != 0) {
    return mapped.readonly > 0;
  }
  if (mapped.source == nullptr) {
    return false;
  }
  const Field *from = mapped.source->field(name);

  return from != nullptr && from->readonly;
}

void TypesMappingsRegistry::mergeMappedField(std::vector<Field> &fields, const std::string &name,
                                             const Type *type, bool isReadonly) const {
  for (Field &field : fields) {
    if (field.name == name) {
      field.type = Type::unionOf({field.type, type});
      return;
    }
  }
  fields.push_back({name, type, isReadonly});
}

void TypesMappingsRegistry::startMapped(const StartMapped &task) {
  const TypeRef &ref = *task.ref;
  this->state.walk.tasks.pushTask(AfterMappedKeys{.ref = &ref});
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[0], .use = TypeUse::STANDALONE});
}

void TypesMappingsRegistry::afterMappedKeys(const AfterMappedKeys &task) {
  const TypeRef &ref = *task.ref;
  const Type *keys = this->state.walk.tasks.takeAnswer();
  if (keys->isError()) {
    return this->state.walk.tasks.pushAnswer(keys);
  }
  if (ref.args[0].kind() == TypeRefKind::KEY_OF_REF) {
    this->state.walk.tasks.pushTask(AfterMappedSource{.ref = &ref, .keys = keys});

    return this->state.walk.tasks.pushTask(
        ResolveTypeAnswer{.ref = &ref.args[0].args[0], .use = TypeUse::STANDALONE});
  }
  const auto *source = this->widerSource(keys);

  return this->startMappedParts(ref, keys, source);
}

void TypesMappingsRegistry::afterMappedSource(const AfterMappedSource &task) {
  const Type *source = this->state.walk.tasks.takeAnswer();

  return this->startMappedParts(*task.ref, task.keys, source);
}

const Type *TypesMappingsRegistry::widerSource(const Type *keys) const {
  const Type *wider = keys;
  while (wider != nullptr) {
    if (wider->typeOp() == TypeOp::KeyOf) {
      return wider->operands()[0];
    }
    wider = this->widerKeys(wider);
  }

  return nullptr;
}

const Type *TypesMappingsRegistry::widerKeys(const Type *keys) const {
  if (keys->typeOp() == TypeOp::Exclude || keys->typeOp() == TypeOp::Extract) {
    return keys->operands()[0];
  }
  if (keys->isParam()) {
    return keys->constraint();
  }

  return nullptr;
}

void TypesMappingsRegistry::startMappedParts(const TypeRef &ref, const Type *keys,
                                             const Type *source) {
  auto *key = Type::param(ref.name);
  key->setConstraint(keys, {});
  this->state.walk.scopes.openTypeScope();
  auto *alias = this->state.ast.make<TypeAliasDecl>(ref.loc, ref.name, TypeRef());
  alias->resolved = key;
  this->state.walk.scopes.setType(ref.name, alias);
  auto draft = MappedPartsDraft{
      .ref = &ref,
      .keys = keys,
      .source = source,
      .key = key,
  };
  this->state.walk.tasks.pushTask(AfterMappedParts{.draft = draft});
  if (ref.args.size() > 2) {
    this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &ref.args[2], .use = TypeUse::INNER});
  }
  this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &ref.args[1], .use = TypeUse::STANDALONE});
}

void TypesMappingsRegistry::afterMappedParts(const AfterMappedParts &task) {
  const MappedPartsDraft &draft = task.draft;
  const TypeRef &ref = *draft.ref;
  const Type *rename = ref.args.size() > 2 ? this->state.walk.tasks.takeAnswer() : nullptr;
  const Type *value = this->state.walk.tasks.takeAnswer();
  this->state.walk.scopes.closeTypeScope();
  if (value->isError()) {
    return this->state.walk.tasks.pushAnswer(value);
  }
  if (rename != nullptr && rename->isError()) {
    return this->state.walk.tasks.pushAnswer(rename);
  }
  const Type *badName = this->nonStringRename(rename);
  if (badName != nullptr) {
    auto message = ErrorTextMappings::rename(badName->name());
    this->state.diagnostics.error(ref.args[2].loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const Type *badKey = this->nonStringKey(draft.keys);
  if (badKey != nullptr) {
    auto message = ErrorTextMappings::keys(badKey->name());
    this->state.diagnostics.error(ref.args[0].loc, message);

    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  const auto *mapped = Type::mapped(draft.key, draft.keys, value, ref.mappedOptional(),
                                    ref.mappedReadonly(), draft.source, rename);
  auto operands = mapped->operands();
  this->state.walk.tasks.pushTask(EvalTypeOp{.op = mapped, .operands = operands.vec()});
}

const Type *TypesMappingsRegistry::nonStringRename(const Type *rename) const {
  if (rename == nullptr || TypeQueries::mentionsParam(rename)) {
    return nullptr;
  }
  for (const Type *renamedTo : TypeopsCheckHelpers::distributedMembers(rename)) {
    if (!renamedTo->isStringLiteral()) {
      return renamedTo;
    }
  }

  return nullptr;
}

const Type *TypesMappingsRegistry::nonStringKey(const Type *keys) const {
  if (TypeQueries::mentionsParam(keys)) {
    return nullptr;
  }
  const Type *each = keys;
  for (const Type *key : this->membersOf(each)) {
    if (!key->isStringLiteral() && !key->isNever()) {
      return key;
    }
  }

  return nullptr;
}

}
