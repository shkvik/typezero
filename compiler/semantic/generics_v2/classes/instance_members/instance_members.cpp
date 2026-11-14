#include "compiler/semantic/generics_v2/classes/instance_members/instance_members.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"

namespace tz {

InstanceMembers::InstanceMembers(SemanticState &owner)
    : state(owner) {}

void InstanceMembers::declareInstanceMembers(const DeclareInstanceMembers &task) {
  ClassDecl *instance = task.instance;
  ClassDecl *generic = instance->generic();
  auto members = std::vector<InstanceMember>();
  for (size_t index = 0; index < instance->methods.size() && index < generic->methods.size();
       ++index) {
    if (!generic->methods[index]->isStatic()) {
      members.push_back(InstanceMember{
          .functionDecl = instance->methods[index],
          .generic = generic->methods[index],
          .isInit = false,
      });
    }
  }
  for (size_t index = 0;
       index < instance->constructors.size() && index < generic->constructors.size(); ++index) {
    members.push_back(InstanceMember{
        .functionDecl = instance->constructors[index],
        .generic = generic->constructors[index],
        .isInit = false,
    });
  }
  for (size_t index = 0; index < instance->fields.size() && index < generic->fields.size();
       ++index) {
    FunctionDecl *method = instance->fields[index].method();
    FunctionDecl *genericMethod = generic->fields[index].method();
    if (method != nullptr && genericMethod != nullptr) {
      members.push_back(
          InstanceMember{.functionDecl = method, .generic = genericMethod, .isInit = false});
    }
  }
  auto draft = InstanceMembersDraft{
      .instance = instance,
      .members = std::move(members),
      .member = 0,
      .instanceParams = {},
      .param = 0,
      .hasInit = false,
  };

  return this->nextInstanceMember(NextInstanceMember{.draft = std::move(draft)});
}

void InstanceMembers::nextInstanceMember(const NextInstanceMember &task) {
  InstanceMembersDraft draft = task.draft;
  ClassDecl *instance = draft.instance;
  auto &tasks = this->state.walk.tasks;
  if (draft.member == draft.members.size()) {
    bool isInitDue = !draft.hasInit && instance->generic()->init != nullptr;
    if (!isInitDue) {
      return;
    }
    tasks.pushTask(AfterInstanceInit{.draft = std::move(draft)});

    return tasks.pushTask(BuildInstanceInit{.instance = instance});
  }
  const InstanceMember &member = draft.members[draft.member];
  this->makeInstanceSymbol(member.functionDecl, member.generic);
  if (member.generic->sym == nullptr) {
    draft.member += 1;

    return tasks.pushTask(NextInstanceMember{.draft = std::move(draft)});
  }
  draft.instanceParams = {};
  draft.param = 0;

  return this->nextIteratedParam(NextIteratedParam{.draft = std::move(draft)});
}

void InstanceMembers::afterInstanceInit(const AfterInstanceInit &task) {
  InstanceMembersDraft draft = task.draft;
  ClassDecl *instance = draft.instance;
  draft.members.push_back(InstanceMember{
      .functionDecl = instance->init,
      .generic = instance->generic()->init,
      .isInit = true,
  });
  draft.hasInit = true;

  return this->nextInstanceMember(NextInstanceMember{.draft = std::move(draft)});
}

FuncSymbol *InstanceMembers::makeInstanceSymbol(FunctionDecl *functionDecl, FunctionDecl *generic) {
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  functionDecl->sym = function;
  functionDecl->generic = generic;
  ClassMember *member = functionDecl->member();
  if (member != nullptr) {
    member->variant = generic->variant();
  }
  Overriding *overriding = functionDecl->overriding();
  if (overriding != nullptr) {
    overriding->overridden = generic->overridden();
    overriding->openToLate = generic->openToLate();
  }

  return function;
}

void InstanceMembers::nextIteratedParam(const NextIteratedParam &task) {
  InstanceMembersDraft draft = task.draft;
  ClassDecl *instance = draft.instance;
  FuncSymbol *genericFunction = draft.members[draft.member].generic->sym;
  while (draft.param < genericFunction->typeParams.size()) {
    const Type *param = genericFunction->typeParams[draft.param];
    const Type *iterated = param->iterated();
    bool isOpen = iterated != nullptr &&
                  GenericCheckHelpers::mentionsAnyOf(iterated, instance->generic()->typeArgs);
    if (isOpen) {
      auto &tasks = this->state.walk.tasks;
      tasks.pushTask(AfterIteratedParam{.draft = std::move(draft)});

      return tasks.pushTask(InstanceType{.instance = instance, .type = iterated});
    }
    draft.instanceParams.push_back(param);
    draft.param += 1;
  }

  return this->startMemberSignature(std::move(draft));
}

void InstanceMembers::afterIteratedParam(const AfterIteratedParam &task) {
  InstanceMembersDraft draft = task.draft;
  const Type *instanceIterated = this->state.walk.tasks.takeAnswer();
  llvm::StringRef iterable = TypeNameTable::getName(TypeName::ITERABLE);
  auto *instanceParam = Type::param(iterable.str() + "<" + instanceIterated->name().str() + ">");
  instanceParam->setIterated(instanceIterated);
  draft.instanceParams.push_back(instanceParam);
  draft.param += 1;

  return this->nextIteratedParam(NextIteratedParam{.draft = std::move(draft)});
}

void InstanceMembers::startMemberSignature(InstanceMembersDraft draft) {
  const InstanceMember &member = draft.members[draft.member];
  FunctionDecl *functionDecl = member.functionDecl;
  FunctionDecl *generic = member.generic;
  FuncSymbol *genericFunction = generic->sym;
  functionDecl->typeArgs = draft.instanceParams;
  functionDecl->sym->typeParams = draft.instanceParams;
  for (size_t index = functionDecl->params.size(); index < generic->params.size(); ++index) {
    functionDecl->params.push_back(generic->params[index]);
  }
  for (size_t index = functionDecl->typeParams.size(); index < generic->typeParams.size();
       ++index) {
    functionDecl->typeParams.push_back(generic->typeParams[index]);
  }
  auto types = MemberTypesDraft{
      .instance = draft.instance,
      .genericParams = genericFunction->typeParams,
      .instanceParams = draft.instanceParams,
      .types = {},
      .isValues = {},
      .index = 0,
      .results = {},
  };
  for (const Type *defaultType : genericFunction->typeDefaults) {
    this->addMemberType(types, defaultType, false);
  }
  for (const Type *param : genericFunction->params) {
    this->addMemberType(types, param, true);
  }
  for (const Param &param : generic->params) {
    this->addMemberType(types, param.declaredType, true);
  }
  this->addMemberType(types, genericFunction->getKnownReturnType(), false);
  if (genericFunction->predicate.has_value()) {
    this->addMemberType(types, genericFunction->predicate->type, false);
  }
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterMemberSignature{.draft = std::move(draft)});

  return tasks.pushTask(NextMemberType{.draft = std::move(types)});
}

void InstanceMembers::addMemberType(MemberTypesDraft &types, const Type *type, bool isValue) const {
  types.types.push_back(type);
  types.isValues.push_back(isValue);
}

void InstanceMembers::nextMemberType(const NextMemberType &task) {
  MemberTypesDraft draft = task.draft;
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
  const ClassDecl *instance = draft.instance;
  bool isValue = draft.isValues[draft.index];
  tasks.pushTask(AfterMemberInstanceType{.draft = std::move(draft)});
  if (isValue) {
    return tasks.pushTask(InstanceValueType{.instance = instance, .type = type});
  }

  return tasks.pushTask(InstanceType{.instance = instance, .type = type});
}

void InstanceMembers::afterMemberInstanceType(const AfterMemberInstanceType &task) {
  MemberTypesDraft draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  const Type *instanceType = tasks.takeAnswer();
  bool isValue = draft.isValues[draft.index];
  auto params = draft.genericParams;
  auto args = draft.instanceParams;
  tasks.pushTask(AfterMemberType{.draft = std::move(draft)});
  if (isValue) {
    return tasks.pushTask(SubstituteValueType{
        .type = instanceType, .params = std::move(params), .args = std::move(args)});
  }

  return tasks.pushTask(
      SubstituteType{.type = instanceType, .params = std::move(params), .args = std::move(args)});
}

void InstanceMembers::afterMemberType(const AfterMemberType &task) {
  MemberTypesDraft draft = task.draft;
  draft.results.push_back(this->state.walk.tasks.takeAnswer());
  draft.index += 1;

  return this->nextMemberType(NextMemberType{.draft = std::move(draft)});
}

void InstanceMembers::afterMemberSignature(const AfterMemberSignature &task) {
  InstanceMembersDraft draft = task.draft;
  const InstanceMember &member = draft.members[draft.member];
  FunctionDecl *functionDecl = member.functionDecl;
  FunctionDecl *generic = member.generic;
  FuncSymbol *function = functionDecl->sym;
  FuncSymbol *genericFunction = generic->sym;
  size_t count = genericFunction->typeDefaults.size() + genericFunction->params.size() +
                 generic->params.size() + 1 + (genericFunction->predicate.has_value() ? 1 : 0);
  auto results = this->state.walk.tasks.takeAnswers(count);
  size_t next = 0;
  for (size_t index = 0; index < genericFunction->typeDefaults.size(); ++index) {
    function->typeDefaults.push_back(results[next++]);
  }
  for (size_t index = 0; index < genericFunction->params.size(); ++index) {
    function->params.push_back(results[next++]);
  }
  for (size_t index = 0; index < generic->params.size(); ++index) {
    functionDecl->params[index].declaredType = results[next++];
  }
  function->minArgs = genericFunction->minArgs;
  function->hasRest = genericFunction->hasRest;
  function->setReturnType(results[next++]);
  function->discardsResult = genericFunction->discardsResult;
  if (genericFunction->predicate.has_value()) {
    function->predicate =
        TypePredicate{.param = genericFunction->predicate->param, .type = results[next++]};
  }
  auto *home = this->state.declaration.modules.findDeclModule(generic);
  if (home != nullptr) {
    this->state.declaration.modules.rememberDeclModule(functionDecl, home);
  }
  if (member.isInit) {
    function->setReturnType(Type::voidTy());
  }
  draft.member += 1;

  return this->nextInstanceMember(NextInstanceMember{.draft = std::move(draft)});
}

void InstanceMembers::walkInstanceVariants(const WalkInstanceVariants &task) {
  this->state.diagnostics.pushList(DiagnosticRoute::OWNER);
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterInstanceVariants{.instance = task.instance});

  return tasks.pushTask(WalkVariants{.classDecl = task.instance});
}

void InstanceMembers::afterInstanceVariants(const AfterInstanceVariants &task) {
  auto &diagnostics = this->state.diagnostics;
  auto variantDiagnostics = diagnostics.popList();
  const auto *firstError = diagnostics.findFirstError(variantDiagnostics);
  if (firstError == nullptr) {
    return;
  }
  auto *check = this->state.generics.copies.findClassCheck(task.instance);
  if (check != nullptr && !check->failed) {
    check->failed = true;
    check->message = firstError->message;
    check->errorLoc = firstError->loc;
  }
}

}
