#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/bodies/functions/closure_checker/closure_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterator_methods/iterator_methods.h"
#include "compiler/semantic/bodies/objects/dict_checker/dict_checker.h"
#include "compiler/semantic/bodies/objects/keyed_field_checker/keyed_field_checker.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/generics/generic_checker/generic_checker.h"
#include "compiler/semantic/infer/call_cycles/call_cycles.h"
#include "compiler/semantic/library/async/timer_checker/timer_checker.h"
#include "compiler/semantic/library/regex/regex_checker/regex_checker.h"
#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/destination/destination.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

VarChecker::VarChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->scopes = &all.scopes;
  this->callCycles = &all.callCycles;
  this->destination = &all.destination;
  this->dictChecker = &all.dictChecker;
  this->exprChecker = &all.exprChecker;
  this->flowChecker = &all.flowChecker;
  this->indexProofs = &all.indexProofs;
  this->regexChecker = &all.regexChecker;
  this->timerChecker = &all.timerChecker;
  this->typeResolver = &all.typeResolver;
  this->typeRelations = &all.typeRelations;
  this->closureChecker = &all.closureChecker;
  this->constantFolder = &all.constantFolder;
  this->genericChecker = &all.genericChecker;
  this->functionChecker = &all.functionChecker;
  this->iteratorMethods = &all.iteratorMethods;
  this->generatorChecker = &all.generatorChecker;
  this->tzModulesChecker = &all.tzModulesChecker;
  this->keyedFieldChecker = &all.keyedFieldChecker;
}

bool VarChecker::isTextChoice(const Expr *expr) {
  for (const Expr *branch : ExprParts::choiceBranches(expr)) {
    if (!isa<StringLitExpr>(branch)) {
      return false;
    }
  }

  return true;
}

std::vector<const Stmt *> VarChecker::followingStatements(llvm::ArrayRef<Stmt *> stmts, size_t i) {
  std::vector<const Stmt *> following;
  for (size_t j = 0; j < i; ++j) {
    if (llvm::isa<FunctionDecl>(stmts[j])) {
      following.push_back(stmts[j]);
    }
  }
  following.insert(following.end(), stmts.begin() + i + 1, stmts.end());

  return following;
}

void VarChecker::checkVarDecl(VarDeclStmt *decl) {
  BlockPosition &position = this->state.getBlockPosition();
  llvm::ArrayRef<Stmt *> block = position.getBlock();
  size_t blockIndex = position.getBlockIndex();
  position.forgetBlock();
  for (size_t number = 0; number < decl->decls.size(); ++number) {
    VarDeclarator &d = decl->decls[number];
    const Type *declared =
        d.typeAnn.has_value() ? this->typeResolver->resolveType(*d.typeAnn, false) : nullptr;
    DeclaratorPlace place{
        .decl = decl,
        .declarator = &d,
        .number = number,
        .declared = declared,
        .block = block,
        .blockIndex = blockIndex,
    };
    this->walkDeclarator(place);
  }
}

void VarChecker::walkDeclarator(const DeclaratorPlace &place) {
  DeclaratorForm form = this->declaratorForm(place);
  switch (form.kind()) {
    case DeclaratorKind::GENERIC_ALIAS:
      return this->declareGenericAlias(place, form.as<GenericAliasDeclarator>()->generic);
    case DeclaratorKind::GENERIC_FUNCTION:
      return this->liftGenericFunction(place, form.as<GenericFunctionDeclarator>()->function);
    case DeclaratorKind::CONST_FUNCTION:
      return this->declareConstFunction(place, form.as<ConstFunctionDeclarator>()->function);
    case DeclaratorKind::VALUE:
      return this->walkValueDeclarator(place);
  }
}

DeclaratorForm VarChecker::declaratorForm(const DeclaratorPlace &place) {
  const VarDeclStmt *decl = place.decl;
  const VarDeclarator &d = *place.declarator;
  bool plainConst = decl->isConst && place.declared == nullptr && !d.destructured();
  auto *name = dyn_cast_or_null<NameExpr>(d.init);
  if (name != nullptr && plainConst && name->typeArgs.empty()) {
    FuncSymbol *generic = this->genericChecker->genericNamed(name);
    if (generic != nullptr) {
      return GenericAliasDeclarator{.generic = generic};
    }
  }
  auto *function = dyn_cast_or_null<FunctionExpr>(d.init);
  bool genericFunction = function != nullptr && !function->fn->typeParams.empty() && plainConst &&
                         this->state.getScopeStack().isTopLevel();
  if (genericFunction) {
    return GenericFunctionDeclarator{.function = function->fn};
  }
  bool constFunction = function != nullptr && decl->isConst && !d.destructured();
  if (constFunction) {
    return ConstFunctionDeclarator{.function = function};
  }

  return ValueDeclarator{};
}

void VarChecker::declareGenericAlias(const DeclaratorPlace &place, FuncSymbol *generic) {
  VarDeclarator &d = *place.declarator;
  Scopes::ResultEntryDeclared added =
      this->scopes->declare(d.name, {FunctionEntry{.func = generic}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(d.loc, added.error().message);
  }
  bool showPrevious =
      added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME;
  if (showPrevious) {
    ScopeEntry previousEntry = this->scopes->lookup(d.name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
  d.alias = true;
}

void VarChecker::liftGenericFunction(const DeclaratorPlace &place, FunctionDecl *fn) {
  fn->place = FunctionPlace::LIFTED;
  fn->name = place.declarator->name;
  Module *module = this->state.getModuleStates().getCurrentModule();
  if (module != nullptr) {
    this->state.getModuleStates().rememberDeclModule(fn, module);
  }
  this->functionChecker->declareFunction(fn);
  this->callCycles->findCycles({fn});
  this->state.getReturnInference().addLiftedFunction(fn);
  place.declarator->alias = true;
}

void VarChecker::declareConstFunction(const DeclaratorPlace &place, FunctionExpr *function) {
  VarDeclarator &d = *place.declarator;
  const Type *declared = place.declared;
  VarChecker::nameFunctionValue(d);
  VarSymbol *var = this->scopes->declareVar(d.name, d.loc, nullptr, true);
  var->holds = Lambda{.lambda = function->fn};
  d.sym = var;
  bool functionDeclared = declared != nullptr && declared->nonOptional()->isFunction();
  const Type *functionType = functionDeclared ? declared->nonOptional() : nullptr;
  Signature signature =
      functionType != nullptr ? ClosureChecker::signatureOf(functionType) : Signature();
  const Signature *expectedSignature = functionType != nullptr ? &signature : nullptr;
  d.init = this->closureChecker->checkFunctionExpr(function, expectedSignature);
  if (declared != nullptr) {
    d.init = this->typeRelations->coerce(d.init, declared);
  }
  auto *adapted = dyn_cast<CastExpr>(d.init);
  if (adapted != nullptr && adapted->adaptsResult) {
    var->holds = OrdinaryValue{};
  }
  var->type = declared != nullptr ? declared : d.init->type;
}

void VarChecker::nameFunctionValue(VarDeclarator &d) {
  auto *function = dyn_cast_or_null<FunctionExpr>(d.init);
  if (function != nullptr && function->fn->name.empty()) {
    function->fn->name = d.name;
  }
}

void VarChecker::walkValueDeclarator(const DeclaratorPlace &place) {
  VarDeclarator &d = *place.declarator;
  VarChecker::nameFunctionValue(d);
  VariableValue variable{
      .type = place.declared,
      .hint = place.declared,
  };
  this->findDestination(place, variable);
  this->declareTimerEarly(place, variable);
  this->walkVariableValue(place, variable);
  this->walkDestructuring(place, variable);
  this->declareVariable(place, variable);
  this->regexChecker->noteRegexVariable(d.sym, variable.value, place.decl->isConst, place.block,
                                        place.blockIndex);
  if (place.decl->isConst && !d.destructured()) {
    this->rememberReflection(d, variable.value);
    this->markUnsharedArray(d, variable);
  }
  this->rememberConstLiteral(place, variable);
  if (!variable.places.empty()) {
    this->state.getTypeContext().rememberDestinations(d.sym, std::move(variable.places));
  }
  this->rememberResultVariable(place, variable);
  if (variable.untyped) {
    this->noteUntyped(d.sym, d.init != nullptr);
  }
  if (variable.value != nullptr) {
    this->flowChecker->assigned(d.sym, variable.value);
  }
  this->rememberFixedLength(place, variable);
  this->rememberConstNumber(place, variable);
  this->rememberConstText(place, variable);
}

void VarChecker::findDestination(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  bool literalShaped = place.declared == nullptr && d.init != nullptr && !d.destructured() &&
                       Destination::literalShaped(d.init);
  if (literalShaped) {
    std::vector<const Stmt *> following =
        place.block.empty() ? std::vector<const Stmt *>()
                            : VarChecker::followingStatements(place.block, place.blockIndex);
    this->state.getBlockPosition().rememberFollowing(std::move(following));
    llvm::ArrayRef<VarDeclarator> later =
        llvm::ArrayRef(place.decl->decls).drop_front(place.number + 1);
    variable.hint = this->destination->literalDestination(d, later);
    variable.places = this->state.getTypeContext().takeDestinationConflict();
  }
}

void VarChecker::declareTimerEarly(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  bool plainInit = d.init != nullptr && !d.destructured();
  const Type *timer = plainInit ? this->timerChecker->timerCallType(d.init) : nullptr;
  if (timer != nullptr) {
    const Type *type = place.declared != nullptr ? place.declared : timer;
    variable.early = this->scopes->declareVar(d.name, d.loc, type, place.decl->isConst);
    LocalVarState *localState = variable.early->local();
    if (localState != nullptr) {
      localState->declSeq = ~0u;
    }
  }
}

void VarChecker::walkVariableValue(const DeclaratorPlace &place, VariableValue &variable) {
  if (place.declarator->init != nullptr) {
    this->walkInit(place, variable);
  } else if (place.decl->isConst) {
    this->typeConstWithoutValue(place, variable);
  } else if (place.declared == nullptr) {
    variable.untyped = true;
    variable.type = Type::error();
  }
}

void VarChecker::walkInit(const DeclaratorPlace &place, VariableValue &variable) {
  VarDeclarator &d = *place.declarator;
  ExpressionMarks &marks = this->state.getExpressionMarks();
  bool arrayPattern = d.destructured() && !d.objectPattern();
  bool marksInit = variable.early == nullptr;
  if (marksInit) {
    marks.enterVariableInit(d.name);
  }
  if (arrayPattern) {
    this->iteratorMethods->markFused(d.init);
  }
  d.init = this->exprChecker->checkExpr(d.init, variable.hint);
  if (arrayPattern) {
    d.init = this->dictChecker->iterableOf(d.init);
  }
  variable.value = d.init;
  if (marksInit) {
    marks.leaveVariableInit();
  }
  if (place.declared != nullptr) {
    d.init = this->typeRelations->coerce(d.init, place.declared);
  } else {
    this->typeFromInit(place, variable);
  }
}

void VarChecker::typeFromInit(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  variable.type = d.init->type;
  ResultInitHasType typed = VarChecker::initHasType(place.decl, d, variable.type);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(d.init->loc(), typed.error().message);
    variable.type = Type::error();
  } else if (variable.type->isUndefined()) {
    variable.untyped = true;
    variable.type = Type::error();
  }
}

void VarChecker::typeConstWithoutValue(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  ResultConstValued valued = VarChecker::constValue(place.decl, d);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(d.loc, valued.error().message);
  }
  if (variable.type == nullptr) {
    variable.type = Type::error();
  }
}

void VarChecker::walkDestructuring(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  if (d.destructured() && d.objectPattern()) {
    return;
  }
  bool undeclaredPattern = d.destructured() && place.declared == nullptr;
  bool text = variable.type->isString() || variable.type->isTypedArray();
  if (undeclaredPattern && text) {
    return this->spreadTextIntoArray(place, variable);
  }
  if (undeclaredPattern && variable.type->isIterator()) {
    return this->destructureIterator(place, variable);
  }

  return this->reportPatternShape(place, variable);
}

void VarChecker::spreadTextIntoArray(const DeclaratorPlace &place, VariableValue &variable) {
  VarDeclarator &d = *place.declarator;
  const Type *element = variable.type->isString() ? Type::string() : variable.type->elementType();
  auto *characters = this->state.getAst().make<ArrayLitExpr>(d.init->loc());
  characters->elements.push_back({d.init, true});
  characters->type = Type::array(element);
  d.init = characters;
  variable.value = characters;
  variable.type = characters->type;
}

void VarChecker::destructureIterator(const DeclaratorPlace &place, VariableValue &variable) {
  VarDeclarator &d = *place.declarator;
  d.init = this->generatorChecker->destructuredIterator(d.init, d);
  variable.value = d.init;
  variable.type = d.init->type;
}

void VarChecker::reportPatternShape(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  ResultDestructure shaped = VarChecker::destructurable(d, variable.type);
  if (shaped.isErr()) {
    llvm::SMLoc at = d.init != nullptr ? d.init->loc() : d.loc;
    this->state.getDiagnostics().error(at, shaped.error().message);
    variable.type = Type::error();
  }
}

void VarChecker::declareVariable(const DeclaratorPlace &place, const VariableValue &variable) {
  VarDeclarator &d = *place.declarator;
  if (variable.early != nullptr) {
    LocalVarState *localState = variable.early->local();
    if (localState != nullptr) {
      localState->declSeq = this->state.getReturnInference().takeSequenceNumber();
    }
    d.sym = variable.early;
  } else {
    d.sym = this->scopes->declareVar(d.name, d.loc, variable.type, place.decl->isConst);
  }
}

void VarChecker::rememberReflection(VarDeclarator &d, const Expr *value) {
  auto *call = dyn_cast_or_null<CallExpr>(value);
  bool singleMethod = call != nullptr && call->builtin() == Builtin::ReflectMethod &&
                      !this->state.getExpressionMarks().hasSeveralVariants(call);
  if (singleMethod) {
    d.sym->holds = ReflectedMethod{.method = call->method()->decl};
  }
  bool readsByKey =
      call != nullptr && call->builtin() == Builtin::ReflectGet && call->args.size() == 2;
  if (readsByKey) {
    std::vector<llvm::StringRef> names = this->reflectedNames(call->args[1]);
    if (!names.empty()) {
      d.sym->holds = ReflectedNames{.names = std::move(names)};
    }
  }
}

std::vector<llvm::StringRef> VarChecker::reflectedNames(const Expr *key) {
  const Expr *bare = VarChecker::withoutImplicitCasts(key);
  std::vector<std::u16string> texts = VarChecker::literalKeyTexts(bare);
  std::vector<llvm::StringRef> names;
  for (const std::u16string &text : texts) {
    std::string utf8 = ToUtf8::toUtf8(text);
    names.push_back(this->state.getAst().keepName(utf8));
  }

  return names;
}

const Expr *VarChecker::withoutImplicitCasts(const Expr *expr) {
  const Expr *bare = expr;
  auto *cast = dyn_cast<CastExpr>(bare);
  while (cast != nullptr && cast->isImplicit()) {
    bare = cast->operand;
    cast = dyn_cast<CastExpr>(bare);
  }

  return bare;
}

std::vector<std::u16string> VarChecker::literalKeyTexts(const Expr *key) {
  auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal != nullptr) {
    return {literal->value};
  }
  if (key->type == nullptr) {
    return {};
  }
  const Type *keyType = key->type->nonOptional();
  std::vector<const Type *> members;
  if (keyType->isUnion()) {
    members.assign(keyType->members().begin(), keyType->members().end());
  } else {
    members.push_back(keyType);
  }
  std::vector<std::u16string> texts;
  for (const Type *member : members) {
    bool literalText = member->isLiteral() && member->literalBase()->isString();
    if (!literalText) {
      return {};
    }
    texts.push_back(member->literalText());
  }

  return texts;
}

void VarChecker::markUnsharedArray(const VarDeclarator &d, const VariableValue &variable) {
  LocalVarState *localState = d.sym->local();
  if (localState != nullptr) {
    bool fresh = VarChecker::isFreshArray(variable.value);
    localState->unshared =
        fresh && variable.type->isArray() && FlowChecker::staysLocal(d.sym->owner(), d.name);
  }
}

bool VarChecker::isFreshArray(const Expr *value) {
  if (llvm::isa_and_nonnull<ArrayLitExpr>(value)) {
    return true;
  }
  auto *made = dyn_cast_or_null<CallExpr>(value);
  if (made == nullptr) {
    return false;
  }
  Builtin builtin = made->builtin();

  return builtin == Builtin::ArrMap || builtin == Builtin::ArrFilter ||
         builtin == Builtin::ArrSlice || builtin == Builtin::ArrConcat ||
         builtin == Builtin::ArrFlatMap || builtin == Builtin::ArrayFrom;
}

void VarChecker::rememberConstLiteral(const DeclaratorPlace &place, const VariableValue &variable) {
  VarDeclarator &d = *place.declarator;
  const Expr *value = variable.value;
  ConstantStore &constants = this->state.getConstantStore();
  bool literal = place.decl->isConst && value != nullptr &&
                 (isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value) ||
                  (isa<ConditionalExpr>(value) && VarChecker::isTextChoice(value)));
  if (literal) {
    constants.rememberConstLiteral(d.sym, value);
    d.sym->literalConst = place.declared == nullptr && variable.type->isString();
  }
  auto *alias = dyn_cast_or_null<NameExpr>(value);
  bool textAlias = place.decl->isConst && alias != nullptr && alias->var() != nullptr &&
                   alias->var()->literalConst && place.declared == nullptr &&
                   variable.type->isString();
  if (textAlias) {
    const Expr *aliased = constants.findConstLiteral(alias->var());
    constants.rememberConstLiteral(d.sym, aliased);
    d.sym->literalConst = true;
  }
}

void VarChecker::rememberResultVariable(const DeclaratorPlace &place,
                                        const VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  const Type *held = variable.type->nonOptional();
  bool holdsResult = held->isResult() || (held->isPromise() && held->promised()->isResult());
  bool tracked = !d.destructured() && !this->state.getGenericStore().isCheckingInstance() &&
                 holdsResult && !this->exported(d.name);
  if (tracked) {
    this->state.getExpressionMarks().addResultVar(d.sym);
  }
}

void VarChecker::rememberFixedLength(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  const Type *base = variable.type->nonOptional();
  bool bytes = base->isByteView() || base->bytes() == Type::Bytes::ArrayBuffer;
  bool fixed = place.decl->isConst && variable.value != nullptr && !d.destructured() && bytes;
  if (fixed) {
    std::optional<Linear> length = this->indexProofs->knownLength(variable.value);
    bool constantLength = length.has_value() && length->term.isZero();
    if (constantLength) {
      this->state.getFlowState().rememberFixedLength(d.sym, length->offset);
    }
  }
}

void VarChecker::rememberConstNumber(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  bool integer = place.decl->isConst && d.init != nullptr && variable.type->isInteger();
  if (integer) {
    this->constantFolder->noteUnsignedVar(d.sym, d.init);
    std::optional<int64_t> constant = this->constantFolder->intConstant(d.init);
    if (constant.has_value()) {
      this->state.getConstantStore().rememberConstVar(d.sym, *constant);
      d.sym->literalConst = place.declared == nullptr && variable.type->isInt() &&
                            TypeRelations::isIntLiteralTree(d.init);
    }
  }
}

void VarChecker::rememberConstText(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &d = *place.declarator;
  bool text = place.decl->isConst && d.init != nullptr && variable.type->isString();
  if (text) {
    std::optional<std::string> key = this->keyedFieldChecker->constantKey(d.init);
    if (key.has_value()) {
      this->state.getConstantStore().rememberConstKey(d.sym, std::move(*key));
    }
    std::string prefix = this->tzModulesChecker->knownPrefix(d.init);
    if (!prefix.empty()) {
      this->state.getConstantStore().rememberConstPrefix(d.sym, std::move(prefix));
    }
  }
}

VarChecker::ResultInitHasType VarChecker::initHasType(const VarDeclStmt *decl,
                                                      const VarDeclarator &d, const Type *type) {
  if (type->isVoid()) {
    std::string message = ErrorTextVarInit::isVoid();

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::VOID_INITIALIZER,
        .message = message,
    });
  }
  if (type->isUndefined() && (decl->isConst || d.destructured())) {
    std::string message = ErrorTextVarInit::undefined(decl->isConst, d.name);

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::UNDEFINED_INITIALIZER,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultConstValued VarChecker::constValue(const VarDeclStmt *decl,
                                                     const VarDeclarator &d) {
  if (decl->isConst && !d.init) {
    std::string message = ErrorTextConstValue::missing();

    return err(VarCheckerConstValueError{
        .code = VarCheckerConstValueErrorCode::CONST_WITHOUT_VALUE,
        .message = message,
    });
  }

  return ok(&d);
}

VarChecker::ResultDestructure VarChecker::destructurable(const VarDeclarator &d, const Type *type) {
  bool fits = !d.destructured() || type->isArray() || type->isTuple() || type->isMatch() ||
              type->isError() || type->nonOptional()->isJson();
  if (!fits && type->isOptional() && type->inner()->isArray()) {
    std::string message = ErrorTextVarShape::mayBeUndef(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!fits) {
    std::string message = ErrorTextVarShape::notArray(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultResultCheck VarChecker::resultChecked(const VarSymbol *var) {
  const Type *held = var->type->nonOptional();
  if (held->isPromise()) {
    std::string message = ErrorTextResultUse::promise(var->name);

    return err(VarCheckerResultCheckedError{
        .code = VarCheckerResultCheckedErrorCode::PROMISE_NEVER_AWAITED,
        .message = message,
    });
  }

  std::string message = ErrorTextResultUse::unchecked(var->name);

  return err(VarCheckerResultCheckedError{
      .code = VarCheckerResultCheckedErrorCode::RESULT_NEVER_CHECKED,
      .message = message,
  });
}

VarChecker::ResultVarHasTypes VarChecker::varTyped(const Untyped &u, const Type *type) {
  llvm::StringRef name = u.var->name;
  std::string hint;
  if (type && !type->isError() && !type->isUndefined()) {
    hint = ErrorTextVarTyped::writeType(name, type->name());
  } else if (u.initUndefined) {
    hint = ErrorTextVarTyped::addOne(name);
  } else {
    hint = ErrorTextVarTyped::example(name);
  }
  if (u.initUndefined) {
    std::string message = ErrorTextVarTyped::undefined(hint);

    return err(VarCheckerVarTypedError{
        .code = VarCheckerVarTypedErrorCode::UNDEFINED_ONLY,
        .message = message,
    });
  }

  std::string message = ErrorTextVarTyped::needsType(name, hint);

  return err(VarCheckerVarTypedError{
      .code = VarCheckerVarTypedErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

bool VarChecker::exported(llvm::StringRef name) const {
  if (!this->state.getScopeStack().isTopLevel() ||
      !this->state.getModuleStates().getCurrentModule()) {
    return false;
  }

  return llvm::any_of(
      this->state.getModuleStates().getCurrentModule()->file.exports, [&](const ExportDecl &decl) {
        const LocalExport *local = decl.form.as<LocalExport>();

        return local &&
               llvm::any_of(local->names, [&](const ModuleName &n) { return n.name == name; });
      });
}

void VarChecker::reportUncheckedResults() {
  llvm::DenseMap<const char *, bool> readAt;
  std::vector<VarSymbol *> resultVars = this->state.getExpressionMarks().getResultVars();
  for (const VarSymbol *var : resultVars) {
    readAt[var->loc.getPointer()] |= var->read;
  }
  for (const VarSymbol *var : resultVars) {
    bool &done = readAt[var->loc.getPointer()];
    if (done) {
      continue;
    }
    done = true;
    ResultResultCheck checked = VarChecker::resultChecked(var);
    if (checked.isErr()) {
      this->state.getDiagnostics().error(var->loc, checked.error().message);
    }
  }
  this->state.getExpressionMarks().forgetResultVars();
}

void VarChecker::noteUntyped(VarSymbol *var, bool initUndefined) {
  this->state.getExpressionMarks().addUntyped(var, initUndefined);
}

void VarChecker::assignedUntyped(const VarSymbol *var, const Type *type) {
  if (!type->isError() && !type->isVoid()) {
    this->state.getExpressionMarks().addUntypedAssignment(var, type);
  }
}

void VarChecker::reportUntyped() {
  ExpressionMarks &marks = this->state.getExpressionMarks();
  for (const Untyped &u : marks.getUntyped()) {
    std::optional<ScopeEntry> found = this->state.getScopeStack().findInCurrentScope(u.var->name);
    if (!found || found->var() != u.var) {
      continue;
    }
    const Type *type = u.initUndefined ? Type::undefinedTy() : nullptr;
    for (const Type *t : u.assigned) {
      const Type *common = type ? TypeRelations::unify(type, t) : t;
      if (!common && type && type->nonOptional()->isNumeric() && t->isNumeric()) {
        common = type->nonOptional()->isFloat() ? type : t->isFloat() ? t : nullptr;
      }
      type = common ? common : Type::error();
    }
    ResultVarHasTypes typed = VarChecker::varTyped(u, type);
    if (typed.isErr()) {
      this->state.getDiagnostics().error(u.var->loc, typed.error().message);
    }
    marks.forgetUntyped(u.var);
  }
}

}
