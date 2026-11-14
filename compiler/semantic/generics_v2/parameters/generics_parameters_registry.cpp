#include "compiler/semantic/generics_v2/parameters/generics_parameters_registry.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

GenericsParametersRegistry::GenericsParametersRegistry(SemanticState &owner)
    : state(owner) {}

void GenericsParametersRegistry::declareClassTypeParams(const DeclareClassTypeParams &task) {
  ClassDecl *classDecl = task.classDecl;
  if (!classDecl->isGeneric() || !classDecl->typeArgs.empty()) {
    return;
  }
  auto &scopes = this->state.walk.scopes;
  auto topTypes = scopes.getTopTypes();
  scopes.enterOnlyTypes(topTypes);
  auto *here = this->state.declaration.modules.getCurrentModule();
  auto *home = this->state.declaration.modules.findDeclModule(classDecl);
  bool isMoved = home != nullptr && home != here && scopes.isTopLevel();
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterClassTypeParams{.classDecl = classDecl, .here = here, .isMoved = isMoved});
  tasks.pushTask(StartTypeParams{.decls = classDecl->typeParams, .isScoped = true});
  if (isMoved) {
    return tasks.pushTask(UseModule{.module = home});
  }
}

void GenericsParametersRegistry::afterClassTypeParams(const AfterClassTypeParams &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  size_t count = classDecl->typeParams.size();
  auto defaults = tasks.takeAnswers(count);
  classDecl->typeArgs = tasks.takeAnswers(count);
  this->state.walk.scopes.closeTypeScope();
  tasks.pushTask(LeaveClassTypeParams{.classDecl = classDecl, .defaults = std::move(defaults)});
  if (task.isMoved) {
    return tasks.pushTask(UseModule{.module = task.here});
  }
}

void GenericsParametersRegistry::leaveClassTypeParams(const LeaveClassTypeParams &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &copies = this->state.generics.copies;
  this->state.walk.scopes.leaveOnlyTypes();
  copies.rememberClassDefaults(classDecl, task.defaults);
  auto *type = const_cast<Type *>(classDecl->type);
  type->setTypeArgs(classDecl->typeArgs);
  auto params = GenericCheckHelpers::typeNames(classDecl->typeArgs, ", ");
  type->rename(classDecl->name.str() + "<" + params + ">");
  copies.rememberClassInstance(classDecl, classDecl->typeArgs, classDecl);
}

void GenericsParametersRegistry::declareFunctionTypeParams(const DeclareFunctionTypeParams &task) {
  auto &tasks = this->state.walk.tasks;
  FunctionDecl *functionDecl = task.functionDecl;
  tasks.pushTask(AfterFunctionTypeParams{.functionDecl = functionDecl, .function = task.function});

  return tasks.pushTask(StartTypeParams{.decls = functionDecl->typeParams, .isScoped = false});
}

void GenericsParametersRegistry::afterFunctionTypeParams(const AfterFunctionTypeParams &task) {
  auto &tasks = this->state.walk.tasks;
  FunctionDecl *functionDecl = task.functionDecl;
  FuncSymbol *function = task.function;
  size_t count = functionDecl->typeParams.size();
  function->typeDefaults = tasks.takeAnswers(count);
  function->typeParams = tasks.takeAnswers(count);
  functionDecl->typeArgs = function->typeParams;
}

void GenericsParametersRegistry::declareAliasTypeParams(const DeclareAliasTypeParams &task) {
  TypeAliasDecl *alias = task.alias;
  if (this->state.generics.aliases.findAliasParams(alias).has_value()) {
    return;
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterAliasTypeParams{.alias = alias});

  return tasks.pushTask(StartTypeParams{.decls = alias->typeParams, .isScoped = true});
}

void GenericsParametersRegistry::afterAliasTypeParams(const AfterAliasTypeParams &task) {
  TypeAliasDecl *alias = task.alias;
  auto &tasks = this->state.walk.tasks;
  size_t count = alias->typeParams.size();
  auto defaults = tasks.takeAnswers(count);
  auto params = tasks.takeAnswers(count);
  this->state.walk.scopes.closeTypeScope();
  auto declared = AliasParams{.params = std::move(params), .defaults = std::move(defaults)};
  this->state.generics.aliases.rememberAliasParams(alias, std::move(declared));
}

void GenericsParametersRegistry::bindClassTypeParams(const BindClassTypeParams &task) {
  const ClassDecl *classDecl = task.classDecl;
  if (classDecl == nullptr || classDecl->typeParams.empty()) {
    return;
  }
  if (classDecl->typeParams.size() == classDecl->typeArgs.size()) {
    return this->bindTypeParams(classDecl->typeParams, classDecl->typeArgs);
  }
}

void GenericsParametersRegistry::bindFunctionTypeParams(const BindFunctionTypeParams &task) {
  const FunctionDecl *functionDecl = task.functionDecl;

  return this->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);
}

void GenericsParametersRegistry::bindTypeParams(llvm::ArrayRef<TypeParamDecl> decls,
                                                llvm::ArrayRef<const Type *> types) {
  size_t count = std::min(decls.size(), types.size());
  for (size_t index = 0; index < count; ++index) {
    const TypeParamDecl &typeParam = decls[index];
    auto *alias = this->state.ast.make<TypeAliasDecl>(typeParam.loc, typeParam.name, TypeRef());
    alias->resolved = types[index];
    alias->boundParam = true;
    this->state.walk.scopes.setType(typeParam.name, alias);
  }
}

void GenericsParametersRegistry::startTypeParams(const StartTypeParams &task) {
  if (task.isScoped) {
    this->state.walk.scopes.openTypeScope();
  }
  auto draft = TypeParamsDraft{
      .decls = task.decls,
      .created = this->createTypeParams(task.decls),
      .defaults = {},
      .constraint = nullptr,
      .alternatives = {},
      .isResolved = true,
      .index = 0,
      .member = 0,
      .isAfterDefault = false,
  };
  this->bindTypeParams(task.decls, this->paramsOf(draft));

  return this->nextTypeParam(NextTypeParam{.draft = std::move(draft)});
}

std::vector<const Type *> GenericsParametersRegistry::paramsOf(const TypeParamsDraft &draft) const {
  return std::vector<const Type *>(draft.created.begin(), draft.created.end());
}

std::vector<Type *>
GenericsParametersRegistry::createTypeParams(llvm::ArrayRef<TypeParamDecl> decls) {
  auto created = std::vector<Type *>();
  for (size_t index = 0; index < decls.size(); ++index) {
    this->walkParamName(decls, index);
    created.push_back(Type::param(decls[index].name));
  }

  return created;
}

void GenericsParametersRegistry::walkParamName(llvm::ArrayRef<TypeParamDecl> decls, size_t index) {
  const TypeParamDecl &typeParam = decls[index];
  for (const TypeParamDecl &earlier : decls.take_front(index)) {
    auto unique = this->uniqueName(earlier, typeParam);
    if (unique.isErr()) {
      this->state.diagnostics.error(typeParam.loc, unique.error().message);
    }
  }
  auto own = this->ownName(typeParam);
  if (own.isErr()) {
    this->state.diagnostics.error(typeParam.loc, own.error().message);
  }
}

void GenericsParametersRegistry::nextTypeParam(const NextTypeParam &task) {
  TypeParamsDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == draft.decls.size()) {
    for (const Type *param : this->paramsOf(draft)) {
      tasks.pushAnswer(param);
    }
    for (const Type *fallback : draft.defaults) {
      tasks.pushAnswer(fallback);
    }
    return;
  }
  const TypeParamDecl &typeParam = draft.decls[draft.index];
  draft.constraint = nullptr;
  draft.alternatives = {};
  draft.isResolved = true;
  draft.member = 0;
  if (!typeParam.constraint.has_value()) {
    return this->startDefault(std::move(draft));
  }
  const TypeRef &ref = *typeParam.constraint;
  if (this->isUnknownList(ref)) {
    draft.created[draft.index]->setList();

    return this->applyConstraint(std::move(draft));
  }
  if (ref.kind() == TypeRefKind::NAME_REF && TypeNameTable::isName(ref.name, TypeName::UNKNOWN)) {
    return this->applyConstraint(std::move(draft));
  }
  if (ref.kind() == TypeRefKind::UNION_REF) {
    return this->nextConstraintMember(NextConstraintMember{.draft = std::move(draft)});
  }
  tasks.pushTask(AfterConstraintType{.draft = std::move(draft)});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &ref, .use = TypeUse::STANDALONE});
}

bool GenericsParametersRegistry::isUnknownList(const TypeRef &ref) const {
  if (ref.kind() != TypeRefKind::ARRAY_REF || ref.args[0].kind() != TypeRefKind::NAME_REF) {
    return false;
  }

  return TypeNameTable::isName(ref.args[0].name, TypeName::UNKNOWN);
}

void GenericsParametersRegistry::afterConstraintType(const AfterConstraintType &task) {
  TypeParamsDraft draft = task.draft;
  draft.constraint = this->state.walk.tasks.takeAnswer();
  draft.isResolved = !draft.constraint->isError();

  return this->applyConstraint(std::move(draft));
}

void GenericsParametersRegistry::nextConstraintMember(const NextConstraintMember &task) {
  TypeParamsDraft draft = task.draft;
  const TypeRef &ref = *draft.decls[draft.index].constraint;
  if (draft.member < ref.args.size()) {
    const TypeRef &member = ref.args[draft.member];
    auto &tasks = this->state.walk.tasks;
    tasks.pushTask(AfterConstraintMember{.draft = std::move(draft)});

    return tasks.pushTask(ResolveTypeAnswer{.ref = &member, .use = TypeUse::STANDALONE});
  }
  const std::vector<const Type *> &alternatives = draft.alternatives;
  bool isOptionalPair = draft.isResolved && alternatives.size() == 2 &&
                        (alternatives[0]->isUndefined() || alternatives[1]->isUndefined());
  if (isOptionalPair) {
    const Type *defined = alternatives[0]->isUndefined() ? alternatives[1] : alternatives[0];
    draft.constraint = Type::optional(defined);
  }

  return this->applyConstraint(std::move(draft));
}

void GenericsParametersRegistry::afterConstraintMember(const AfterConstraintMember &task) {
  TypeParamsDraft draft = task.draft;
  const Type *type = this->state.walk.tasks.takeAnswer();
  if (type->isError()) {
    draft.isResolved = false;
  }
  if (!llvm::is_contained(draft.alternatives, type)) {
    draft.alternatives.push_back(type);
  }
  draft.member += 1;

  return this->nextConstraintMember(NextConstraintMember{.draft = std::move(draft)});
}

void GenericsParametersRegistry::applyConstraint(TypeParamsDraft draft) {
  const TypeParamDecl &typeParam = draft.decls[draft.index];
  if (!draft.isResolved) {
    return this->startDefault(std::move(draft));
  }
  auto alone = this->checkConstraintsAlone(draft, typeParam);
  if (alone.isErr()) {
    this->state.diagnostics.error(typeParam.constraint->loc, alone.error().message);

    return this->startDefault(std::move(draft));
  }
  std::vector<const Type *> alternatives =
      draft.constraint != nullptr ? std::vector<const Type *>() : draft.alternatives;
  draft.created[draft.index]->setConstraint(draft.constraint, alternatives);

  return this->startDefault(std::move(draft));
}

GenericsParametersRegistry::ResultConstraintsKept
GenericsParametersRegistry::checkConstraintsAlone(const TypeParamsDraft &draft,
                                                  const TypeParamDecl &typeParam) const {
  llvm::ArrayRef<const Type *> all = draft.constraint != nullptr
                                         ? llvm::ArrayRef<const Type *>(draft.constraint)
                                         : llvm::ArrayRef<const Type *>(draft.alternatives);
  for (const Type *type : all) {
    auto kept = this->constraintAlone(type, typeParam);
    if (kept.isErr()) {
      return err(kept.error());
    }
  }

  return ok();
}

void GenericsParametersRegistry::startDefault(TypeParamsDraft draft) {
  const TypeParamDecl &typeParam = draft.decls[draft.index];
  if (!typeParam.defaultType.has_value()) {
    draft.defaults.push_back(nullptr);

    return this->finishTypeParam(std::move(draft));
  }
  auto &tasks = this->state.walk.tasks;
  const TypeRef &ref = *typeParam.defaultType;
  tasks.pushTask(AfterDefaultType{.draft = std::move(draft)});

  return tasks.pushTask(ResolveTypeAnswer{.ref = &ref, .use = TypeUse::STANDALONE});
}

void GenericsParametersRegistry::afterDefaultType(const AfterDefaultType &task) {
  TypeParamsDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  const Type *fallback = tasks.takeAnswer();
  if (fallback->isError()) {
    draft.defaults.push_back(fallback);

    return this->finishTypeParam(std::move(draft));
  }
  const Type *param = draft.created[draft.index];
  tasks.pushTask(AfterDefaultConstraint{.draft = std::move(draft), .fallback = fallback});

  return tasks.pushTask(CheckConstraint{.arg = fallback, .param = param, .params = {}, .args = {}});
}

void GenericsParametersRegistry::afterDefaultConstraint(const AfterDefaultConstraint &task) {
  TypeParamsDraft draft = task.draft;
  bool isSatisfied = this->state.walk.tasks.takeVerdict();
  const TypeParamDecl &typeParam = draft.decls[draft.index];
  const Type *param = draft.created[draft.index];
  auto fits = this->defaultFits(task.fallback, param, typeParam, isSatisfied);
  if (fits.isErr()) {
    this->state.diagnostics.error(typeParam.defaultType->loc, fits.error().message);
    draft.defaults.push_back(Type::error());

    return this->finishTypeParam(std::move(draft));
  }
  draft.defaults.push_back(task.fallback);

  return this->finishTypeParam(std::move(draft));
}

GenericsParametersRegistry::ResultDefaultSatisfy
GenericsParametersRegistry::defaultFits(const Type *fallback, const Type *param,
                                        const TypeParamDecl &typeParam, bool isSatisfied) const {
  if (!isSatisfied) {
    auto constraint = GenericCheckHelpers::constraintSpelling(param, &typeParam);
    auto message = ErrorTextDefaultFits::unsatisfied(fallback->name(), typeParam.name, constraint);

    return err(GenericsParametersRegistryDefaultFitsError{
        .code = GenericsParametersRegistryDefaultFitsErrorCode::DEFAULT_UNSATISFIED,
        .message = message,
    });
  }

  return ok(fallback);
}

void GenericsParametersRegistry::finishTypeParam(TypeParamsDraft draft) {
  const TypeParamDecl &typeParam = draft.decls[draft.index];
  if (typeParam.defaultType.has_value()) {
    draft.isAfterDefault = true;
  }
  auto present = this->defaultGiven(typeParam, draft.isAfterDefault);
  if (present.isErr()) {
    this->state.diagnostics.error(typeParam.loc, present.error().message);
  }
  draft.index += 1;

  return this->nextTypeParam(NextTypeParam{.draft = std::move(draft)});
}

GenericsParametersRegistry::ResultParamNameIsNew
GenericsParametersRegistry::uniqueName(const TypeParamDecl &earlier,
                                       const TypeParamDecl &typeParam) const {
  if (earlier.name == typeParam.name) {
    auto message = ErrorTextUniqueParam::duplicate(typeParam.name);

    return err(GenericsParametersRegistryUniqueNameError{
        .code = GenericsParametersRegistryUniqueNameErrorCode::DUPLICATE_TYPE_PARAMETER,
        .message = message,
    });
  }

  return ok(&typeParam);
}

GenericsParametersRegistry::ResultNotBuiltInName
GenericsParametersRegistry::ownName(const TypeParamDecl &typeParam) const {
  if (Type::fromName(typeParam.name) != nullptr) {
    auto message = ErrorTextOwnName::builtin(typeParam.name);

    return err(GenericsParametersRegistryOwnNameError{
        .code = GenericsParametersRegistryOwnNameErrorCode::BUILTIN_NAME,
        .message = message,
    });
  }

  return ok(&typeParam);
}

GenericsParametersRegistry::ResultConstraintKept
GenericsParametersRegistry::constraintAlone(const Type *type,
                                            const TypeParamDecl &typeParam) const {
  if (type->isUndefined()) {
    auto message = ErrorTextConstraint::undefined(typeParam.name);

    return err(GenericsParametersRegistryConstraintAloneError{
        .code = GenericsParametersRegistryConstraintAloneErrorCode::UNDEFINED_CONSTRAINT,
        .message = message,
    });
  }

  return ok(type);
}

GenericsParametersRegistry::ResultDefaultPresent
GenericsParametersRegistry::defaultGiven(const TypeParamDecl &typeParam,
                                         bool isAfterDefault) const {
  if (!typeParam.defaultType.has_value() && isAfterDefault) {
    auto message = ErrorTextDefaultGiven::missing(typeParam.name);

    return err(GenericsParametersRegistryDefaultGivenError{
        .code = GenericsParametersRegistryDefaultGivenErrorCode::MISSING_DEFAULT,
        .message = message,
    });
  }

  return ok(&typeParam);
}

}
