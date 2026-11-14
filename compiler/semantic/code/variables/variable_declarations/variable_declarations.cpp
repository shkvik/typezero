#include "compiler/semantic/code/variables/variable_declarations/variable_declarations.h"
#include "compiler/semantic/semantic_state/v2/walk/scope_stack_context/scope_stack_context_errors.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr unsigned kNoSequence = ~0u;

}

VariableDeclarations::VariableDeclarations(SemanticState &owner)
    : state(owner) {}

void VariableDeclarations::walkVarDecl(const WalkVarDecl &task) {
  auto &position = this->state.walk.block;
  auto block = position.getBlock();
  size_t blockIndex = position.getBlockIndex();
  position.forgetBlock();
  auto draft = DeclaratorDraft{
      .decl = task.varDecl,
      .number = 0,
      .declared = nullptr,
      .block = block,
      .blockIndex = blockIndex,
      .type = nullptr,
      .hint = nullptr,
      .value = nullptr,
      .isEarly = false,
      .isUntyped = false,
      .places = {},
  };

  return this->nextDeclarator(NextDeclarator{.draft = std::move(draft)});
}

void VariableDeclarations::nextDeclarator(const NextDeclarator &task) {
  const DeclaratorDraft &draft = task.draft;
  if (draft.number == draft.decl->decls.size()) {
    return;
  }
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  if (!declarator.typeAnn.has_value()) {
    return this->startDeclarator(draft);
  }
  this->state.walk.tasks.pushTask(AfterDeclaredType{.draft = draft});

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &*declarator.typeAnn, .use = TypeUse::STANDALONE});
}

void VariableDeclarations::afterDeclaredType(const AfterDeclaredType &task) {
  auto draft = task.draft;
  draft.declared = this->state.walk.tasks.takeAnswer();

  return this->startDeclarator(draft);
}

void VariableDeclarations::startDeclarator(const DeclaratorDraft &draft) {
  auto next = DeclaratorDraft{
      .decl = draft.decl,
      .number = draft.number + 1,
      .declared = nullptr,
      .block = draft.block,
      .blockIndex = draft.blockIndex,
      .type = nullptr,
      .hint = nullptr,
      .value = nullptr,
      .isEarly = false,
      .isUntyped = false,
      .places = {},
  };
  this->state.walk.tasks.pushTask(NextDeclarator{.draft = std::move(next)});
  auto form = this->declaratorForm(draft);
  switch (form.kind()) {
    case DeclaratorKind::GENERIC_ALIAS:
      return this->declareGenericAlias(draft, form.as<GenericAliasDeclarator>()->generic);
    case DeclaratorKind::GENERIC_FUNCTION:
      return this->liftGenericFunction(draft, form.as<GenericFunctionDeclarator>()->function);
    case DeclaratorKind::CONST_FUNCTION:
      return this->startConstFunction(draft, form.as<ConstFunctionDeclarator>()->function);
    case DeclaratorKind::VALUE:
      return this->startValueDeclarator(draft);
  }
}

DeclaratorForm VariableDeclarations::declaratorForm(const DeclaratorDraft &draft) const {
  const VarDeclStmt *varDecl = draft.decl;
  const VarDeclarator &declarator = varDecl->decls[draft.number];
  bool isPlainConst = varDecl->isConst && draft.declared == nullptr && !declarator.destructured();
  auto *name = dyn_cast_or_null<NameExpr>(declarator.init);
  bool isPlainName = isPlainConst && name != nullptr && name->typeArgs.empty();
  auto *generic = isPlainName ? this->state.walk.scopes.findGenericFunction(name) : nullptr;
  if (generic != nullptr) {
    return GenericAliasDeclarator{.generic = generic};
  }
  auto *functionExpr = dyn_cast_or_null<FunctionExpr>(declarator.init);
  bool isGenericFunction = functionExpr != nullptr && !functionExpr->fn->typeParams.empty() &&
                           isPlainConst && this->state.walk.scopes.isTopLevel();
  if (isGenericFunction) {
    return GenericFunctionDeclarator{.function = functionExpr->fn};
  }
  bool isConstFunction = functionExpr != nullptr && varDecl->isConst && !declarator.destructured();
  if (isConstFunction) {
    return ConstFunctionDeclarator{.function = functionExpr};
  }

  return ValueDeclarator{};
}

void VariableDeclarations::declareGenericAlias(const DeclaratorDraft &draft, FuncSymbol *generic) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  declarator.alias = true;
  auto added = this->state.walk.scopes.declare(declarator.name, {FunctionEntry{.func = generic}});
  if (added.isOk()) {
    return;
  }
  this->state.diagnostics.error(declarator.loc, added.error().message);
  if (added.error().code == ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  auto previousEntry = this->state.walk.scopes.findName(declarator.name);
  auto declaredHere = ErrorTextDeclareName::previous();

  return this->state.diagnostics.note(previousEntry.loc(), declaredHere);
}

void VariableDeclarations::liftGenericFunction(const DeclaratorDraft &draft,
                                               FunctionDecl *functionDecl) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  functionDecl->place = FunctionPlace::LIFTED;
  functionDecl->name = declarator.name;
  auto *module = this->state.declaration.modules.getCurrentModule();
  if (module != nullptr) {
    this->state.declaration.modules.rememberDeclModule(functionDecl, module);
  }
  this->state.code.variables.addLiftedFunction(functionDecl);
  declarator.alias = true;
  this->state.walk.tasks.pushTask(FindCallCycles{.functions = {functionDecl}});

  return this->state.walk.tasks.pushTask(
      StartFunctionSymbol{.functionDecl = functionDecl, .variable = nullptr, .isNamed = true});
}

void VariableDeclarations::startConstFunction(const DeclaratorDraft &draft,
                                              FunctionExpr *functionExpr) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  this->nameFunctionValue(declarator);
  this->state.walk.tasks.pushTask(
      AfterConstFunctionSymbol{.draft = draft, .functionExpr = functionExpr});

  return this->state.walk.tasks.pushTask(DeclareVariableSlot{
      .slot = &declarator.sym,
      .name = declarator.name,
      .loc = declarator.loc,
      .type = nullptr,
      .isConst = true,
  });
}

void VariableDeclarations::afterConstFunctionSymbol(const AfterConstFunctionSymbol &task) {
  const DeclaratorDraft &draft = task.draft;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  const Type *declared = draft.declared;
  declarator.sym->holds = Lambda{.lambda = task.functionExpr->fn};
  this->state.walk.tasks.pushTask(FinishConstFunction{.draft = draft});
  if (declared != nullptr) {
    this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &declarator.init, .to = declared, .what = ""});
  }
  this->state.walk.tasks.pushTask(WriteCheckedSlot{.slot = &declarator.init});
  bool isFunctionDeclared = declared != nullptr && declared->nonOptional()->isFunction();
  std::optional<Signature> expected = std::nullopt;
  if (isFunctionDeclared) {
    expected = this->signatureOf(declared->nonOptional());
  }

  return this->state.walk.tasks.pushTask(
      CheckFunctionExpr{.functionExpr = task.functionExpr, .expected = std::move(expected)});
}

Signature VariableDeclarations::signatureOf(const Type *functionType) const {
  llvm::ArrayRef<const Type *> params = functionType->params();

  return Signature{
      .params = std::vector<const Type *>(params.begin(), params.end()),
      .minArgs = functionType->minArgs(),
      .ret = functionType->ret(),
      .voidResult = functionType->ret()->isVoid(),
      .hasThis = functionType->hasThis(),
  };
}

void VariableDeclarations::finishConstFunction(const FinishConstFunction &task) {
  const DeclaratorDraft &draft = task.draft;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  VarSymbol *variable = declarator.sym;
  auto *adapted = dyn_cast<CastExpr>(declarator.init);
  if (adapted != nullptr && adapted->adaptsResult) {
    variable->holds = OrdinaryValue{};
  }
  variable->type = draft.declared != nullptr ? draft.declared : declarator.init->type;
}

void VariableDeclarations::startValueDeclarator(const DeclaratorDraft &start) {
  auto draft = start;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  this->nameFunctionValue(declarator);
  draft.type = draft.declared;
  draft.hint = draft.declared;
  if (!this->isSeekingDestination(draft)) {
    return this->startValueOrTimer(std::move(draft));
  }

  return this->findDestination(FindDestination{
      .draft = std::move(draft),
      .types = DestinationTypes{.known = {}, .missing = {}},
  });
}

void VariableDeclarations::startValueOrTimer(DeclaratorDraft draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  bool isPlainInit = declarator.init != nullptr && !declarator.destructured();
  const Type *timer =
      isPlainInit ? this->state.library.timers.timerCallType(declarator.init) : nullptr;
  if (timer == nullptr) {
    return this->startVariableValue(std::move(draft));
  }
  draft.isEarly = true;
  const Type *type = draft.declared != nullptr ? draft.declared : timer;
  bool isConst = draft.decl->isConst;
  this->state.walk.tasks.pushTask(AfterEarlyTimer{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(DeclareVariableSlot{
      .slot = &declarator.sym,
      .name = declarator.name,
      .loc = declarator.loc,
      .type = type,
      .isConst = isConst,
  });
}

bool VariableDeclarations::isSeekingDestination(const DeclaratorDraft &draft) const {
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  bool isPlainInit = declarator.init != nullptr && !declarator.destructured();

  return draft.declared == nullptr && isPlainInit &&
         this->state.inference.destinations.isLiteralShaped(declarator.init);
}

void VariableDeclarations::findDestination(const FindDestination &task) {
  auto draft = task.draft;
  auto types = task.types;
  auto &tasks = this->state.walk.tasks;
  auto resolved = tasks.takeAnswers(types.missing.size());
  for (size_t index = 0; index < resolved.size(); ++index) {
    types.known.push_back({types.missing[index].first, resolved[index]});
  }
  types.missing.clear();
  const VarDeclarator &declarator = draft.decl->decls[draft.number];
  auto following = draft.block.empty() ? std::vector<const Stmt *>()
                                       : this->followingStatements(draft.block, draft.blockIndex);
  llvm::ArrayRef<VarDeclarator> declarators = draft.decl->decls;
  auto later = declarators.drop_front(draft.number + 1);
  auto &destinations = this->state.inference.destinations;
  auto found = destinations.literalDestination(declarator, later, following, types);
  if (types.missing.empty()) {
    draft.hint = found.hint;
    draft.places = std::move(found.conflict);

    return this->startValueOrTimer(std::move(draft));
  }
  auto missing = types.missing;
  tasks.pushTask(FindDestination{.draft = std::move(draft), .types = std::move(types)});
  tasks.pushTask(DropDiagnosticList{});
  for (const auto &[ref, use] : llvm::reverse(missing)) {
    tasks.pushTask(ResolveTypeAnswer{.ref = ref, .use = use});
  }

  return this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
}

void VariableDeclarations::afterEarlyTimer(const AfterEarlyTimer &task) {
  const DeclaratorDraft &draft = task.draft;
  LocalVarState *localState = draft.decl->decls[draft.number].sym->local();
  if (localState != nullptr) {
    localState->declSeq = kNoSequence;
  }

  return this->startVariableValue(draft);
}

void VariableDeclarations::startVariableValue(DeclaratorDraft draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  if (declarator.init != nullptr) {
    return this->startInit(std::move(draft));
  }
  if (draft.decl->isConst) {
    auto valued = this->checkConstValue(draft.decl, declarator);
    if (valued.isErr()) {
      this->state.diagnostics.error(declarator.loc, valued.error().message);
    }
    draft.type = draft.type != nullptr ? draft.type : Type::error();
  } else if (draft.declared == nullptr) {
    draft.isUntyped = true;
    draft.type = Type::error();
  }

  return this->destructureValue(DestructureValue{.draft = std::move(draft)});
}

void VariableDeclarations::startInit(DeclaratorDraft draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  bool isArrayPattern = declarator.destructured() && !declarator.objectPattern();
  bool isMarkingInit = !draft.isEarly;
  if (isMarkingInit) {
    this->state.code.variables.enterVariableInit(declarator.name);
  }
  const Type *hint = draft.hint;
  this->state.walk.tasks.pushTask(AfterInit{.draft = std::move(draft)});
  if (isArrayPattern) {
    this->state.walk.tasks.pushTask(IterableOfSlot{.slot = &declarator.init});
  }
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &declarator.init, .expected = hint});
  if (isArrayPattern) {
    this->state.walk.tasks.pushTask(MarkFused{.expr = declarator.init});
  }
}

void VariableDeclarations::afterInit(const AfterInit &task) {
  auto draft = task.draft;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  draft.value = declarator.init;
  if (!draft.isEarly) {
    this->state.code.variables.leaveVariableInit();
  }
  if (draft.declared != nullptr) {
    const Type *declared = draft.declared;
    this->state.walk.tasks.pushTask(DestructureValue{.draft = std::move(draft)});
    return this->state.walk.tasks.pushTask(
        CoerceSlot{.slot = &declarator.init, .to = declared, .what = ""});
  }
  draft.type = declarator.init->type;
  auto typed = this->checkInitHasType(draft.decl, declarator, draft.type);
  if (typed.isErr()) {
    this->state.diagnostics.error(declarator.init->loc(), typed.error().message);
    draft.type = Type::error();
  } else if (draft.type->isUndefined()) {
    draft.isUntyped = true;
    draft.type = Type::error();
  }

  return this->destructureValue(DestructureValue{.draft = std::move(draft)});
}

void VariableDeclarations::destructureValue(const DestructureValue &task) {
  auto draft = task.draft;
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  if (declarator.destructured() && declarator.objectPattern()) {
    return this->declareValueVariable(std::move(draft));
  }
  bool isUndeclaredPattern = declarator.destructured() && draft.declared == nullptr;
  bool isText = draft.type->isString() || draft.type->isTypedArray();
  if (isUndeclaredPattern && isText) {
    this->spreadTextIntoArray(draft);
    return this->declareValueVariable(std::move(draft));
  }
  if (isUndeclaredPattern && draft.type->isIterator()) {
    this->state.walk.tasks.pushTask(AfterDestructuredIterator{.draft = draft});
    return this->state.walk.tasks.pushTask(
        DestructuredIteratorSlot{.slot = &declarator.init, .declarator = &declarator});
  }
  auto shaped = this->checkDestructurable(declarator, draft.type);
  if (shaped.isErr()) {
    llvm::SMLoc at = declarator.init != nullptr ? declarator.init->loc() : declarator.loc;
    this->state.diagnostics.error(at, shaped.error().message);
    draft.type = Type::error();
  }

  return this->declareValueVariable(std::move(draft));
}

void VariableDeclarations::spreadTextIntoArray(DeclaratorDraft &draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  const Type *element = draft.type->isString() ? Type::string() : draft.type->elementType();
  auto *characters = this->state.ast.make<ArrayLitExpr>(declarator.init->loc());
  characters->elements.push_back(ArrayLitExpr::Element{.expr = declarator.init, .spread = true});
  characters->type = Type::array(element);
  declarator.init = characters;
  draft.value = characters;
  draft.type = characters->type;
}

void VariableDeclarations::afterDestructuredIterator(const AfterDestructuredIterator &task) {
  auto draft = task.draft;
  Expr *init = draft.decl->decls[draft.number].init;
  draft.value = init;
  draft.type = init->type;

  return this->declareValueVariable(std::move(draft));
}

void VariableDeclarations::declareValueVariable(DeclaratorDraft draft) {
  VarDeclarator &declarator = draft.decl->decls[draft.number];
  if (draft.isEarly) {
    LocalVarState *localState = declarator.sym->local();
    if (localState != nullptr) {
      localState->declSeq = this->state.declaration.order.takeSequenceNumber();
    }
    return this->state.walk.tasks.pushTask(RememberValueDeclarator{.draft = std::move(draft)});
  }
  const Type *type = draft.type;
  bool isConst = draft.decl->isConst;
  this->state.walk.tasks.pushTask(RememberValueDeclarator{.draft = std::move(draft)});

  return this->state.walk.tasks.pushTask(DeclareVariableSlot{
      .slot = &declarator.sym,
      .name = declarator.name,
      .loc = declarator.loc,
      .type = type,
      .isConst = isConst,
  });
}

std::vector<const Stmt *> VariableDeclarations::followingStatements(llvm::ArrayRef<Stmt *> stmts,
                                                                    size_t index) const {
  auto following = std::vector<const Stmt *>();
  for (size_t earlier = 0; earlier < index; ++earlier) {
    if (isa<FunctionDecl>(stmts[earlier])) {
      following.push_back(stmts[earlier]);
    }
  }
  following.insert(following.end(), stmts.begin() + index + 1, stmts.end());

  return following;
}

void VariableDeclarations::nameFunctionValue(VarDeclarator &declarator) const {
  auto *functionExpr = dyn_cast_or_null<FunctionExpr>(declarator.init);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = declarator.name;
  }
}

VariableDeclarations::ResultInitHasType
VariableDeclarations::checkInitHasType(const VarDeclStmt *varDecl, const VarDeclarator &declarator,
                                       const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextVarInit::isVoid();

    return err(CodeVariablesRegistryInitHasTypeError{
        .code = CodeVariablesRegistryInitHasTypeErrorCode::VOID_INITIALIZER,
        .message = message,
    });
  }
  if (type->isUndefined() && (varDecl->isConst || declarator.destructured())) {
    auto message = ErrorTextVarInit::undefined(varDecl->isConst, declarator.name);

    return err(CodeVariablesRegistryInitHasTypeError{
        .code = CodeVariablesRegistryInitHasTypeErrorCode::UNDEFINED_INITIALIZER,
        .message = message,
    });
  }

  return ok(type);
}

VariableDeclarations::ResultConstValued
VariableDeclarations::checkConstValue(const VarDeclStmt *varDecl,
                                      const VarDeclarator &declarator) const {
  if (varDecl->isConst && declarator.init == nullptr) {
    auto message = ErrorTextConstValue::missing();

    return err(CodeVariablesRegistryConstValueError{
        .code = CodeVariablesRegistryConstValueErrorCode::CONST_WITHOUT_VALUE,
        .message = message,
    });
  }

  return ok(&declarator);
}

VariableDeclarations::ResultDestructure
VariableDeclarations::checkDestructurable(const VarDeclarator &declarator, const Type *type) const {
  bool isFitting = !declarator.destructured() || type->isArray() || type->isTuple() ||
                   type->isMatch() || type->isError() || type->nonOptional()->isJson();
  if (!isFitting && type->isOptional() && type->inner()->isArray()) {
    auto message = ErrorTextVarShape::mayBeUndef(type->name());

    return err(CodeVariablesRegistryDestructurableError{
        .code = CodeVariablesRegistryDestructurableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextVarShape::notArray(type->name());

    return err(CodeVariablesRegistryDestructurableError{
        .code = CodeVariablesRegistryDestructurableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

}
