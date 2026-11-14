#include "compiler/semantic/generics_v2/functions/function_instances/function_instances.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/types/argument_lists/argument_lists.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

namespace {

constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

FunctionInstances::FunctionInstances(SemanticState &owner,
                                     FunctionInstanceRules &sharedFunctionInstanceRules)
    : state(owner),
      functionInstanceRules(sharedFunctionInstanceRules) {}

void FunctionInstances::instantiateCallTarget(const InstantiateCallTarget &task) {
  return this->functionInstanceRules.startInstantiate(InstantiateDraft{
      .generic = task.generic,
      .args = task.typeArgs,
      .loc = task.loc,
      .exact = task.exact,
      .reps = {},
      .name = nullptr,
      .call = task.call,
      .isCopyingReturn = task.isCopyingReturn,
      .instance = nullptr,
      .function = nullptr,
      .copy = nullptr,
      .index = 0,
      .isPrecise = false,
  });
}

void FunctionInstances::afterReusedWalk(const AfterReusedWalk &task) {
  const InstantiateDraft &draft = task.draft;
  InstanceRecord *instance = draft.instance;
  bool isReportDue = this->state.generics.copies.isCheckingInstance() || !instance->isReported;
  this->finishInstantiate(draft, instance->decl->sym);
  if (instance->failed && isReportDue) {
    return this->state.walk.tasks.pushTask(
        ReportInstanceAt{.instance = instance, .loc = draft.loc, .args = draft.args});
  }
}

void FunctionInstances::nextMeaningfulParam(const NextMeaningfulParam &task) {
  InstantiateDraft draft = task.draft;
  FuncSymbol *generic = draft.generic;
  while (draft.index < generic->params.size() && generic->params[draft.index] == nullptr) {
    draft.index += 1;
  }
  if (draft.index == generic->params.size()) {
    draft.index = 0;

    return this->startInstanceDecl(std::move(draft));
  }
  auto &tasks = this->state.walk.tasks;
  auto substitute = SubstituteType{
      .type = generic->params[draft.index],
      .params = generic->typeParams,
      .args = draft.reps,
  };
  tasks.pushTask(AfterMeaningfulParam{.draft = std::move(draft)});

  return tasks.pushTask(std::move(substitute));
}

void FunctionInstances::afterMeaningfulParam(const AfterMeaningfulParam &task) {
  InstantiateDraft draft = task.draft;
  const Type *substituted = this->state.walk.tasks.takeAnswer();
  auto meaningful = this->paramMeaningful(draft, substituted);
  if (meaningful.isOk()) {
    draft.index += 1;

    return this->nextMeaningfulParam(NextMeaningfulParam{.draft = std::move(draft)});
  }
  if (this->state.generics.copies.isCheckingInstance()) {
    return;
  }

  return this->state.diagnostics.error(draft.loc, meaningful.error().message);
}

FunctionInstances::ResultParamsMeaning
FunctionInstances::paramMeaningful(const InstantiateDraft &draft, const Type *substituted) const {
  FuncSymbol *generic = draft.generic;
  if (substituted->isError()) {
    const Type *type = generic->params[draft.index];
    auto bindings = GenericCheckHelpers::bindingNames(generic->typeParams, draft.args);
    auto message = ErrorTextInstanceParams::meaningless(generic->name, bindings, type->name());

    return err(GenericsFunctionsRegistryParamsMeaningfulError{
        .code = GenericsFunctionsRegistryParamsMeaningfulErrorCode::PARAMS_MEANINGLESS,
        .message = message,
    });
  }

  return ok(generic);
}

void FunctionInstances::startInstanceDecl(InstantiateDraft draft) {
  auto &copies = this->state.generics.copies;
  InstanceRecord *instance = copies.addInstanceRecord();
  instance->generic = draft.generic;
  instance->args = draft.reps;
  instance->concrete.assign(draft.args.begin(), draft.args.end());
  instance->exact.assign(draft.exact.begin(), draft.exact.end());
  instance->callLoc = draft.loc;
  instance->requester = copies.getCheckingInstance();
  draft.instance = instance;

  return this->buildInstanceDecl(std::move(draft));
}

void FunctionInstances::walkPreciseCopy(const WalkPreciseCopy &task) {
  InstanceRecord *instance = task.instance;

  return this->buildInstanceDecl(InstantiateDraft{
      .generic = instance->generic,
      .args = instance->concrete,
      .loc = instance->callLoc,
      .exact = instance->exact,
      .reps = instance->concrete,
      .name = nullptr,
      .call = nullptr,
      .isCopyingReturn = false,
      .instance = instance,
      .function = nullptr,
      .copy = nullptr,
      .index = 0,
      .isPrecise = true,
  });
}

void FunctionInstances::buildInstanceDecl(InstantiateDraft draft) {
  FuncSymbol *generic = draft.generic;
  FunctionDecl *original = generic->decl;
  auto *copy = this->freshCopy(original);
  copy->isInstance = true;
  copy->typeArgs = draft.reps;
  auto *function = this->state.ast.newFunc();
  function->name = generic->name;
  function->loc = generic->loc;
  function->decl = copy;
  function->minArgs = generic->minArgs;
  function->hasRest = generic->hasRest;
  draft.function = function;
  draft.copy = copy;
  auto list = TypeListDraft{
      .types = {},
      .isValues = {},
      .params = generic->typeParams,
      .args = draft.reps,
      .index = 0,
      .results = {},
  };
  for (const Type *param : generic->params) {
    list.types.push_back(param);
    list.isValues.push_back(true);
  }
  list.types.push_back(generic->hasReturnType() ? generic->getKnownReturnType() : nullptr);
  list.isValues.push_back(false);
  list.types.push_back(generic->predicate.has_value() ? generic->predicate->type : nullptr);
  list.isValues.push_back(false);
  size_t shared = std::min(copy->params.size(), original->params.size());
  for (size_t index = 0; index < shared; ++index) {
    list.types.push_back(original->params[index].declaredType);
    list.isValues.push_back(true);
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceTypes{.draft = std::move(draft)});

  return tasks.pushTask(SubstituteTypeList{.draft = std::move(list)});
}

void FunctionInstances::afterInstanceTypes(const AfterInstanceTypes &task) {
  InstantiateDraft draft = task.draft;
  FuncSymbol *generic = draft.generic;
  FuncSymbol *function = draft.function;
  InstanceRecord *instance = draft.instance;
  FunctionDecl *copy = draft.copy;
  FunctionDecl *original = generic->decl;
  size_t shared = std::min(copy->params.size(), original->params.size());
  auto results = this->state.walk.tasks.takeAnswers(generic->params.size() + 2 + shared);
  size_t next = 0;
  for (size_t index = 0; index < generic->params.size(); ++index) {
    function->params.push_back(results[next++]);
  }
  function->setReturnType(results[next++]);
  const Type *predicate = results[next++];
  if (generic->predicate.has_value()) {
    function->predicate = TypePredicate{.param = generic->predicate->param, .type = predicate};
  }
  for (size_t index = 0; index < shared; ++index) {
    copy->params[index].declaredType = results[next++];
  }
  this->applyExactParams(function, copy, draft.exact);
  copy->sym = function;
  this->expandArgumentList(copy, function);
  auto *home = this->state.declaration.modules.findDeclModule(original);
  if (home != nullptr) {
    this->state.declaration.modules.rememberDeclModule(copy, home);
  }
  auto &tasks = this->state.walk.tasks;
  if (draft.isPrecise) {
    return tasks.pushTask(WalkPreciseBody{.instance = instance, .copy = copy});
  }
  instance->decl = copy;
  auto keyTypes = this->functionInstanceRules.instanceKey(draft.reps, draft.exact);
  auto &copies = this->state.generics.copies;
  copies.rememberFunctionInstance(generic, keyTypes, instance);
  this->placeInstance(*instance);
  bool isTemplate = this->state.code.late.isTemplate(generic);
  if (!isTemplate && !copies.isCheckingInstance()) {
    copies.addPendingInstance(instance);

    return this->finishInstantiate(draft, function);
  }
  tasks.pushTask(AfterNewInstanceWalk{.draft = std::move(draft)});
  if (isTemplate) {
    return tasks.pushTask(WalkTemplateInstance{.instance = instance});
  }

  return tasks.pushTask(WalkInstance{.instance = instance});
}

void FunctionInstances::afterNewInstanceWalk(const AfterNewInstanceWalk &task) {
  const InstantiateDraft &draft = task.draft;
  InstanceRecord *instance = draft.instance;
  this->finishInstantiate(draft, draft.function);
  if (instance->failed) {
    return this->state.walk.tasks.pushTask(
        ReportInstanceAt{.instance = instance, .loc = draft.loc, .args = draft.args});
  }
}

void FunctionInstances::finishInstantiate(const InstantiateDraft &draft,
                                          FuncSymbol *instance) const {
  if (draft.name != nullptr) {
    draft.name->binding = NamedFunction{.func = instance};
  }
  CallExpr *call = draft.call;
  if (call == nullptr) {
    return;
  }
  this->retarget(call, instance);
  if (draft.isCopyingReturn) {
    call->type = instance->hasReturnType() ? instance->getKnownReturnType() : Type::error();
  }
}

void FunctionInstances::retarget(CallExpr *call, FuncSymbol *instance) const {
  switch (call->target.kind()) {
    case CallTargetKind::METHOD_CALL:
      call->target.as<MethodCall>()->method = instance;
      return;
    case CallTargetKind::SUPER_CALL:
      call->target.as<SuperCall>()->method = instance;
      return;
    case CallTargetKind::FUNCTION_CALL:
    case CallTargetKind::UNRESOLVED_CALL:
    case CallTargetKind::OVERLOAD_CALL:
    case CallTargetKind::BUILTIN_CALL:
    case CallTargetKind::UNION_METHOD_CALL:
      call->target = FunctionCall{.func = instance};
      return;
  }
}

FunctionDecl *FunctionInstances::freshCopy(const FunctionDecl *functionDecl) {
  AstCloner cloner(this->state.ast);
  FunctionDecl *copy = cloner.copyFunction(functionDecl);
  copy->place = functionDecl->place;
  copy->isAsync = functionDecl->isAsync;
  this->copyMemberRole(functionDecl, copy);
  copy->sym = functionDecl->sym;
  copy->name = functionDecl->name;
  copy->typeArgs = functionDecl->typeArgs;
  copy->isInstance = functionDecl->isInstance;
  copy->parent = functionDecl->parent;
  copy->group = functionDecl->group;
  copy->var = functionDecl->var;
  copy->creationSeq = functionDecl->creationSeq;
  this->copyParams(functionDecl, copy);

  return copy;
}

void FunctionInstances::copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  const ClassMember *original = functionDecl->member();
  if (original == nullptr) {
    return;
  }
  auto copied = ClassMember{.cls = original->cls, .access = original->access};
  if (functionDecl->isStatic()) {
    copied.method = StaticMethod{};
  } else if (functionDecl->isConstructor()) {
    copied.method = Constructor{};
  }
  copy->role = std::move(copied);
}

void FunctionInstances::copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
  bool hasList = !functionDecl->listName.empty() && copy->params.size() > functionDecl->listStart;
  if (hasList) {
    copy->params.resize(functionDecl->listStart);
    copy->listName = functionDecl->listName;
    copy->listStart = functionDecl->listStart;
    copy->listType = functionDecl->listType;
  }
  size_t shared = std::min(copy->params.size(), functionDecl->params.size());
  for (size_t index = 0; index < shared; ++index) {
    copy->params[index].declaredType = functionDecl->params[index].declaredType;
  }
  for (size_t index = copy->params.size(); index < functionDecl->params.size(); ++index) {
    copy->params.push_back(functionDecl->params[index]);
  }
}

bool FunctionInstances::isExpandableRest(const FunctionDecl *functionDecl,
                                         const FuncSymbol *function) const {
  if (!function->hasRest || functionDecl->params.empty()) {
    return false;
  }

  return functionDecl->params.back().rest() && functionDecl->listName.empty();
}

void FunctionInstances::expandArgumentList(FunctionDecl *functionDecl, FuncSymbol *function) {
  if (!this->isExpandableRest(functionDecl, function)) {
    return;
  }
  const Type *list = function->params.back();
  auto flat = std::vector<const Type *>();
  size_t minArgs = function->minArgs;
  bool hasRest = function->hasRest;
  if (!ArgumentLists::flattenArguments(function->params, minArgs, hasRest, flat)) {
    return;
  }
  Param rest = functionDecl->params.back();
  functionDecl->params.pop_back();
  functionDecl->listName = rest.name;
  functionDecl->listStart = functionDecl->params.size();
  functionDecl->listType = list;
  for (size_t index = 0; index < list->fields().size(); ++index) {
    auto param = this->listPartParam(rest, list, index);
    functionDecl->params.push_back(param);
  }
  const Type *more = list->tupleRest();
  if (more != nullptr) {
    auto param = this->listRestParam(rest, more);
    functionDecl->params.push_back(param);
  }
  function->params = std::move(flat);
  function->minArgs = minArgs;
  function->hasRest = hasRest;
}

Param FunctionInstances::listPartParam(const Param &rest, const Type *list, size_t index) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + "." + std::to_string(index);
  Param param;
  param.name = this->state.ast.keepName(name);
  param.loc = rest.loc;
  if (index >= list->tupleMin()) {
    param.form = OptionalParam{};
  }
  param.declaredType = list->fields()[index].type;

  return param;
}

Param FunctionInstances::listRestParam(const Param &rest, const Type *more) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + kRestPartSuffix.str();
  Param param;
  param.name = this->state.ast.keepName(name);
  param.loc = rest.loc;
  param.form = RestParam{};
  param.declaredType = more;

  return param;
}

void FunctionInstances::placeInstance(const InstanceRecord &instance) {
  FunctionDecl *original = instance.generic->decl;
  ClassDecl *classDecl = original->cls();
  if (classDecl != nullptr && !original->isStatic()) {
    classDecl->methodInstances.push_back(instance.decl);
    return;
  }
  this->state.inventory.instances.push_back(instance.decl);
}

void FunctionInstances::applyExactParams(FuncSymbol *function, FunctionDecl *copy,
                                         llvm::ArrayRef<const Type *> exact) const {
  size_t shared = std::min({exact.size(), function->params.size(), copy->params.size()});
  for (size_t index = 0; index < shared; ++index) {
    if (exact[index] != nullptr) {
      copy->params[index].declaredType = exact[index];
      function->params[index] = exact[index];
    }
  }
}

}
