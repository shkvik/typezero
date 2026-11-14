#include "compiler/semantic/generics_v2/classes/class_instances/class_instances.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/stream/stream_kind_table/stream_kind_table.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

namespace {

constexpr int kMaxClassChain = 16;

}

ClassInstances::ClassInstances(SemanticState &owner)
    : state(owner) {}

void ClassInstances::instantiateClassType(const InstantiateClassType &task) {
  auto draft = ClassInstanceDraft{
      .generic = task.classDecl,
      .args = task.args,
      .use = task.loc,
      .index = 0,
  };
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceTypeParams{.draft = std::move(draft)});

  return tasks.pushTask(DeclareClassTypeParams{.classDecl = task.classDecl});
}

void ClassInstances::afterInstanceTypeParams(const AfterInstanceTypeParams &task) {
  ClassInstanceDraft draft = task.draft;
  auto counted = this->checkArgumentCount(draft.generic, draft.args);
  if (counted.isErr()) {
    return this->failInstance(draft, counted.error());
  }

  return this->nextInstanceDefault(NextInstanceDefault{.draft = std::move(draft)});
}

void ClassInstances::nextInstanceDefault(const NextInstanceDefault &task) {
  ClassInstanceDraft draft = task.draft;
  const ClassDecl *generic = draft.generic;
  const std::vector<const Type *> &params = generic->typeArgs;
  size_t index = draft.args.size();
  if (index == params.size()) {
    draft.index = 0;

    return this->nextInstanceArgument(NextInstanceArgument{.draft = std::move(draft)});
  }
  auto defaults = this->state.generics.copies.getClassDefaults(generic);
  const Type *defaultType = index < defaults.size() ? defaults[index] : nullptr;
  auto present = this->checkDefaultPresent(generic, defaultType);
  if (present.isErr()) {
    return this->failInstance(draft, present.error());
  }
  std::vector<const Type *> known = draft.args;
  known.resize(params.size(), nullptr);
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceDefault{.draft = std::move(draft)});

  return tasks.pushTask(
      SubstituteType{.type = defaultType, .params = params, .args = std::move(known)});
}

void ClassInstances::afterInstanceDefault(const AfterInstanceDefault &task) {
  ClassInstanceDraft draft = task.draft;
  draft.args.push_back(this->state.walk.tasks.takeAnswer());

  return this->nextInstanceDefault(NextInstanceDefault{.draft = std::move(draft)});
}

void ClassInstances::nextInstanceArgument(const NextInstanceArgument &task) {
  ClassInstanceDraft draft = task.draft;
  const ClassDecl *generic = draft.generic;
  if (draft.index == generic->typeArgs.size()) {
    return this->finishInstanceChecks(std::move(draft));
  }
  auto fitting = this->checkArgumentKind(generic, draft.args, draft.index);
  if (fitting.isErr()) {
    return this->failInstance(draft, fitting.error());
  }
  auto check = CheckConstraint{
      .arg = draft.args[draft.index],
      .param = generic->typeArgs[draft.index],
      .params = generic->typeArgs,
      .args = draft.args,
  };
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceArgument{.draft = std::move(draft)});

  return tasks.pushTask(std::move(check));
}

void ClassInstances::afterInstanceArgument(const AfterInstanceArgument &task) {
  ClassInstanceDraft draft = task.draft;
  bool isSatisfied = this->state.walk.tasks.takeVerdict();
  if (!isSatisfied) {
    const ClassDecl *generic = draft.generic;
    size_t index = draft.index;
    auto constraint = this->constraintText(generic, index);
    auto message = ErrorTextClassInstance::unsatisfied(
        draft.args[index]->name(), constraint, generic->typeArgs[index]->name(), generic->name);

    return this->failInstance(
        draft, GenericsClassesRegistryInstantiateClassError{
                   .code = GenericsClassesRegistryInstantiateClassErrorCode::CONSTRAINT_UNSATISFIED,
                   .message = message,
               });
  }
  draft.index += 1;

  return this->nextInstanceArgument(NextInstanceArgument{.draft = std::move(draft)});
}

void ClassInstances::finishInstanceChecks(ClassInstanceDraft draft) {
  ClassDecl *generic = draft.generic;
  llvm::SMLoc loc = draft.use.isValid() ? draft.use : generic->loc();
  auto &tasks = this->state.walk.tasks;
  if (this->isEmitterClass(generic)) {
    auto fits = this->state.library.events.checkEventMapFits(draft.args[0]);
    if (fits.isErr()) {
      this->state.diagnostics.error(loc, fits.error().message);

      return tasks.pushAnswer(Type::error());
    }
  }
  ClassDecl *known = this->state.generics.copies.findClassInstance(generic, draft.args);
  if (known != nullptr) {
    tasks.pushAnswer(known->type);

    return this->reuseInstance(known, draft.use, loc);
  }
  auto bounded = this->checkChainBounded(generic);
  if (bounded.isErr()) {
    return this->failInstance(draft, bounded.error());
  }
  auto *instance = this->makeInstance(generic, draft.args);
  this->rememberCheck(instance, draft.use);
  tasks.pushTask(FinishClassInstance{.instance = instance});
  tasks.pushTask(WalkOrPostpone{.instance = instance, .loc = loc});

  return this->fillOrPostpone(instance);
}

void ClassInstances::failInstance(const ClassInstanceDraft &draft,
                                  const GenericsClassesRegistryInstantiateClassError &error) {
  auto code = error.code;
  if (code != GenericsClassesRegistryInstantiateClassErrorCode::ALREADY_REPORTED) {
    bool isEndless = code == GenericsClassesRegistryInstantiateClassErrorCode::ENDLESS_INSTANCES;
    llvm::SMLoc at = isEndless || !draft.use.isValid() ? draft.generic->loc() : draft.use;
    this->state.diagnostics.error(at, error.message);
  }

  return this->state.walk.tasks.pushAnswer(Type::error());
}

void ClassInstances::finishClassInstance(const FinishClassInstance &task) {
  return this->state.walk.tasks.pushAnswer(task.instance->type);
}

ClassInstances::ResultCanBeMade
ClassInstances::checkArgumentCount(const ClassDecl *generic,
                                   llvm::ArrayRef<const Type *> args) const {
  size_t count = generic->typeArgs.size();
  if (args.size() > count) {
    auto message = ErrorTextClassInstance::tooMany(generic->name, count, args.size());

    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::TOO_MANY_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok();
}

ClassInstances::ResultCanBeMade ClassInstances::checkDefaultPresent(const ClassDecl *generic,
                                                                    const Type *defaultType) const {
  if (this->isEmitterClass(generic)) {
    auto message = ErrorTextClassInstance::eventMap();

    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::EVENT_MAP_MISSING,
        .message = message,
    });
  }
  if (defaultType == nullptr && this->isStreamClass(generic)) {
    auto message = ErrorTextClassInstance::chunks(generic->name);

    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::CHUNK_TYPE_MISSING,
        .message = message,
    });
  }
  if (defaultType == nullptr) {
    auto message = ErrorTextClassInstance::needs(generic->name, generic->typeArgs.size());

    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::TYPE_ARGUMENTS_MISSING,
        .message = message,
    });
  }

  return ok();
}

bool ClassInstances::isEmitterClass(const ClassDecl *classDecl) const {
  return classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, LibraryClass::EVENT_EMITTER);
}

bool ClassInstances::isStreamClass(const ClassDecl *classDecl) const {
  auto streamKind = StreamKindTable::find(classDecl->name);

  return classDecl->library() && streamKind.has_value();
}

ClassInstances::ResultCanBeMade
ClassInstances::checkArgumentKind(const ClassDecl *generic, const std::vector<const Type *> &args,
                                  size_t index) const {
  const Type *arg = args[index];
  if (arg->isError()) {
    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  if (arg->isUndefined()) {
    auto message = ErrorTextClassInstance::undefinedArg(arg->name(), generic->name);

    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::UNDEFINED_TYPE_ARGUMENT,
        .message = message,
    });
  }

  return ok();
}

std::string ClassInstances::constraintText(const ClassDecl *generic, size_t index) const {
  const TypeParamDecl &typeParam = generic->typeParams[index];
  bool isNamed =
      typeParam.constraint.has_value() && typeParam.constraint->kind() == TypeRefKind::NAME_REF;
  if (isNamed) {
    return typeParam.constraint->name.str();
  }
  const Type *param = generic->typeArgs[index];
  auto allowed = std::vector<const Type *>();
  if (param->constraint() != nullptr) {
    allowed.push_back(param->constraint());
  }
  llvm::append_range(allowed, param->alternatives());

  return GenericCheckHelpers::typeNames(allowed, " | ");
}

void ClassInstances::reuseInstance(const ClassDecl *known, llvm::SMLoc use, llvm::SMLoc loc) {
  auto *check = this->state.generics.copies.findClassCheck(known);
  if (check == nullptr) {
    return;
  }
  if (!check->callLoc.isValid()) {
    check->callLoc = use;
  }
  bool isReportDue = check->checked && check->failed && !check->isReported;
  if (isReportDue) {
    return this->state.walk.tasks.pushTask(ReportClassInstance{.check = check, .loc = loc});
  }
}

ClassInstances::ResultCanBeMade ClassInstances::checkChainBounded(const ClassDecl *generic) {
  auto &copies = this->state.generics.copies;
  if (copies.countFillingOf(generic) < kMaxClassChain) {
    return ok();
  }
  bool isReported = copies.isEndlessReported(generic);
  copies.markEndlessReported(generic);
  if (isReported) {
    return err(GenericsClassesRegistryInstantiateClassError{
        .code = GenericsClassesRegistryInstantiateClassErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  auto message = ErrorTextClassInstance::endless(generic->name);

  return err(GenericsClassesRegistryInstantiateClassError{
      .code = GenericsClassesRegistryInstantiateClassErrorCode::ENDLESS_INSTANCES,
      .message = message,
  });
}

ClassDecl *ClassInstances::makeInstance(ClassDecl *generic, const std::vector<const Type *> &args) {
  AstCloner cloner(this->state.ast);
  ClassDecl *instance = cloner.copyClass(generic);
  instance->decorators.clear();
  instance->syntax = StatementClass{};
  instance->generics = GenericInstance{.generic = generic};
  instance->typeArgs = args;
  instance->isAbstract = generic->isAbstract;
  instance->origin = generic->origin;
  instance->checkOnly = this->isMentioningParam(args);
  instance->stage = ClassStage::RESOLVED;
  instance->declared = true;
  instance->id = this->state.declaration.classes.takeClassId();
  auto display = this->instanceName(generic, args);
  auto *type = Type::classType(display, instance);
  type->setTypeArgs(args);
  instance->type = type;
  this->registerInstance(generic, instance);

  return instance;
}

bool ClassInstances::isMentioningParam(llvm::ArrayRef<const Type *> args) const {
  for (const Type *arg : args) {
    if (TypeQueries::mentionsParam(arg)) {
      return true;
    }
  }

  return false;
}

std::string ClassInstances::instanceName(const ClassDecl *generic,
                                         llvm::ArrayRef<const Type *> args) const {
  auto defaults = this->state.generics.copies.getClassDefaults(generic);
  size_t shown = GenericCheckHelpers::shownArgumentCount(defaults, args);
  if (shown == 0) {
    return generic->name.str();
  }
  auto names = GenericCheckHelpers::typeNames(args.take_front(shown), ", ");

  return generic->name.str() + "<" + names + ">";
}

void ClassInstances::registerInstance(ClassDecl *generic, ClassDecl *instance) {
  this->state.declaration.classes.addClass(instance);
  generic->generics.as<GenericTemplate>()->instances.push_back(instance);
  this->state.generics.copies.rememberClassInstance(generic, instance->typeArgs, instance);
  auto *home = this->state.declaration.modules.findDeclModule(generic);
  if (home != nullptr) {
    this->state.declaration.modules.rememberDeclModule(instance, home);
  }
}

void ClassInstances::rememberCheck(ClassDecl *instance, llvm::SMLoc use) {
  if (instance->checkOnly) {
    return;
  }
  auto &copies = this->state.generics.copies;
  auto *check = copies.addInstanceRecord();
  check->cls = instance;
  check->callLoc = use;
  check->requester = copies.getCheckingInstance();
  copies.rememberClassCheck(instance, check);
}

void ClassInstances::fillOrPostpone(ClassDecl *instance) {
  auto &tasks = this->state.walk.tasks;
  if (!this->state.declaration.classes.hasDeclaredMethods()) {
    return this->state.generics.copies.addUnfilledClass(instance);
  }
  if (this->state.declaration.classes.hasLinkedClasses()) {
    tasks.pushTask(LinkInstance{.instance = instance});
  }

  return tasks.pushTask(FillInstance{.instance = instance});
}

void ClassInstances::walkOrPostpone(const WalkOrPostpone &task) {
  auto &copies = this->state.generics.copies;
  auto *check = copies.findClassCheck(task.instance);
  if (check == nullptr) {
    return;
  }
  if (!copies.isCheckingInstance()) {
    return copies.addPendingInstance(check);
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceWalk{.check = check, .loc = task.loc});

  return tasks.pushTask(WalkClassInstance{.check = check});
}

void ClassInstances::afterInstanceWalk(const AfterInstanceWalk &task) {
  if (task.check->failed) {
    return this->state.walk.tasks.pushTask(
        ReportClassInstance{.check = task.check, .loc = task.loc});
  }
}

void ClassInstances::linkInstance(const LinkInstance &task) {
  ClassDecl *instance = task.instance;
  auto &tasks = this->state.walk.tasks;
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  tasks.pushTask(BuildVtable{.classDecl = instance});
  tasks.pushTask(DropDiagnosticList{});

  return tasks.pushTask(StartLinkOverrides{.classDecl = instance});
}

void ClassInstances::markOverridden(const MarkOverridden &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->setOverridden(functionDecl);
  FunctionDecl *generic = functionDecl->generic != nullptr ? functionDecl->generic : functionDecl;
  ClassDecl *classDecl = generic->cls();
  if (classDecl == nullptr || !classDecl->isGeneric()) {
    return;
  }
  this->setOverridden(generic);
  for (ClassDecl *instance : classDecl->instances()) {
    this->markInstanceCopies(instance, generic);
  }
}

void ClassInstances::setOverridden(FunctionDecl *functionDecl) const {
  Overriding *overriding = functionDecl->overriding();
  if (overriding != nullptr) {
    overriding->overridden = true;
  }
}

void ClassInstances::markInstanceCopies(const ClassDecl *instance,
                                        const FunctionDecl *generic) const {
  for (FunctionDecl *method : instance->findOwnMethods()) {
    if (method->generic == generic) {
      this->setOverridden(method);
    }
  }
}

}
