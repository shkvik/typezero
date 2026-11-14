#include "compiler/semantic/code/functions/function_bodies/function_bodies.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

FunctionBodies::FunctionBodies(SemanticState &owner)
    : state(owner) {}

void FunctionBodies::walkFunctionBody(const WalkFunctionBody &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl->intrinsic()) {
    functionDecl->checked = true;
    return;
  }
  if (functionDecl->external()) {
    return this->state.walk.tasks.pushTask(WalkExternal{.functionDecl = functionDecl});
  }
  functionDecl->checked = true;
  auto &functions = this->state.walk.functions;
  auto *outer = functions.getCurrentFunction();
  functions.rememberCurrentFunction(functionDecl->sym);
  functions.enterFunction(functionDecl);
  this->state.diagnostics.pushList(DiagnosticRoute::PARENT);
  auto &classContext = this->state.walk.self;
  auto *ownerClass =
      functionDecl->isClosure() ? classContext.getCurrentClass() : functionDecl->cls();
  classContext.enterClass(ownerClass);
  this->state.walk.scopes.openScope();
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterBodyTypeParams{.functionDecl = functionDecl, .outer = outer});
  if (!functionDecl->typeArgs.empty()) {
    tasks.pushTask(BindFunctionTypeParams{.functionDecl = functionDecl});
  }
  if (functionDecl->cls() != nullptr) {
    tasks.pushTask(BindOuterTypeParams{.classDecl = functionDecl->cls()});
  }
  if (functionDecl->cls() != nullptr && !functionDecl->isStatic()) {
    return tasks.pushTask(BindClassTypeParams{.classDecl = functionDecl->cls()});
  }
}

void FunctionBodies::afterBodyTypeParams(const AfterBodyTypeParams &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  auto draft = BodyDraft{
      .functionDecl = functionDecl,
      .outer = task.outer,
      .isInitializer = false,
      .index = 0,
  };
  if (!functionDecl->takesThis()) {
    return this->nextBodyParam(NextBodyParam{.draft = draft});
  }
  this->state.walk.tasks.pushTask(AfterBodyThis{.draft = draft});

  return this->state.walk.tasks.pushTask(DeclareThis{.functionDecl = functionDecl});
}

void FunctionBodies::afterBodyThis(const AfterBodyThis &task) {
  auto draft = task.draft;
  FunctionDecl *functionDecl = draft.functionDecl;
  VarSymbol *self = functionDecl->thisVar();
  bool isInitializer = functionDecl->isConstructor() || functionDecl == functionDecl->cls()->init;
  if (isInitializer) {
    this->enterInitializer(functionDecl, self);
  }
  draft.isInitializer = isInitializer;

  return this->nextBodyParam(NextBodyParam{.draft = draft});
}

void FunctionBodies::enterInitializer(FunctionDecl *functionDecl, VarSymbol *self) {
  this->state.walk.self.enterInitializer(functionDecl, self);
  this->state.walk.flow.forgetSetFields();
  bool isDerived = functionDecl->cls()->parent != nullptr;
  for (const ClassField &field : functionDecl->cls()->fields) {
    if (this->isPresetField(functionDecl, field, isDerived)) {
      this->state.walk.flow.addSetField(field.name);
    }
  }
  if (isDerived && functionDecl->isConstructor()) {
    this->state.walk.flow.markBeforeSuper();
  } else {
    this->state.walk.flow.markSuperCalled();
  }
  if (this->state.walk.flow.isBeforeSuper()) {
    return this->walkRootSupers(functionDecl);
  }
}

void FunctionBodies::nextBodyParam(const NextBodyParam &task) {
  auto draft = task.draft;
  FunctionDecl *functionDecl = draft.functionDecl;
  auto &tasks = this->state.walk.tasks;
  if (draft.index == functionDecl->params.size()) {
    return this->startBodyStatements(draft);
  }
  Param &param = functionDecl->params[draft.index];
  const Type *type = param.declaredType;
  auto declare = DeclareVariableSlot{
      .slot = &param.sym,
      .name = param.name,
      .loc = param.loc,
      .type = type,
      .isConst = false,
  };
  auto *defaulted = param.form.as<DefaultedParam>();
  bool isUntyped = param.defaultValue() != nullptr && type->isError() && !param.type.has_value();
  if (isUntyped) {
    param.defaultValue()->type = type;
  }
  if (isUntyped || defaulted == nullptr) {
    draft.index += 1;
    tasks.pushTask(NextBodyParam{.draft = draft});
    return tasks.pushTask(declare);
  }
  auto names = llvm::StringSet<>();
  AstWalker::collectNames(defaulted->defaultValue, names);
  auto prior = this->checkDefaultOrder(functionDecl, draft.index, names);
  if (prior.isErr()) {
    this->state.diagnostics.error(defaulted->defaultValue->loc(), prior.error().message);
    defaulted->defaultValue->type = Type::error();
    draft.index += 1;
    tasks.pushTask(NextBodyParam{.draft = draft});
    return tasks.pushTask(declare);
  }
  auto *functionExpr = dyn_cast<FunctionExpr>(defaulted->defaultValue);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = param.name;
  }
  const Type *hint = type->isError() ? nullptr : type;
  tasks.pushTask(AfterDefaultedParam{.draft = draft});
  tasks.pushTask(declare);
  tasks.pushTask(CoerceSlot{.slot = &defaulted->defaultValue, .to = type, .what = ""});

  return tasks.pushTask(CheckExprSlot{.slot = &defaulted->defaultValue, .expected = hint});
}

void FunctionBodies::afterDefaultedParam(const AfterDefaultedParam &task) {
  auto draft = task.draft;
  Param &param = draft.functionDecl->params[draft.index];
  const Type *type = param.declaredType;
  const Expr *given = param.defaultValue();
  bool isDefined = type->isOptional() && !given->type->isOptional() && !given->type->isUndefined();
  if (isDefined) {
    this->state.walk.flow.markDefined(param.sym);
  }
  draft.index += 1;

  return this->nextBodyParam(NextBodyParam{.draft = draft});
}

void FunctionBodies::startBodyStatements(const BodyDraft &draft) {
  FunctionDecl *functionDecl = draft.functionDecl;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(AfterBodyStatements{.draft = draft});
  tasks.pushTask(WalkStatements{.block = functionDecl->body});
  tasks.pushTask(EnterErrorWatch{});
  tasks.pushTask(BeginPredicate{.functionDecl = functionDecl});
  tasks.pushTask(DeclareArgumentList{.functionDecl = functionDecl});
  tasks.pushTask(AddIndexCallbackFacts{.functionDecl = functionDecl});

  return tasks.pushTask(AddByteCallbackFacts{.functionDecl = functionDecl});
}

void FunctionBodies::enterErrorWatch() {
  return this->state.walk.flow.enterErrorWatch();
}

void FunctionBodies::afterBodyStatements(const AfterBodyStatements &task) {
  const BodyDraft &draft = task.draft;
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(FinishFunctionBody{.draft = draft});
  tasks.pushTask(CloseScope{});
  tasks.pushTask(AfterPredicateEnd{.functionDecl = draft.functionDecl});
  tasks.pushTask(EndPredicate{.functionDecl = draft.functionDecl});
  tasks.pushTask(LeaveErrorWatch{});
  if (!this->state.walk.flow.isUnreachable()) {
    return tasks.pushTask(ReportUnlistenedErrors{});
  }
}

void FunctionBodies::leaveErrorWatch() {
  return this->state.walk.flow.leaveErrorWatch();
}

void FunctionBodies::afterPredicateEnd(const AfterPredicateEnd &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  bool isConstructor = functionDecl->isConstructor() &&
                       this->state.walk.self.getInitializer() == functionDecl &&
                       !this->state.walk.flow.isBeforeSuper();
  if (isConstructor) {
    return this->state.walk.tasks.pushTask(CheckFieldsSet{.loc = functionDecl->loc()});
  }
}

void FunctionBodies::finishFunctionBody(const FinishFunctionBody &task) {
  const BodyDraft &draft = task.draft;
  FunctionDecl *functionDecl = draft.functionDecl;
  auto &functions = this->state.walk.functions;
  functions.leaveFunction();
  functions.rememberCurrentFunction(draft.outer);
  auto &classContext = this->state.walk.self;
  if (draft.isInitializer) {
    classContext.leaveInitializer();
  }
  classContext.leaveClass();
  this->walkBodyReturns(functionDecl);
  auto bodyDiagnostics = this->state.diagnostics.popList();

  return this->markBrokenOwners(functionDecl, bodyDiagnostics);
}

void FunctionBodies::walkInferredBody(const WalkInferredBody &task) {
  auto &tasks = this->state.walk.tasks;
  tasks.pushTask(LeaveReturnBody{});
  tasks.pushTask(WalkFunctionBody{.functionDecl = task.functionDecl});

  return tasks.pushTask(EnterReturnBody{.functionDecl = task.functionDecl});
}

void FunctionBodies::walkFunctionFromScratch(const WalkFunctionFromScratch &task) {
  this->state.walk.flow.replaceFlow(Flow());

  return this->walkFunctionBody(WalkFunctionBody{.functionDecl = task.functionDecl});
}

void FunctionBodies::declareArgumentList(const DeclareArgumentList &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl->listName.empty()) {
    return;
  }
  auto listParams = llvm::ArrayRef<Param>(functionDecl->params).drop_front(functionDecl->listStart);
  auto parts = std::vector<llvm::StringRef>();
  auto elements = std::vector<const Type *>();
  for (const Param &param : listParams) {
    parts.push_back(param.name);
    if (!param.rest()) {
      elements.push_back(param.declaredType);
    }
  }
  llvm::SMLoc loc = listParams.empty() ? functionDecl->loc() : listParams.front().loc;
  bool isFixed = functionDecl->listType->tupleRest() == nullptr;
  auto &tasks = this->state.walk.tasks;
  if (!this->isUsedAsValue(functionDecl->body, functionDecl->listName, isFixed)) {
    const auto *tuple = Type::tuple(elements);
    tasks.pushTask(RememberArgumentList{.name = functionDecl->listName, .parts = std::move(parts)});
    return tasks.pushTask(DeclareVariable{
        .name = functionDecl->listName,
        .loc = loc,
        .type = tuple,
        .isConst = true,
    });
  }
  auto spreadOnly = this->restListValue(functionDecl);
  if (spreadOnly.isErr()) {
    this->state.diagnostics.error(loc, spreadOnly.error().message);
    return tasks.pushTask(DeclareVariable{
        .name = functionDecl->listName,
        .loc = loc,
        .type = Type::error(),
        .isConst = true,
    });
  }

  return this->declareListValue(functionDecl, loc, parts, elements);
}

void FunctionBodies::rememberArgumentList(const RememberArgumentList &task) {
  auto found = this->state.walk.scopes.findInCurrentScope(task.name);
  VarSymbol *variable = found.has_value() ? found->var() : nullptr;
  if (variable == nullptr) {
    return;
  }
  auto parts = task.parts;

  return this->state.code.arguments.rememberArgumentList(variable, std::move(parts));
}

void FunctionBodies::startModuleBodies(const StartModuleBodies &task) {
  for (Stmt *item : llvm::reverse(task.module->file.items)) {
    this->pushItemBody(item);
  }

  return this->state.walk.tasks.pushTask(UseModule{.module = task.module});
}

void FunctionBodies::pushItemBody(Stmt *item) {
  auto *classDecl = dyn_cast<ClassDecl>(item);
  bool isClassWalked =
      classDecl != nullptr && classDecl->type != nullptr && classDecl->baseExpr == nullptr;
  if (isClassWalked) {
    return this->state.walk.tasks.pushTask(WalkClassBodies{.classDecl = classDecl});
  }
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl != nullptr) {
    return this->state.walk.tasks.pushTask(WalkTopFunction{.functionDecl = functionDecl});
  }
}

void FunctionBodies::startLiftedFunction(const StartLiftedFunction &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (this->state.code.late.isTemplate(functionDecl->sym)) {
    return;
  }
  this->state.walk.tasks.pushTask(WalkTopFunction{.functionDecl = functionDecl});

  return this->state.walk.tasks.pushTask(EnterHomeModule{.functionDecl = functionDecl});
}

void FunctionBodies::walkTopFunction(const WalkTopFunction &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (this->state.code.late.isTemplate(functionDecl->sym)) {
    return;
  }
  this->state.walk.tasks.pushTask(AfterTopReturnType{.functionDecl = functionDecl});
  if (!functionDecl->sym->hasReturnType()) {
    return this->state.walk.tasks.pushTask(InferReturnType{.function = functionDecl->sym});
  }
}

void FunctionBodies::afterTopReturnType(const AfterTopReturnType &task) {
  if (!task.functionDecl->checked) {
    return this->state.walk.tasks.pushTask(
        WalkFunctionFromScratch{.functionDecl = task.functionDecl});
  }
}

FunctionBodies::ResultConstructorSuper
FunctionBodies::checkSuperCalled(const FunctionDecl *functionDecl, unsigned supers) const {
  if (supers == 0) {
    auto message = ErrorTextFunctionBody::noSuper(functionDecl->cls()->name,
                                                  functionDecl->cls()->parent->name);

    return err(CodeFunctionsRegistrySuperCalledError{
        .code = CodeFunctionsRegistrySuperCalledErrorCode::NO_SUPER_CALL,
        .message = message,
    });
  }

  return ok(functionDecl);
}

FunctionBodies::ResultDefaultUsesPrior
FunctionBodies::checkDefaultOrder(const FunctionDecl *functionDecl, size_t index,
                                  const llvm::StringSet<> &names) const {
  for (size_t later = index; later < functionDecl->params.size(); ++later) {
    const Param &laterParam = functionDecl->params[later];
    if (names.contains(laterParam.name)) {
      auto message =
          ErrorTextFunctionBody::forward(functionDecl->params[index].label(), laterParam.name);

      return err(CodeFunctionsRegistryDefaultOrderError{
          .code = CodeFunctionsRegistryDefaultOrderErrorCode::FORWARD_REFERENCE,
          .message = message,
      });
    }
  }

  return ok(functionDecl);
}

FunctionBodies::ResultEveryPathReturns
FunctionBodies::checkReturnsEverywhere(const FunctionDecl *functionDecl,
                                       const Type *returnType) const {
  bool isMissing = returnType != nullptr && !returnType->isVoid() && !returnType->isError() &&
                   !returnType->isOptional() && !returnType->isUnknown() &&
                   !this->state.flow.reachability.isAlwaysReturning(functionDecl->body);
  if (isMissing) {
    auto label = ErrorTextFunctionName::label(functionDecl->name);
    auto message = ErrorTextFunctionBody::noReturn(label, returnType->name());

    return err(CodeFunctionsRegistryReturnsEverywhereError{
        .code = CodeFunctionsRegistryReturnsEverywhereErrorCode::MISSING_RETURN,
        .message = message,
    });
  }

  return ok(functionDecl);
}

void FunctionBodies::walkBodyReturns(const FunctionDecl *functionDecl) {
  const auto *returnType = this->bodyReturnType(functionDecl);
  auto returns = this->checkReturnsEverywhere(functionDecl, returnType);
  if (returns.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), returns.error().message);
  }
}

void FunctionBodies::markBrokenOwners(const FunctionDecl *functionDecl,
                                      const std::vector<Diagnostic> &bodyDiagnostics) {
  auto destination = this->state.diagnostics.getCurrentDestination();
  bool isReachingProgram = destination == DiagnosticRoute::PROGRAM;
  bool hasFailed = Diagnostics::hasErrors(bodyDiagnostics) && isReachingProgram;
  if (functionDecl->sym->isGeneric() && hasFailed) {
    this->state.generics.copies.markBrokenGeneric(functionDecl->sym);
  }
  if (functionDecl->cls() != nullptr && functionDecl->cls()->isGeneric() && hasFailed) {
    this->state.generics.copies.markBrokenClass(functionDecl->cls());
  }
}

bool FunctionBodies::isPresetField(const FunctionDecl *functionDecl, const ClassField &field,
                                   bool isDerived) const {
  if (field.isStatic()) {
    return false;
  }
  bool isOwnInit = functionDecl->isConstructor() && !isDerived && field.init != nullptr &&
                   field.method() == nullptr;

  return field.optional() || isOwnInit || (!functionDecl->isConstructor() && field.redeclared);
}

void FunctionBodies::walkRootSupers(FunctionDecl *functionDecl) {
  unsigned supers = 0;
  for (Stmt *stmt : functionDecl->body->body) {
    auto *call = this->superCallOf(stmt);
    if (call != nullptr) {
      this->state.code.constructors.markRootSuper(call);
      ++supers;
    }
  }
  auto called = this->checkSuperCalled(functionDecl, supers);
  if (called.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), called.error().message);
  }
}

CallExpr *FunctionBodies::superCallOf(Stmt *stmt) const {
  auto *expr = dyn_cast<ExprStmt>(stmt);
  auto *call = expr != nullptr ? dyn_cast<CallExpr>(expr->expr) : nullptr;
  auto *callee = call != nullptr ? dyn_cast<NameExpr>(call->callee) : nullptr;
  if (callee == nullptr || callee->name != KeywordTable::getName(Keyword::SUPER)) {
    return nullptr;
  }

  return call;
}

const Type *FunctionBodies::bodyReturnType(const FunctionDecl *functionDecl) const {
  FuncSymbol *function = functionDecl->sym;
  bool isPromised = functionDecl->isAsync && function->hasReturnType() &&
                    function->getKnownReturnType()->isPromise();
  if (isPromised) {
    return function->getKnownReturnType()->promised();
  }
  if (!functionDecl->isGenerator) {
    return function->getKnownReturnType();
  }
  bool isIterating =
      function->hasReturnType() && (function->getKnownReturnType()->isIterator() ||
                                    function->getKnownReturnType()->isAsyncIterator());

  return isIterating ? function->getKnownReturnType()->returned() : nullptr;
}

bool FunctionBodies::isUsedAsValue(const Stmt *body, llvm::StringRef name, bool isFixed) const {
  auto nodes = PlacedNodes::collect({body}, {});
  auto listUses = this->listUses(nodes, isFixed);
  for (const PlacedExpr &placed : nodes.getExprs()) {
    auto *named = dyn_cast<NameExpr>(placed.expr);
    if (named != nullptr && named->name == name && !listUses.contains(named)) {
      return true;
    }
  }

  return false;
}

llvm::SmallPtrSet<const Expr *, 4> FunctionBodies::listUses(const PlacedNodes &nodes,
                                                            bool isFixed) const {
  auto uses = llvm::SmallPtrSet<const Expr *, 4>();
  for (const PlacedExpr &placed : nodes.getExprs()) {
    this->addSpreadUses(placed.expr, uses);
    if (isFixed) {
      this->addFixedUses(placed.expr, uses);
    }
  }

  return uses;
}

void FunctionBodies::addSpreadUses(const Expr *expr,
                                   llvm::SmallPtrSetImpl<const Expr *> &uses) const {
  auto *call = dyn_cast<CallExpr>(expr);
  auto *literal = dyn_cast<ArrayLitExpr>(expr);
  if (call != nullptr) {
    for (size_t position = 0; position < call->args.size(); ++position) {
      if (call->spreads(position)) {
        uses.insert(call->args[position]);
      }
    }
  }
  if (literal != nullptr) {
    for (const ArrayLitExpr::Element &element : literal->elements) {
      if (element.spread) {
        uses.insert(element.expr);
      }
    }
  }
}

void FunctionBodies::addFixedUses(const Expr *expr,
                                  llvm::SmallPtrSetImpl<const Expr *> &uses) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  if (member != nullptr && BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)) {
    uses.insert(member->object);
  }
  if (index != nullptr && isa<IntLitExpr>(index->index)) {
    uses.insert(index->object);
  }
}

void FunctionBodies::declareListValue(FunctionDecl *functionDecl, llvm::SMLoc loc,
                                      llvm::ArrayRef<llvm::StringRef> parts,
                                      llvm::ArrayRef<const Type *> elements) {
  auto &ast = this->state.ast;
  auto *alias = ast.make<TypeAliasDecl>(loc, ast.hiddenName(), TypeRef());
  alias->resolved = Type::tuple(elements);
  this->state.walk.scopes.setType(alias->name, alias);
  auto *tuple = ast.make<ArrayLitExpr>(loc);
  for (llvm::StringRef part : parts) {
    auto *partName = ast.make<NameExpr>(loc, part);
    auto element = ArrayLitExpr::Element{.expr = partName, .spread = false};
    tuple->elements.push_back(element);
  }
  TypeRef ref;
  ref.name = alias->name;
  ref.loc = loc;
  VarDeclarator declarator;
  declarator.name = functionDecl->listName;
  declarator.loc = loc;
  declarator.typeAnn = ref;
  declarator.init = tuple;
  auto *varDecl = ast.make<VarDeclStmt>(loc, true);
  varDecl->decls.push_back(declarator);
  functionDecl->body->body.insert(functionDecl->body->body.begin(), varDecl);
}

FunctionBodies::ResultListIsSpread
FunctionBodies::restListValue(const FunctionDecl *functionDecl) const {
  if (functionDecl->listType->tupleRest() != nullptr) {
    auto message =
        ErrorTextRestList::spreadOnly(functionDecl->listName, functionDecl->listType->name());

    return err(CodeFunctionsRegistryRestListValueError{
        .code = CodeFunctionsRegistryRestListValueErrorCode::REST_LIST_AS_VALUE,
        .message = message,
    });
  }

  return ok(functionDecl);
}

}
