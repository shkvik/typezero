#include "compiler/semantic/library_v2/common/library_common_registry.h"
#include "compiler/program/library/builtin_names/prelude_type_table/prelude_type_table.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

LibraryCommonRegistry::LibraryCommonRegistry(SemanticState &owner)
    : state(owner) {}

void LibraryCommonRegistry::rejectSpreadCall(const RejectSpreadCall &task) {
  CallExpr *call = task.call;
  auto &common = this->state.library.common;
  for (size_t index = 0; index < call->args.size(); ++index) {
    auto plain = common.checkSpreadFree(call, index);
    if (plain.isErr()) {
      this->state.diagnostics.error(call->args[index]->loc(), plain.error().message);
      call->type = Type::error();

      return this->state.walk.tasks.pushTask(WalkArgsLoosely{.call = call});
    }
  }
}

void LibraryCommonRegistry::readNodeOptions(const ReadNodeOptions &task) {
  CallExpr *call = task.call;
  size_t index = task.index;
  auto &tasks = this->state.walk.tasks;
  if (index >= call->args.size() || isa<UndefinedExpr>(call->args[index])) {
    return tasks.pushVerdict(true);
  }
  auto names = this->optionNames(task.allowed);
  Expr *arg = call->args[index];
  auto *literal = dyn_cast<ObjectLitExpr>(arg);
  if (literal == nullptr) {
    auto message = ErrorTextNodeOptions::literal(task.functionName, names[0]);
    this->state.diagnostics.error(arg->loc(), message);
    tasks.pushVerdict(false);

    return tasks.pushTask(CheckExprSlot{.slot = &call->args[index], .expected = nullptr});
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    bool isByName =
        !property.spread() && !property.key() && !property.method() && !property.name.empty();
    if (!isByName) {
      auto message = ErrorTextNodeOptions::byName(task.functionName, names[0]);
      this->state.diagnostics.error(property.loc, message);

      return tasks.pushVerdict(false);
    }
    auto option = NodeOptionTable::find(property.name);
    if (!option.has_value() || !llvm::is_contained(task.allowed, *option)) {
      auto message = ErrorTextNodeOptions::unknown(task.functionName, property.name, names);
      this->state.diagnostics.error(property.loc, message);

      return tasks.pushVerdict(false);
    }
  }

  return tasks.pushVerdict(true);
}

std::vector<llvm::StringRef>
LibraryCommonRegistry::optionNames(llvm::ArrayRef<NodeOption> options) const {
  auto names = std::vector<llvm::StringRef>();
  for (NodeOption option : options) {
    names.push_back(NodeOptionTable::getName(option));
  }

  return names;
}

void LibraryCommonRegistry::preludeTypeOf(const PreludeTypeOf &task) {
  auto name = PreludeTypeTable::getName(task.type);
  auto *alias = this->state.declaration.prelude.findBuiltinAlias(name);
  assert(alias != nullptr && "the prelude declares the type");
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AnswerAliasType{.alias = alias});

  return tasks.pushTask(ResolveAlias{.alias = alias});
}

void LibraryCommonRegistry::answerAliasType(const AnswerAliasType &task) {
  TypeAliasDecl *alias = task.alias;
  const Type *type = alias->resolved != nullptr ? alias->resolved : alias->self;

  return this->state.walk.tasks.pushAnswer(type);
}

void LibraryCommonRegistry::pushCheckedValue(const PushCheckedValue &task) {
  return this->state.walk.tasks.pushChecked(task.expr);
}

void LibraryCommonRegistry::markCallFailed(const MarkCallFailed &task) {
  CallExpr *call = task.call;
  call->type = Type::error();
  for (Expr *&arg : llvm::reverse(call->args)) {
    this->pushLooseCheck(&arg);
  }
}

void LibraryCommonRegistry::pushLooseCheck(Expr **slot) {
  auto &tasks = this->state.walk.tasks;
  auto *functionExpr = dyn_cast<FunctionExpr>(*slot);
  if (functionExpr == nullptr) {
    return tasks.pushTask(CheckExprSlot{.slot = slot, .expected = nullptr});
  }
  auto params = std::vector<const Type *>(functionExpr->fn->params.size(), Type::error());
  auto loose = Signature{
      .params = params,
      .pad = false,
  };
  tasks.pushTask(WriteCheckedSlot{.slot = slot});

  return tasks.pushTask(CheckFunctionExpr{.functionExpr = functionExpr, .expected = loose});
}

void LibraryCommonRegistry::callLibraryFunction(const CallLibraryFunction &task) {
  auto *functionDecl = this->state.library.declarations.findLibraryFunction(task.name);
  assert(functionDecl != nullptr && functionDecl->sym != nullptr && "a function of the library");

  return this->startLibraryCall(task.call, functionDecl->sym, task.bound, task.given, task.loc);
}

void LibraryCommonRegistry::callLibraryStatic(const CallLibraryStatic &task) {
  auto methods = task.classDecl->findStaticMethodsNamed(task.name);
  assert(!methods.empty() && methods[0]->sym != nullptr && "a static method of the library");

  return this->startLibraryCall(task.call, methods[0]->sym, task.bound, task.given, task.loc);
}

void LibraryCommonRegistry::callLibraryMethod(const CallLibraryMethod &task) {
  auto methods = task.classDecl->findMethodsNamed(task.name);
  assert(!methods.empty() && methods[0]->sym != nullptr && "a method of the library class");
  FuncSymbol *function = methods[0]->sym;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterMethodReturn{.draft = task, .function = function});

  return tasks.pushTask(ReturnTypeOf{.function = function});
}

void LibraryCommonRegistry::afterMethodReturn(const AfterMethodReturn &task) {
  const CallLibraryMethod &draft = task.draft;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = tasks.takeAnswer();
  this->targetMethod(draft, function);
  bool isInstantiating = function->isGeneric() && !this->state.walk.functions.isInsideTrial() &&
                         llvm::none_of(draft.bound, &TypeQueries::mentionsParam);
  tasks.pushTask(AfterMethodCallee{.draft = draft, .function = function, .returnType = returnType});
  if (!isInstantiating) {
    return;
  }

  return tasks.pushTask(InstantiateCallTarget{
      .call = draft.call,
      .generic = function,
      .typeArgs = draft.bound,
      .loc = draft.member->loc(),
      .exact = {},
      .isCopyingReturn = false,
  });
}

void LibraryCommonRegistry::targetMethod(const CallLibraryMethod &draft, FuncSymbol *function) {
  CallExpr *call = draft.call;
  if (draft.receiver == CallReceiver::SUPER) {
    call->target = SuperCall{.method = function};

    return;
  }
  if (function->isGeneric()) {
    call->target = MethodCall{.method = function};

    return;
  }
  FunctionDecl *implementation =
      this->state.library.common.soleImplementation(draft.classDecl, function->decl);
  if (implementation != nullptr) {
    call->target = MethodCall{.method = implementation->sym};

    return;
  }
  call->target = MethodCall{.method = function, .virtualCall = true};
}

void LibraryCommonRegistry::afterMethodCallee(const AfterMethodCallee &task) {
  const CallLibraryMethod &draft = task.draft;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *instance = function->isGeneric() ? draft.call->method() : function;
  bool isOwnParams = instance == function;
  auto list = TypeListDraft{
      .types = {},
      .isValues = {},
      .params = function->typeParams,
      .args = draft.bound,
      .index = 0,
      .results = {},
  };
  if (task.returnType != nullptr) {
    list.types.push_back(task.returnType);
    list.isValues.push_back(false);
  }
  if (isOwnParams) {
    for (const Type *param : function->params) {
      list.types.push_back(param);
      list.isValues.push_back(false);
    }
  }
  tasks.pushTask(AfterLibraryMemberType{.member = draft.member});
  tasks.pushTask(FunctionTypeOf{.function = instance});
  tasks.pushTask(FinishLibraryCall{
      .call = draft.call,
      .callee = instance,
      .given = draft.given,
      .loc = draft.member->loc(),
      .hasReturn = task.returnType != nullptr,
      .isOwnParams = isOwnParams,
  });

  return tasks.pushTask(SubstituteTypeList{.draft = std::move(list)});
}

void LibraryCommonRegistry::afterLibraryMemberType(const AfterLibraryMemberType &task) {
  task.member->type = this->state.walk.tasks.takeAnswer();
}

void LibraryCommonRegistry::startLibraryCall(CallExpr *call, FuncSymbol *function,
                                             const std::vector<const Type *> &bound,
                                             const std::vector<bool> &given, llvm::SMLoc loc) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterLibraryReturn{
      .call = call,
      .function = function,
      .bound = bound,
      .given = given,
      .loc = loc,
  });

  return tasks.pushTask(ReturnTypeOf{.function = function});
}

void LibraryCommonRegistry::afterLibraryReturn(const AfterLibraryReturn &task) {
  CallExpr *call = task.call;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  const Type *returnType = tasks.takeAnswer();
  call->target = FunctionCall{.func = function};
  bool isInstantiating = function->isGeneric() && !this->state.walk.functions.isInsideTrial() &&
                         llvm::none_of(task.bound, &TypeQueries::mentionsParam);
  tasks.pushTask(AfterLibraryCallee{
      .call = call,
      .function = function,
      .bound = task.bound,
      .given = task.given,
      .loc = task.loc,
      .returnType = returnType,
  });
  if (!isInstantiating) {
    return;
  }

  return tasks.pushTask(InstantiateCallTarget{
      .call = call,
      .generic = function,
      .typeArgs = task.bound,
      .loc = task.loc,
      .exact = {},
      .isCopyingReturn = false,
  });
}

void LibraryCommonRegistry::afterLibraryCallee(const AfterLibraryCallee &task) {
  CallExpr *call = task.call;
  FuncSymbol *function = task.function;
  auto &tasks = this->state.walk.tasks;
  FuncSymbol *callee = call->func();
  bool isOwnParams = callee == function;
  auto list = TypeListDraft{
      .types = {},
      .isValues = {},
      .params = function->typeParams,
      .args = task.bound,
      .index = 0,
      .results = {},
  };
  if (task.returnType != nullptr) {
    list.types.push_back(task.returnType);
    list.isValues.push_back(false);
  }
  if (isOwnParams) {
    for (const Type *param : function->params) {
      list.types.push_back(param);
      list.isValues.push_back(false);
    }
  }
  tasks.pushTask(FinishLibraryCall{
      .call = call,
      .callee = callee,
      .given = task.given,
      .loc = task.loc,
      .hasReturn = task.returnType != nullptr,
      .isOwnParams = isOwnParams,
  });

  return tasks.pushTask(SubstituteTypeList{.draft = std::move(list)});
}

void LibraryCommonRegistry::finishLibraryCall(const FinishLibraryCall &task) {
  CallExpr *call = task.call;
  FuncSymbol *callee = task.callee;
  auto &tasks = this->state.walk.tasks;
  size_t count = (task.hasReturn ? 1 : 0) + (task.isOwnParams ? callee->params.size() : 0);
  auto answers = tasks.takeAnswers(count);
  call->type = task.hasReturn ? answers.front() : Type::error();
  auto params = callee->params;
  if (task.isOwnParams) {
    params.assign(answers.begin() + (task.hasReturn ? 1 : 0), answers.end());
  }

  return tasks.pushTask(CheckArguments{
      .call = call,
      .params = std::move(params),
      .minArgs = callee->minArgs,
      .hasRest = callee->hasRest,
      .loc = task.loc,
      .checked = task.given,
  });
}

}
