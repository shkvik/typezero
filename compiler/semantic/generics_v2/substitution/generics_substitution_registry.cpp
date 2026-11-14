#include "compiler/semantic/generics_v2/substitution/generics_substitution_registry.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GenericsSubstitutionRegistry::GenericsSubstitutionRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsSubstitutionRegistry::substituteType(const SubstituteType &task) {
  return this->substituteNode(SubstituteNode{
      .type = task.type,
      .params = task.params,
      .args = task.args,
      .voidAs = VoidAs::VOID,
  });
}

void GenericsSubstitutionRegistry::substituteValueType(const SubstituteValueType &task) {
  return this->substituteNode(SubstituteNode{
      .type = task.type,
      .params = task.params,
      .args = task.args,
      .voidAs = VoidAs::UNDEFINED,
  });
}

void GenericsSubstitutionRegistry::instanceType(const InstanceType &task) {
  const ClassDecl *generic = task.instance->generic();
  if (task.type == nullptr || generic == nullptr) {
    return this->state.walk.tasks.pushAnswer(task.type);
  }

  return this->substituteType(SubstituteType{
      .type = task.type,
      .params = generic->typeArgs,
      .args = task.instance->typeArgs,
  });
}

void GenericsSubstitutionRegistry::instanceValueType(const InstanceValueType &task) {
  const ClassDecl *generic = task.instance->generic();
  if (task.type == nullptr || generic == nullptr) {
    return this->state.walk.tasks.pushAnswer(task.type);
  }

  return this->substituteValueType(SubstituteValueType{
      .type = task.type,
      .params = generic->typeArgs,
      .args = task.instance->typeArgs,
  });
}

void GenericsSubstitutionRegistry::substituteTypeList(const SubstituteTypeList &task) {
  TypeListDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  while (draft.index < draft.types.size() && draft.types[draft.index] == nullptr) {
    draft.results.push_back(nullptr);
    draft.index += 1;
  }
  if (draft.index == draft.types.size()) {
    for (const Type *result : draft.results) {
      tasks.pushAnswer(result);
    }
    return;
  }
  const Type *type = draft.types[draft.index];
  bool isValue = draft.isValues[draft.index];
  auto params = draft.params;
  auto args = draft.args;
  tasks.pushTask(AfterTypeListItem{.draft = std::move(draft)});
  if (isValue) {
    return tasks.pushTask(
        SubstituteValueType{.type = type, .params = std::move(params), .args = std::move(args)});
  }

  return tasks.pushTask(
      SubstituteType{.type = type, .params = std::move(params), .args = std::move(args)});
}

void GenericsSubstitutionRegistry::afterTypeListItem(const AfterTypeListItem &task) {
  TypeListDraft draft = task.draft;
  draft.results.push_back(this->state.walk.tasks.takeAnswer());
  draft.index += 1;

  return this->substituteTypeList(SubstituteTypeList{.draft = std::move(draft)});
}

void GenericsSubstitutionRegistry::substituteNode(const SubstituteNode &task) {
  auto &tasks = this->state.walk.tasks;
  const Type *type = task.type;
  if (type == nullptr || !GenericCheckHelpers::mentionsAnyOf(type, task.params)) {
    return tasks.pushAnswer(type);
  }
  auto form = this->substituteForm(type);
  if (!form.has_value()) {
    const auto *plain = this->plainSubstitute(task);

    return tasks.pushAnswer(plain);
  }
  if (*form == SubstituteForm::OBJECT) {
    this->state.generics.parameters.enterSubstitution(type);
  }
  auto parts = this->nodeParts(task, *form);
  tasks.pushTask(BuildSubstitutedType{.type = type, .form = *form, .count = parts.size()});
  for (SubstituteNode &part : llvm::reverse(parts)) {
    tasks.pushTask(std::move(part));
  }
}

const Type *GenericsSubstitutionRegistry::plainSubstitute(const SubstituteNode &node) const {
  const Type *type = node.type;
  if (!type->isParam()) {
    return type;
  }
  auto position = size_t(llvm::find(node.params, type) - node.params.begin());
  const Type *arg = node.args[position];
  if (arg == nullptr) {
    return type;
  }
  if (arg->isVoid() && node.voidAs == VoidAs::UNDEFINED) {
    return Type::undefinedTy();
  }

  return arg;
}

std::optional<SubstituteForm> GenericsSubstitutionRegistry::substituteForm(const Type *type) const {
  switch (type->kind()) {
    case Type::Kind::Param:
      return this->paramForm(type);
    case Type::Kind::Optional:
      return SubstituteForm::OPTIONAL;
    case Type::Kind::Array:
      return SubstituteForm::ARRAY;
    case Type::Kind::Result:
      return SubstituteForm::RESULT;
    case Type::Kind::Function:
      return SubstituteForm::FUNCTION;
    case Type::Kind::Iterator:
      return SubstituteForm::ITERATOR;
    case Type::Kind::Dict:
      return SubstituteForm::DICT;
    case Type::Kind::Union:
      return SubstituteForm::UNION;
    case Type::Kind::Promise:
      return SubstituteForm::PROMISE;
    case Type::Kind::ClassRef:
      return this->classRefForm(type);
    case Type::Kind::Object:
      return this->objectForm(type);
    case Type::Kind::Error:
    case Type::Kind::Void:
    case Type::Kind::Undefined:
    case Type::Kind::Bool:
    case Type::Kind::Int:
    case Type::Kind::Double:
    case Type::Kind::Decimal:
    case Type::Kind::String:
    case Type::Kind::Never:
    case Type::Kind::Json:
    case Type::Kind::Literal:
    case Type::Kind::Unknown:
      return std::nullopt;
  }

  return std::nullopt;
}

std::optional<SubstituteForm> GenericsSubstitutionRegistry::paramForm(const Type *type) const {
  if (type->typeOp() == TypeOp::None) {
    return std::nullopt;
  }

  return SubstituteForm::TYPE_OPERATOR;
}

std::optional<SubstituteForm> GenericsSubstitutionRegistry::classRefForm(const Type *type) const {
  if (!type->isConstructor()) {
    return std::nullopt;
  }

  return SubstituteForm::CONSTRUCTOR;
}

std::optional<SubstituteForm> GenericsSubstitutionRegistry::objectForm(const Type *type) const {
  if (type->isAsyncIterator()) {
    return SubstituteForm::ASYNC_ITERATOR;
  }
  if (this->state.code.decorators.findDescriptorOrigin(type) != nullptr) {
    return SubstituteForm::DESCRIPTOR;
  }
  const ClassDecl *classDecl = type->classDecl();
  if (classDecl != nullptr && !type->typeArgs().empty()) {
    return SubstituteForm::CLASS_INSTANCE;
  }
  if (this->state.generics.aliases.findAliasOrigin(type) != nullptr) {
    return SubstituteForm::ALIAS_INSTANCE;
  }
  if (type->isTuple()) {
    return SubstituteForm::TUPLE;
  }
  if (this->state.generics.parameters.isSubstituting(type)) {
    return std::nullopt;
  }

  return SubstituteForm::OBJECT;
}

std::vector<SubstituteNode> GenericsSubstitutionRegistry::nodeParts(const SubstituteNode &node,
                                                                    SubstituteForm form) const {
  const Type *type = node.type;
  switch (form) {
    case SubstituteForm::TYPE_OPERATOR:
      return this->nodesOf(node, type->operands(), VoidAs::VOID);
    case SubstituteForm::OPTIONAL:
    case SubstituteForm::ARRAY:
      return this->nodesOf(node, {type->inner()}, VoidAs::UNDEFINED);
    case SubstituteForm::RESULT:
      return this->nodesOf(node, {type->okType(), type->errType()}, VoidAs::VOID);
    case SubstituteForm::FUNCTION: {
      auto parts = this->nodesOf(node, type->params(), VoidAs::UNDEFINED);
      auto returned = this->nodesOf(node, {type->ret()}, VoidAs::VOID);
      parts.push_back(std::move(returned.front()));

      return parts;
    }
    case SubstituteForm::ITERATOR:
    case SubstituteForm::ASYNC_ITERATOR: {
      auto parts = this->nodesOf(node, {type->yielded()}, VoidAs::UNDEFINED);
      auto rest = this->nodesOf(node, {type->returned(), type->sent()}, VoidAs::VOID);
      llvm::append_range(parts, rest);

      return parts;
    }
    case SubstituteForm::DICT:
      return this->nodesOf(node, {type->keyType(), type->valueType()}, VoidAs::UNDEFINED);
    case SubstituteForm::UNION:
      return this->nodesOf(node, type->members(), VoidAs::UNDEFINED);
    case SubstituteForm::PROMISE:
      return this->nodesOf(node, {type->promised()}, VoidAs::VOID);
    case SubstituteForm::CONSTRUCTOR:
      return this->nodesOf(node, {type->constructed()}, VoidAs::UNDEFINED);
    case SubstituteForm::DESCRIPTOR: {
      const auto *origin = this->state.code.decorators.findDescriptorOrigin(type);
      auto parts = this->nodesOf(node, {origin->args}, VoidAs::UNDEFINED);
      auto returned = this->nodesOf(node, {origin->ret}, VoidAs::VOID);
      parts.push_back(std::move(returned.front()));

      return parts;
    }
    case SubstituteForm::CLASS_INSTANCE:
      return this->nodesOf(node, type->typeArgs(), VoidAs::VOID);
    case SubstituteForm::ALIAS_INSTANCE: {
      const auto *origin = this->state.generics.aliases.findAliasOrigin(type);

      return this->nodesOf(node, origin->args, VoidAs::VOID);
    }
    case SubstituteForm::TUPLE:
      return this->nodesOf(node, this->tupleParts(type), VoidAs::UNDEFINED);
    case SubstituteForm::OBJECT:
      return this->nodesOf(node, this->fieldParts(type), VoidAs::UNDEFINED);
  }

  return {};
}

std::vector<SubstituteNode>
GenericsSubstitutionRegistry::nodesOf(const SubstituteNode &node,
                                      llvm::ArrayRef<const Type *> types, VoidAs voidAs) const {
  auto nodes = std::vector<SubstituteNode>();
  for (const Type *part : types) {
    nodes.push_back(SubstituteNode{
        .type = part,
        .params = node.params,
        .args = node.args,
        .voidAs = voidAs,
    });
  }

  return nodes;
}

std::vector<const Type *> GenericsSubstitutionRegistry::tupleParts(const Type *type) const {
  auto parts = this->fieldParts(type);
  if (!this->hasListParam(type) && type->tupleRest() != nullptr) {
    parts.push_back(type->tupleRest());
  }

  return parts;
}

std::vector<const Type *> GenericsSubstitutionRegistry::fieldParts(const Type *type) const {
  auto parts = std::vector<const Type *>();
  for (const Field &field : type->fields()) {
    parts.push_back(field.type);
  }

  return parts;
}

bool GenericsSubstitutionRegistry::hasListParam(const Type *tuple) const {
  for (const Field &field : tuple->fields()) {
    if (field.type->isListParam()) {
      return true;
    }
  }

  return false;
}

void GenericsSubstitutionRegistry::buildSubstitutedType(const BuildSubstitutedType &task) {
  auto parts = this->state.walk.tasks.takeAnswers(task.count);
  if (this->isTaskForm(task.form)) {
    return this->pushFormTask(task.type, task.form, std::move(parts));
  }
  const auto *built = this->builtType(task.type, task.form, std::move(parts));

  return this->state.walk.tasks.pushAnswer(built);
}

bool GenericsSubstitutionRegistry::isTaskForm(SubstituteForm form) const {
  return form == SubstituteForm::TYPE_OPERATOR || form == SubstituteForm::DESCRIPTOR ||
         form == SubstituteForm::CLASS_INSTANCE || form == SubstituteForm::ALIAS_INSTANCE;
}

void GenericsSubstitutionRegistry::pushFormTask(const Type *type, SubstituteForm form,
                                                std::vector<const Type *> parts) {
  auto &tasks = this->state.walk.tasks;
  if (form == SubstituteForm::TYPE_OPERATOR) {
    return tasks.pushTask(EvalTypeOp{.op = type, .operands = std::move(parts)});
  }
  if (form == SubstituteForm::DESCRIPTOR) {
    return tasks.pushTask(MakeMethodDescriptor{.params = parts[0], .returned = parts[1]});
  }
  if (form == SubstituteForm::CLASS_INSTANCE) {
    const ClassDecl *classDecl = type->classDecl();
    const ClassDecl *owner = classDecl->generic() != nullptr ? classDecl->generic() : classDecl;

    return tasks.pushTask(InstantiateClassType{
        .classDecl = const_cast<ClassDecl *>(owner),
        .args = std::move(parts),
        .loc = this->state.generics.copies.getInstanceLoc(),
    });
  }
  TypeAliasDecl *alias = this->state.generics.aliases.findAliasOrigin(type)->alias;

  return tasks.pushTask(
      InstantiateAlias{.alias = alias, .args = std::move(parts), .loc = alias->loc()});
}

const Type *GenericsSubstitutionRegistry::builtType(const Type *type, SubstituteForm form,
                                                    std::vector<const Type *> parts) {
  switch (form) {
    case SubstituteForm::OPTIONAL:
      return Type::optional(parts[0]);
    case SubstituteForm::ARRAY:
      return Type::array(parts[0]);
    case SubstituteForm::RESULT:
      return Type::result(parts[0], parts[1]);
    case SubstituteForm::FUNCTION:
      return this->substitutedFunction(type, std::move(parts));
    case SubstituteForm::ITERATOR:
      return Type::iterator(parts[0], parts[1], parts[2]);
    case SubstituteForm::DICT:
      return Type::dict(type->dictKind(), parts[0], parts[1]);
    case SubstituteForm::UNION:
      return Type::unionOf(parts);
    case SubstituteForm::PROMISE:
      return Type::promise(parts[0]);
    case SubstituteForm::CONSTRUCTOR:
      return Type::constructorOf(parts[0]);
    case SubstituteForm::ASYNC_ITERATOR:
      return Type::asyncIterator(parts[0], parts[1], parts[2]);
    case SubstituteForm::TUPLE:
      return this->substitutedTuple(type, std::move(parts));
    case SubstituteForm::OBJECT:
      return this->substitutedObject(type, parts);
    case SubstituteForm::TYPE_OPERATOR:
    case SubstituteForm::DESCRIPTOR:
    case SubstituteForm::CLASS_INSTANCE:
    case SubstituteForm::ALIAS_INSTANCE:
      break;
  }

  return Type::error();
}

const Type *
GenericsSubstitutionRegistry::substitutedFunction(const Type *type,
                                                  std::vector<const Type *> parts) const {
  const Type *returnType = parts.back();
  parts.pop_back();

  return Type::function(parts, type->minArgs(), type->hasRest(), returnType, type->hasThis());
}

const Type *GenericsSubstitutionRegistry::substitutedTuple(const Type *type,
                                                           std::vector<const Type *> parts) const {
  bool hasRest = !this->hasListParam(type) && type->tupleRest() != nullptr;
  const Type *rest = hasRest ? parts.back() : nullptr;
  if (hasRest) {
    parts.pop_back();
  }
  auto spread = std::vector<bool>();
  for (const Field &field : type->fields()) {
    spread.push_back(field.type->isListParam());
  }
  if (llvm::is_contained(spread, true)) {
    auto why = std::string();
    const auto *list = ArgumentLists::spreadTuple(parts, spread, why);

    return list != nullptr ? list : Type::error();
  }
  if (type->tupleMin() < parts.size() || type->tupleRest() != nullptr) {
    return Type::argumentList(parts, type->tupleMin(), rest);
  }
  bool isReadonly = !type->fields().empty() && type->fields().front().readonly;

  return Type::tuple(parts, isReadonly);
}

const Type *GenericsSubstitutionRegistry::substitutedObject(const Type *type,
                                                            llvm::ArrayRef<const Type *> parts) {
  this->state.generics.parameters.leaveSubstitution(type);
  auto fields = std::vector<Field>();
  for (size_t index = 0; index < parts.size(); ++index) {
    const Field &field = type->fields()[index];
    auto substituted = Field{.name = field.name, .type = parts[index], .readonly = field.readonly};
    fields.push_back(substituted);
  }

  return Type::object(std::move(fields));
}

void GenericsSubstitutionRegistry::substituteInstanceFields(const SubstituteInstanceFields &task) {
  return this->nextInstanceField(NextInstanceField{.instance = task.instance, .index = 0});
}

void GenericsSubstitutionRegistry::nextInstanceField(const NextInstanceField &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  auto index = this->findUnresolvedField(instance, task.index);
  if (!index.has_value()) {
    return;
  }
  const Type *original = instance->generic()->fields[*index].resolved;
  if (original == nullptr) {
    instance->fields[*index].resolved = Type::error();

    return tasks.pushTask(NextInstanceField{.instance = instance, .index = *index + 1});
  }
  tasks.pushTask(AfterInstanceField{.instance = instance, .index = *index});

  return tasks.pushTask(InstanceValueType{.instance = instance, .type = original});
}

std::optional<size_t> GenericsSubstitutionRegistry::findUnresolvedField(const ClassDecl *instance,
                                                                        size_t from) const {
  const ClassDecl *generic = instance->generic();
  for (size_t index = from; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    const ClassField &field = instance->fields[index];
    bool isUnresolved = !field.isStatic() && field.method() == nullptr && field.resolved == nullptr;
    if (isUnresolved) {
      return index;
    }
  }

  return std::nullopt;
}

void GenericsSubstitutionRegistry::afterInstanceField(const AfterInstanceField &task) {
  ClassDecl *instance = task.instance;
  instance->fields[task.index].resolved = this->state.walk.tasks.takeAnswer();

  return this->nextInstanceField(NextInstanceField{.instance = instance, .index = task.index + 1});
}

}
