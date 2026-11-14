#include "compiler/semantic/generics_v2/aliases/generics_aliases_registry.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

namespace {

constexpr unsigned kMaxAliasDepth = 50;
constexpr unsigned kMaxTailCalls = 1000;

}

GenericsAliasesRegistry::GenericsAliasesRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsAliasesRegistry::instantiateOwnAlias(const InstantiateOwnAlias &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterOwnAliasParams{.alias = task.alias});

  return tasks.pushTask(DeclareAliasTypeParams{.alias = task.alias});
}

void GenericsAliasesRegistry::afterOwnAliasParams(const AfterOwnAliasParams &task) {
  TypeAliasDecl *alias = task.alias;
  auto &tasks = this->state.walk.tasks;
  std::vector<const Type *> params = this->state.generics.aliases.findAliasParams(alias)->params;
  tasks.pushTask(DropOwnAliasAnswer{});

  return tasks.pushTask(
      InstantiateAlias{.alias = alias, .args = std::move(params), .loc = alias->loc()});
}

void GenericsAliasesRegistry::dropOwnAliasAnswer() {
  this->state.walk.tasks.takeAnswer();
}

void GenericsAliasesRegistry::instantiateAlias(const InstantiateAlias &task) {
  auto &aliases = this->state.generics.aliases;
  if (aliases.getAliasDepth() == 0) {
    aliases.rememberAliasUse(task.loc);
  }
  auto draft = AliasDraft{
      .alias = task.alias,
      .args = task.args,
      .loc = task.loc,
      .params = {},
      .defaults = {},
      .chain = {},
      .entry = nullptr,
      .display = "",
      .calls = 0,
      .index = 0,
      .isMoved = false,
  };

  return this->startAliasStep(std::move(draft));
}

void GenericsAliasesRegistry::startAliasStep(AliasDraft draft) {
  auto &tasks = this->state.walk.tasks;
  TypeAliasDecl *alias = draft.alias;
  tasks.pushTask(AfterAliasParams{.draft = std::move(draft)});

  return tasks.pushTask(DeclareAliasTypeParams{.alias = alias});
}

void GenericsAliasesRegistry::afterAliasParams(const AfterAliasParams &task) {
  AliasDraft draft = task.draft;
  AliasParams declared = *this->state.generics.aliases.findAliasParams(draft.alias);
  draft.params = declared.params;
  draft.defaults = declared.defaults;
  auto counted = this->argumentCount(draft.alias, draft.params.size(), draft.args.size());
  if (counted.isErr()) {
    this->state.diagnostics.error(draft.loc, counted.error().message);

    return this->finishAlias(draft, Type::error());
  }

  return this->nextAliasDefault(std::move(draft));
}

void GenericsAliasesRegistry::nextAliasDefault(AliasDraft draft) {
  size_t index = draft.args.size();
  if (index == draft.params.size()) {
    draft.index = 0;

    return this->nextAliasMatch(std::move(draft));
  }
  auto exists = this->defaultExists(draft.alias, draft.params.size(), draft.defaults[index]);
  if (exists.isErr()) {
    this->state.diagnostics.error(draft.loc, exists.error().message);

    return this->finishAlias(draft, Type::error());
  }
  auto &tasks = this->state.walk.tasks;
  auto fill =
      SubstituteType{.type = draft.defaults[index], .params = draft.params, .args = draft.args};
  tasks.pushTask(AfterAliasDefault{.draft = std::move(draft)});

  return tasks.pushTask(std::move(fill));
}

void GenericsAliasesRegistry::afterAliasDefault(const AfterAliasDefault &task) {
  AliasDraft draft = task.draft;
  const Type *filled = this->state.walk.tasks.takeAnswer();
  draft.args.push_back(filled);

  return this->nextAliasDefault(std::move(draft));
}

void GenericsAliasesRegistry::nextAliasMatch(AliasDraft draft) {
  size_t index = draft.index;
  if (index == draft.params.size()) {
    return this->startAliasBody(std::move(draft));
  }
  const Type *arg = draft.args[index];
  if (arg->isError()) {
    return this->finishAlias(draft, arg);
  }
  auto &tasks = this->state.walk.tasks;
  auto check = CheckConstraint{
      .arg = arg,
      .param = draft.params[index],
      .params = draft.params,
      .args = draft.args,
  };
  tasks.pushTask(AfterAliasMatch{.draft = std::move(draft)});

  return tasks.pushTask(std::move(check));
}

void GenericsAliasesRegistry::afterAliasMatch(const AfterAliasMatch &task) {
  AliasDraft draft = task.draft;
  bool isSatisfied = this->state.walk.tasks.takeVerdict();
  auto matched = this->argumentMatch(draft, isSatisfied);
  if (matched.isErr()) {
    this->state.diagnostics.error(draft.loc, matched.error().message);

    return this->finishAlias(draft, Type::error());
  }
  draft.index += 1;

  return this->nextAliasMatch(std::move(draft));
}

GenericsAliasesRegistry::ResultArgumentMatch
GenericsAliasesRegistry::argumentMatch(const AliasDraft &draft, bool isSatisfied) const {
  size_t index = draft.index;
  const Type *arg = draft.args[index];
  if (!isSatisfied) {
    const Type *param = draft.params[index];
    auto constraint =
        GenericCheckHelpers::constraintSpelling(param, &draft.alias->typeParams[index]);
    auto message =
        ErrorTextAliasMatch::unsatisfied(arg->name(), constraint, param->name(), draft.alias->name);

    return err(GenericsAliasesRegistryArgumentMatchError{
        .code = GenericsAliasesRegistryArgumentMatchErrorCode::CONSTRAINT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(arg);
}

void GenericsAliasesRegistry::startAliasBody(AliasDraft draft) {
  TypeAliasDecl *alias = draft.alias;
  auto &aliases = this->state.generics.aliases;
  AliasInstance &entry = *aliases.recordAliasInstance(alias, draft.args);
  const auto *reused = this->reusedInstance(entry);
  if (reused != nullptr) {
    return this->finishAlias(draft, reused);
  }
  if (!this->isAliasDepthFitting(alias, entry, draft.calls)) {
    return this->finishAlias(draft, Type::error());
  }
  draft.display = this->aliasDisplay(draft);
  entry.resolving = true;
  draft.entry = &entry;
  draft.chain.push_back(&entry);
  aliases.enterAlias();
  auto *home = this->state.declaration.modules.findDeclModule(alias);
  draft.isMoved = home != nullptr && home != this->state.declaration.modules.getCurrentModule();
  if (draft.isMoved) {
    const auto &homeTypes = this->state.declaration.modules.getModuleState(home).types;
    this->state.walk.scopes.enterOnlyTypes(homeTypes);
  }
  this->state.walk.scopes.openTypeScope();
  this->bindAliasParams(alias, draft.args);
  auto &tasks = this->state.walk.tasks;
  if (alias->type.kind() == TypeRefKind::OBJECT_REF) {
    entry.self = Type::declareObject(draft.display);
    bool hasBases = !alias->bases.empty();
    tasks.pushTask(AfterAliasObject{.draft = std::move(draft)});
    if (hasBases) {
      return tasks.pushTask(StartInterfaceFields{.alias = alias});
    }

    return tasks.pushTask(ResolveObjectFields{.ref = &alias->type});
  }
  bool isConditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  if (isConditional) {
    this->state.types.context.allowTailCall();
  } else {
    this->state.types.context.forbidTailCall();
  }
  tasks.pushTask(AfterAliasInner{.draft = std::move(draft)});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &alias->type, .use = TypeUse::INNER});
}

void GenericsAliasesRegistry::bindAliasParams(const TypeAliasDecl *alias,
                                              llvm::ArrayRef<const Type *> types) {
  size_t count = std::min(alias->typeParams.size(), types.size());
  for (size_t index = 0; index < count; ++index) {
    const TypeParamDecl &typeParam = alias->typeParams[index];
    auto *bound = this->state.ast.make<TypeAliasDecl>(typeParam.loc, typeParam.name, TypeRef());
    bound->resolved = types[index];
    bound->boundParam = true;
    this->state.walk.scopes.setType(typeParam.name, bound);
  }
}

void GenericsAliasesRegistry::afterAliasObject(const AfterAliasObject &task) {
  const AliasDraft &draft = task.draft;
  ObjectTypeRef *shape = draft.alias->type.form.as<ObjectTypeRef>();
  std::vector<const Type *> bases =
      draft.alias->bases.empty() ? std::vector<const Type *>() : shape->resolvedBases;
  const Type *type = this->definedObject(draft, shape->resolvedFields, shape->resolvedOverloads);
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(CloseAliasBody{.draft = draft, .type = type});
  if (type->isError()) {
    return;
  }
  for (const Type *base : llvm::reverse(bases)) {
    tasks.pushTask(AddBase{.type = type, .base = base});
  }
}

const Type *GenericsAliasesRegistry::definedObject(const AliasDraft &draft,
                                                   std::vector<Field> fields,
                                                   std::vector<Overload> overloads) const {
  const AliasInstance &entry = *draft.entry;
  if (entry.recursive) {
    entry.self->defineFields(std::move(fields), std::move(overloads));

    return entry.self;
  }

  return Type::object(std::move(fields), draft.display, std::move(overloads));
}

void GenericsAliasesRegistry::afterAliasInner(const AfterAliasInner &task) {
  this->state.types.context.forbidTailCall();
  const Type *type = this->state.walk.tasks.takeAnswer();

  return this->closeAliasBody(CloseAliasBody{.draft = task.draft, .type = type});
}

void GenericsAliasesRegistry::closeAliasBody(const CloseAliasBody &task) {
  AliasDraft draft = task.draft;
  this->state.walk.scopes.closeTypeScope();
  if (draft.isMoved) {
    this->state.walk.scopes.leaveOnlyTypes();
  }
  this->state.generics.aliases.leaveAlias();
  const Type *type = task.type;
  auto &types = this->state.types.context;
  if (type == nullptr && types.hasTailCall()) {
    auto tail = types.takeTailCall();
    draft.alias = tail.alias;
    draft.args = std::move(tail.args);
    draft.loc = draft.alias->loc();
    draft.calls += 1;

    return this->startAliasStep(std::move(draft));
  }
  if (type == nullptr) {
    type = Type::error();
  }
  this->rememberAliasType(draft, type);

  return this->finishAlias(draft, type);
}

void GenericsAliasesRegistry::finishAlias(const AliasDraft &draft, const Type *type) {
  for (AliasInstance *entry : draft.chain) {
    entry->resolving = false;
    entry->type = type;
  }

  return this->state.walk.tasks.pushAnswer(type);
}

GenericsAliasesRegistry::ResultArgumentCount
GenericsAliasesRegistry::argumentCount(const TypeAliasDecl *alias, size_t count, size_t got) const {
  if (got > count) {
    auto message = ErrorTextAliasCount::tooMany(alias->name, count, got);

    return err(GenericsAliasesRegistryArgumentCountError{
        .code = GenericsAliasesRegistryArgumentCountErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(got);
}

GenericsAliasesRegistry::ResultDefaultExists
GenericsAliasesRegistry::defaultExists(const TypeAliasDecl *alias, size_t count,
                                       const Type *fallback) const {
  if (fallback == nullptr) {
    auto message = ErrorTextAliasDefault::needs(alias->name, count);

    return err(GenericsAliasesRegistryDefaultExistsError{
        .code = GenericsAliasesRegistryDefaultExistsErrorCode::TYPE_ARGUMENTS_MISSING,
        .message = message,
    });
  }

  return ok(fallback);
}

GenericsAliasesRegistry::ResultAliasDistinct
GenericsAliasesRegistry::notResolving(const TypeAliasDecl *alias,
                                      const AliasInstance &entry) const {
  if (entry.resolving && entry.self == nullptr) {
    auto message = ErrorTextAliasSelf::itself(alias->name);

    return err(GenericsAliasesRegistryNotResolvingError{
        .code = GenericsAliasesRegistryNotResolvingErrorCode::REFERS_TO_ITSELF,
        .message = message,
    });
  }

  return ok(alias);
}

GenericsAliasesRegistry::ResultDepthInBounds
GenericsAliasesRegistry::depthInBounds(const TypeAliasDecl *alias, unsigned calls,
                                       bool isConditional) const {
  bool isDeep =
      this->state.generics.aliases.getAliasDepth() >= kMaxAliasDepth || calls > kMaxTailCalls;
  if (isDeep && !isConditional) {
    auto message = ErrorTextAliasDepth::endless(alias->name);

    return err(GenericsAliasesRegistryDepthInBoundsError{
        .code = GenericsAliasesRegistryDepthInBoundsErrorCode::ENDLESS_ALIAS,
        .message = message,
    });
  }
  if (isDeep && calls > kMaxTailCalls) {
    auto message = ErrorTextAliasDepth::tailCalls(alias->name, kMaxTailCalls);

    return err(GenericsAliasesRegistryDepthInBoundsError{
        .code = GenericsAliasesRegistryDepthInBoundsErrorCode::TOO_MANY_TAIL_CALLS,
        .message = message,
    });
  }
  if (isDeep) {
    auto message = ErrorTextAliasDepth::nests(alias->name, kMaxAliasDepth, kMaxTailCalls);

    return err(GenericsAliasesRegistryDepthInBoundsError{
        .code = GenericsAliasesRegistryDepthInBoundsErrorCode::TOO_DEEP,
        .message = message,
    });
  }

  return ok(calls);
}

const Type *GenericsAliasesRegistry::reusedInstance(AliasInstance &entry) const {
  if (entry.type != nullptr) {
    return entry.type;
  }
  if (entry.resolving && entry.self != nullptr) {
    entry.recursive = true;

    return entry.self;
  }

  return nullptr;
}

bool GenericsAliasesRegistry::isAliasDepthFitting(const TypeAliasDecl *alias,
                                                  const AliasInstance &entry, unsigned calls) {
  auto distinct = this->notResolving(alias, entry);
  if (distinct.isErr()) {
    this->state.diagnostics.error(alias->loc(), distinct.error().message);

    return false;
  }
  bool isConditional = alias->type.kind() == TypeRefKind::CONDITIONAL_REF;
  auto bounded = this->depthInBounds(alias, calls, isConditional);
  if (bounded.isOk()) {
    return true;
  }
  auto neededHere = ErrorTextAliasDepth::neededHere();
  this->state.diagnostics.error(alias->loc(), bounded.error().message);
  auto use = this->state.generics.aliases.getAliasUse();
  if (isConditional && use.isValid() && use != alias->loc()) {
    this->state.diagnostics.note(use, neededHere);
  }

  return false;
}

std::string GenericsAliasesRegistry::aliasDisplay(const AliasDraft &draft) const {
  size_t shown = GenericCheckHelpers::shownArgumentCount(draft.defaults, draft.args);
  if (shown == 0) {
    return draft.alias->name.str();
  }
  auto shownArgs = llvm::ArrayRef<const Type *>(draft.args).take_front(shown);
  auto list = GenericCheckHelpers::typeNames(shownArgs, ", ");

  return draft.alias->name.str() + "<" + list + ">";
}

void GenericsAliasesRegistry::rememberAliasType(const AliasDraft &draft, const Type *type) {
  auto &aliases = this->state.generics.aliases;
  if (type->isObject() && !type->isError() && aliases.findAliasOrigin(type) == nullptr) {
    aliases.rememberAliasOrigin(type, draft.alias, draft.args);
  }
  bool isBuiltin =
      this->state.declaration.prelude.findBuiltinAlias(draft.alias->name) == draft.alias;
  if (type->isUnion() && isBuiltin) {
    Type::nameUnion(type, draft.display);
  }
}

}
