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

namespace {

constexpr unsigned kNoSequence = ~0u;

}

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

bool VarChecker::isTextChoice(const Expr *expr) const {
  for (const Expr *branch : ExprParts::choiceBranches(expr)) {
    if (!isa<StringLitExpr>(branch)) {
      return false;
    }
  }

  return true;
}

std::vector<const Stmt *> VarChecker::followingStatements(llvm::ArrayRef<Stmt *> stmts,
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

void VarChecker::walkVarDecl(VarDeclStmt *varDecl) {
  auto &position = this->state.getBlockPosition();
  auto block = position.getBlock();
  size_t blockIndex = position.getBlockIndex();
  position.forgetBlock();
  for (size_t number = 0; number < varDecl->decls.size(); ++number) {
    VarDeclarator &declarator = varDecl->decls[number];
    const auto *declared = this->declaredType(declarator);
    auto place = DeclaratorPlace{
        .decl = varDecl,
        .declarator = &declarator,
        .number = number,
        .declared = declared,
        .block = block,
        .blockIndex = blockIndex,
    };
    this->walkDeclarator(place);
  }
}

const Type *VarChecker::declaredType(const VarDeclarator &declarator) {
  if (!declarator.typeAnn.has_value()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(*declarator.typeAnn);
}

void VarChecker::walkDeclarator(const DeclaratorPlace &place) {
  auto form = this->declaratorForm(place);
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
  const VarDeclStmt *varDecl = place.decl;
  const VarDeclarator &declarator = *place.declarator;
  bool isPlainConst = varDecl->isConst && place.declared == nullptr && !declarator.destructured();
  auto *generic = isPlainConst ? this->aliasedGeneric(declarator.init) : nullptr;
  if (generic != nullptr) {
    return GenericAliasDeclarator{.generic = generic};
  }
  auto *functionExpr = dyn_cast_or_null<FunctionExpr>(declarator.init);
  bool isGenericFunction = functionExpr != nullptr && !functionExpr->fn->typeParams.empty() &&
                           isPlainConst && this->state.getScopeStack().isTopLevel();
  if (isGenericFunction) {
    return GenericFunctionDeclarator{.function = functionExpr->fn};
  }
  bool isConstFunction = functionExpr != nullptr && varDecl->isConst && !declarator.destructured();
  if (isConstFunction) {
    return ConstFunctionDeclarator{.function = functionExpr};
  }

  return ValueDeclarator{};
}

FuncSymbol *VarChecker::aliasedGeneric(Expr *init) {
  auto *name = dyn_cast_or_null<NameExpr>(init);
  if (name == nullptr || !name->typeArgs.empty()) {
    return nullptr;
  }

  return this->genericChecker->genericNamed(name);
}

void VarChecker::declareGenericAlias(const DeclaratorPlace &place, FuncSymbol *generic) {
  VarDeclarator &declarator = *place.declarator;
  auto added = this->scopes->declare(declarator.name, {FunctionEntry{.func = generic}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(declarator.loc, added.error().message);
  }
  bool isShowingPrevious =
      added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME;
  if (isShowingPrevious) {
    auto previousEntry = this->scopes->lookup(declarator.name);
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }
  declarator.alias = true;
}

void VarChecker::liftGenericFunction(const DeclaratorPlace &place, FunctionDecl *functionDecl) {
  functionDecl->place = FunctionPlace::LIFTED;
  functionDecl->name = place.declarator->name;
  auto *module = this->state.getModuleStates().getCurrentModule();
  if (module != nullptr) {
    this->state.getModuleStates().rememberDeclModule(functionDecl, module);
  }
  this->functionChecker->declareFunction(functionDecl);
  this->callCycles->findCycles({functionDecl});
  this->state.getReturnInference().addLiftedFunction(functionDecl);
  place.declarator->alias = true;
}

void VarChecker::declareConstFunction(const DeclaratorPlace &place, FunctionExpr *functionExpr) {
  VarDeclarator &declarator = *place.declarator;
  const Type *declared = place.declared;
  this->nameFunctionValue(declarator);
  auto *variable = this->scopes->declareConst(declarator.name, declarator.loc, nullptr);
  variable->holds = Lambda{.lambda = functionExpr->fn};
  declarator.sym = variable;
  declarator.init = this->checkConstFunction(functionExpr, declared);
  if (declared != nullptr) {
    declarator.init = this->typeRelations->coerce(declarator.init, declared);
  }
  auto *adapted = dyn_cast<CastExpr>(declarator.init);
  if (adapted != nullptr && adapted->adaptsResult) {
    variable->holds = OrdinaryValue{};
  }
  variable->type = declared != nullptr ? declared : declarator.init->type;
}

Expr *VarChecker::checkConstFunction(FunctionExpr *functionExpr, const Type *declared) {
  bool isFunctionDeclared = declared != nullptr && declared->nonOptional()->isFunction();
  if (!isFunctionDeclared) {
    return this->closureChecker->checkFunctionExpr(functionExpr, nullptr);
  }
  const Type *functionType = declared->nonOptional();
  auto signature = this->closureChecker->signatureOf(functionType);

  return this->closureChecker->checkFunctionExpr(functionExpr, &signature);
}

void VarChecker::nameFunctionValue(VarDeclarator &declarator) const {
  auto *functionExpr = dyn_cast_or_null<FunctionExpr>(declarator.init);
  if (functionExpr != nullptr && functionExpr->fn->name.empty()) {
    functionExpr->fn->name = declarator.name;
  }
}

void VarChecker::walkValueDeclarator(const DeclaratorPlace &place) {
  VarDeclarator &declarator = *place.declarator;
  this->nameFunctionValue(declarator);
  auto variable = VariableValue{
      .type = place.declared,
      .hint = place.declared,
  };
  this->findDestination(place, variable);
  this->declareTimerEarly(place, variable);
  this->walkVariableValue(place, variable);
  this->walkDestructuring(place, variable);
  this->declareVariable(place, variable);
  this->regexChecker->noteRegexVariable(declarator.sym, variable.value, place.decl->isConst,
                                        place.block, place.blockIndex);
  if (place.decl->isConst && !declarator.destructured()) {
    this->rememberReflection(declarator, variable.value);
    this->markUnsharedArray(declarator, variable);
  }
  this->rememberConstLiteral(place, variable);
  if (!variable.places.empty()) {
    this->state.getTypeContext().rememberDestinations(declarator.sym, std::move(variable.places));
  }
  this->rememberResultVariable(place, variable);
  if (variable.untyped) {
    bool hasInit = declarator.init != nullptr;
    this->noteUntyped(declarator.sym, hasInit);
  }
  if (variable.value != nullptr) {
    this->flowChecker->assigned(declarator.sym, variable.value);
  }
  this->rememberFixedLength(place, variable);
  this->rememberConstNumber(place, variable);
  this->rememberConstText(place, variable);
}

void VarChecker::findDestination(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  bool isLiteralShaped = place.declared == nullptr && declarator.init != nullptr &&
                         !declarator.destructured() &&
                         this->destination->isLiteralShaped(declarator.init);
  if (!isLiteralShaped) {
    return;
  }
  auto following = this->followingInBlock(place);
  this->state.getBlockPosition().rememberFollowing(std::move(following));
  llvm::ArrayRef<VarDeclarator> declarators = place.decl->decls;
  auto later = declarators.drop_front(place.number + 1);
  variable.hint = this->destination->literalDestination(declarator, later);
  variable.places = this->state.getTypeContext().takeDestinationConflict();
}

std::vector<const Stmt *> VarChecker::followingInBlock(const DeclaratorPlace &place) const {
  if (place.block.empty()) {
    return {};
  }

  return this->followingStatements(place.block, place.blockIndex);
}

void VarChecker::declareTimerEarly(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  bool isPlainInit = declarator.init != nullptr && !declarator.destructured();
  const Type *timer = isPlainInit ? this->timerChecker->timerCallType(declarator.init) : nullptr;
  if (timer == nullptr) {
    return;
  }
  const Type *type = place.declared != nullptr ? place.declared : timer;
  variable.early =
      this->scopes->declareVar(declarator.name, declarator.loc, type, place.decl->isConst);
  LocalVarState *localState = variable.early->local();
  if (localState != nullptr) {
    localState->declSeq = kNoSequence;
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
  VarDeclarator &declarator = *place.declarator;
  auto &marks = this->state.getExpressionMarks();
  bool isArrayPattern = declarator.destructured() && !declarator.objectPattern();
  bool isMarkingInit = variable.early == nullptr;
  if (isMarkingInit) {
    marks.enterVariableInit(declarator.name);
  }
  if (isArrayPattern) {
    this->iteratorMethods->markFused(declarator.init);
  }
  declarator.init = this->exprChecker->checkExpr(declarator.init, variable.hint);
  if (isArrayPattern) {
    declarator.init = this->dictChecker->iterableOf(declarator.init);
  }
  variable.value = declarator.init;
  if (isMarkingInit) {
    marks.leaveVariableInit();
  }
  if (place.declared != nullptr) {
    declarator.init = this->typeRelations->coerce(declarator.init, place.declared);
  } else {
    this->typeFromInit(place, variable);
  }
}

void VarChecker::typeFromInit(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  variable.type = declarator.init->type;
  auto typed = this->checkInitHasType(place.decl, declarator, variable.type);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(declarator.init->loc(), typed.error().message);
    variable.type = Type::error();
  } else if (variable.type->isUndefined()) {
    variable.untyped = true;
    variable.type = Type::error();
  }
}

void VarChecker::typeConstWithoutValue(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  auto valued = this->checkConstValue(place.decl, declarator);
  if (valued.isErr()) {
    this->state.getDiagnostics().error(declarator.loc, valued.error().message);
  }
  if (variable.type == nullptr) {
    variable.type = Type::error();
  }
}

void VarChecker::walkDestructuring(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  if (declarator.destructured() && declarator.objectPattern()) {
    return;
  }
  bool isUndeclaredPattern = declarator.destructured() && place.declared == nullptr;
  bool isText = variable.type->isString() || variable.type->isTypedArray();
  if (isUndeclaredPattern && isText) {
    return this->spreadTextIntoArray(place, variable);
  }
  if (isUndeclaredPattern && variable.type->isIterator()) {
    return this->destructureIterator(place, variable);
  }

  return this->reportPatternShape(place, variable);
}

void VarChecker::spreadTextIntoArray(const DeclaratorPlace &place, VariableValue &variable) {
  VarDeclarator &declarator = *place.declarator;
  const Type *element = variable.type->isString() ? Type::string() : variable.type->elementType();
  auto *characters = this->state.getAst().make<ArrayLitExpr>(declarator.init->loc());
  characters->elements.push_back(ArrayLitExpr::Element{.expr = declarator.init, .spread = true});
  characters->type = Type::array(element);
  declarator.init = characters;
  variable.value = characters;
  variable.type = characters->type;
}

void VarChecker::destructureIterator(const DeclaratorPlace &place, VariableValue &variable) {
  VarDeclarator &declarator = *place.declarator;
  declarator.init = this->generatorChecker->destructuredIterator(declarator.init, declarator);
  variable.value = declarator.init;
  variable.type = declarator.init->type;
}

void VarChecker::reportPatternShape(const DeclaratorPlace &place, VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  auto shaped = this->checkDestructurable(declarator, variable.type);
  if (shaped.isErr()) {
    llvm::SMLoc at = declarator.init != nullptr ? declarator.init->loc() : declarator.loc;
    this->state.getDiagnostics().error(at, shaped.error().message);
    variable.type = Type::error();
  }
}

void VarChecker::declareVariable(const DeclaratorPlace &place, const VariableValue &variable) {
  VarDeclarator &declarator = *place.declarator;
  if (variable.early == nullptr) {
    declarator.sym = this->scopes->declareVar(declarator.name, declarator.loc, variable.type,
                                              place.decl->isConst);
    return;
  }
  LocalVarState *localState = variable.early->local();
  if (localState != nullptr) {
    localState->declSeq = this->state.getReturnInference().takeSequenceNumber();
  }
  declarator.sym = variable.early;
}

void VarChecker::rememberReflection(VarDeclarator &declarator, const Expr *value) {
  auto *call = dyn_cast_or_null<CallExpr>(value);
  if (call == nullptr) {
    return;
  }
  bool isSingleMethod = call->builtin() == Builtin::ReflectMethod &&
                        !this->state.getExpressionMarks().hasSeveralVariants(call);
  if (isSingleMethod) {
    declarator.sym->holds = ReflectedMethod{.method = call->method()->decl};
  }
  bool isReadingByKey = call->builtin() == Builtin::ReflectGet && call->args.size() == 2;
  if (isReadingByKey) {
    this->rememberReflectedNames(declarator, call->args[1]);
  }
}

void VarChecker::rememberReflectedNames(VarDeclarator &declarator, const Expr *key) {
  auto names = this->reflectedNames(key);
  if (!names.empty()) {
    declarator.sym->holds = ReflectedNames{.names = std::move(names)};
  }
}

std::vector<llvm::StringRef> VarChecker::reflectedNames(const Expr *key) {
  const auto *bare = ExprParts::withoutImplicitCasts(key);
  auto texts = this->literalKeyTexts(bare);
  auto names = std::vector<llvm::StringRef>();
  for (const std::u16string &text : texts) {
    auto utf8 = ToUtf8::toUtf8(text);
    names.push_back(this->state.getAst().keepName(utf8));
  }

  return names;
}

std::vector<std::u16string> VarChecker::literalKeyTexts(const Expr *key) const {
  auto *literal = dyn_cast<StringLitExpr>(key);
  if (literal != nullptr) {
    return {literal->value};
  }
  if (key->type == nullptr) {
    return {};
  }
  const Type *keyType = key->type->nonOptional();
  auto texts = std::vector<std::u16string>();
  for (const Type *member : this->keyMembers(keyType)) {
    bool isLiteralText = member->isLiteral() && member->literalBase()->isString();
    if (!isLiteralText) {
      return {};
    }
    texts.push_back(member->literalText());
  }

  return texts;
}

std::vector<const Type *> VarChecker::keyMembers(const Type *keyType) const {
  if (!keyType->isUnion()) {
    return {keyType};
  }
  auto members = keyType->members();

  return std::vector<const Type *>(members.begin(), members.end());
}

void VarChecker::markUnsharedArray(const VarDeclarator &declarator, const VariableValue &variable) {
  LocalVarState *localState = declarator.sym->local();
  if (localState == nullptr) {
    return;
  }
  bool isFresh = this->isFreshArray(variable.value);
  localState->unshared =
      isFresh && variable.type->isArray() &&
      this->flowChecker->isStayingLocal(declarator.sym->owner(), declarator.name);
}

bool VarChecker::isFreshArray(const Expr *value) const {
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
  VarDeclarator &declarator = *place.declarator;
  const Expr *value = variable.value;
  auto &constants = this->state.getConstantStore();
  bool isLiteral = place.decl->isConst && value != nullptr && this->isLiteralValue(value);
  if (isLiteral) {
    constants.rememberConstLiteral(declarator.sym, value);
    declarator.sym->literalConst = place.declared == nullptr && variable.type->isString();
  }
  auto *alias = dyn_cast_or_null<NameExpr>(value);
  bool isLiteralAlias = alias != nullptr && alias->var() != nullptr && alias->var()->literalConst;
  bool isTextAlias = place.decl->isConst && isLiteralAlias && place.declared == nullptr &&
                     variable.type->isString();
  if (isTextAlias) {
    const auto *aliased = constants.findConstLiteral(alias->var());
    constants.rememberConstLiteral(declarator.sym, aliased);
    declarator.sym->literalConst = true;
  }
}

bool VarChecker::isLiteralValue(const Expr *value) const {
  bool isTextChoice = isa<ConditionalExpr>(value) && this->isTextChoice(value);

  return isa<ObjectLitExpr>(value) || isa<StringLitExpr>(value) || isTextChoice;
}

void VarChecker::rememberResultVariable(const DeclaratorPlace &place,
                                        const VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  const Type *held = variable.type->nonOptional();
  bool isHoldingResult = held->isResult() || (held->isPromise() && held->promised()->isResult());
  bool isTracked = !declarator.destructured() &&
                   !this->state.getGenericStore().isCheckingInstance() && isHoldingResult &&
                   !this->isExported(declarator.name);
  if (isTracked) {
    this->state.getExpressionMarks().addResultVar(declarator.sym);
  }
}

void VarChecker::rememberFixedLength(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  const Type *base = variable.type->nonOptional();
  bool hasBytes = base->isByteView() || base->bytes() == Type::Bytes::ArrayBuffer;
  bool isFixed =
      place.decl->isConst && variable.value != nullptr && !declarator.destructured() && hasBytes;
  if (!isFixed) {
    return;
  }
  auto length = this->indexProofs->knownLength(variable.value);
  bool isConstantLength = length.has_value() && length->term.isZero();
  if (isConstantLength) {
    this->state.getFlowState().rememberFixedLength(declarator.sym, length->offset);
  }
}

void VarChecker::rememberConstNumber(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  bool isInteger = place.decl->isConst && declarator.init != nullptr && variable.type->isInteger();
  if (!isInteger) {
    return;
  }
  this->constantFolder->noteUnsignedVar(declarator.sym, declarator.init);
  auto constant = this->constantFolder->intConstant(declarator.init);
  if (!constant.has_value()) {
    return;
  }
  this->state.getConstantStore().rememberConstVar(declarator.sym, *constant);
  declarator.sym->literalConst = place.declared == nullptr && variable.type->isInt() &&
                                 this->typeRelations->isIntLiteralTree(declarator.init);
}

void VarChecker::rememberConstText(const DeclaratorPlace &place, const VariableValue &variable) {
  const VarDeclarator &declarator = *place.declarator;
  bool isText = place.decl->isConst && declarator.init != nullptr && variable.type->isString();
  if (!isText) {
    return;
  }
  auto key = this->keyedFieldChecker->constantKey(declarator.init);
  if (key.has_value()) {
    this->state.getConstantStore().rememberConstKey(declarator.sym, std::move(*key));
  }
  auto prefix = this->tzModulesChecker->knownPrefix(declarator.init);
  if (!prefix.empty()) {
    this->state.getConstantStore().rememberConstPrefix(declarator.sym, std::move(prefix));
  }
}

VarChecker::ResultInitHasType VarChecker::checkInitHasType(const VarDeclStmt *varDecl,
                                                           const VarDeclarator &declarator,
                                                           const Type *type) const {
  if (type->isVoid()) {
    auto message = ErrorTextVarInit::isVoid();

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::VOID_INITIALIZER,
        .message = message,
    });
  }
  if (type->isUndefined() && (varDecl->isConst || declarator.destructured())) {
    auto message = ErrorTextVarInit::undefined(varDecl->isConst, declarator.name);

    return err(VarCheckerInitHasTypeError{
        .code = VarCheckerInitHasTypeErrorCode::UNDEFINED_INITIALIZER,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultConstValued VarChecker::checkConstValue(const VarDeclStmt *varDecl,
                                                          const VarDeclarator &declarator) const {
  if (varDecl->isConst && declarator.init == nullptr) {
    auto message = ErrorTextConstValue::missing();

    return err(VarCheckerConstValueError{
        .code = VarCheckerConstValueErrorCode::CONST_WITHOUT_VALUE,
        .message = message,
    });
  }

  return ok(&declarator);
}

VarChecker::ResultDestructure VarChecker::checkDestructurable(const VarDeclarator &declarator,
                                                              const Type *type) const {
  bool isFitting = !declarator.destructured() || type->isArray() || type->isTuple() ||
                   type->isMatch() || type->isError() || type->nonOptional()->isJson();
  if (!isFitting && type->isOptional() && type->inner()->isArray()) {
    auto message = ErrorTextVarShape::mayBeUndef(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::ARRAY_MAY_BE_UNDEFINED,
        .message = message,
    });
  }
  if (!isFitting) {
    auto message = ErrorTextVarShape::notArray(type->name());

    return err(VarCheckerDestructurableError{
        .code = VarCheckerDestructurableErrorCode::NOT_AN_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

VarChecker::ResultResultCheck VarChecker::resultChecked(const VarSymbol *variable) const {
  const Type *held = variable->type->nonOptional();
  if (held->isPromise()) {
    auto message = ErrorTextResultUse::promise(variable->name);

    return err(VarCheckerResultCheckedError{
        .code = VarCheckerResultCheckedErrorCode::PROMISE_NEVER_AWAITED,
        .message = message,
    });
  }

  auto message = ErrorTextResultUse::unchecked(variable->name);

  return err(VarCheckerResultCheckedError{
      .code = VarCheckerResultCheckedErrorCode::RESULT_NEVER_CHECKED,
      .message = message,
  });
}

VarChecker::ResultVarHasTypes VarChecker::varTyped(const Untyped &untyped, const Type *type) const {
  llvm::StringRef name = untyped.var->name;
  auto hint = this->typeHint(untyped, type);
  if (untyped.initUndefined) {
    auto message = ErrorTextVarTyped::undefined(hint);

    return err(VarCheckerVarTypedError{
        .code = VarCheckerVarTypedErrorCode::UNDEFINED_ONLY,
        .message = message,
    });
  }

  auto message = ErrorTextVarTyped::needsType(name, hint);

  return err(VarCheckerVarTypedError{
      .code = VarCheckerVarTypedErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

std::string VarChecker::typeHint(const Untyped &untyped, const Type *type) const {
  llvm::StringRef name = untyped.var->name;
  bool isKnown = type != nullptr && !type->isError() && !type->isUndefined();
  if (isKnown) {
    return ErrorTextVarTyped::writeType(name, type->name());
  }
  if (untyped.initUndefined) {
    return ErrorTextVarTyped::addOne(name);
  }

  return ErrorTextVarTyped::example(name);
}

bool VarChecker::isExported(llvm::StringRef name) const {
  if (!this->state.getScopeStack().isTopLevel()) {
    return false;
  }
  const auto *module = this->state.getModuleStates().getCurrentModule();
  if (module == nullptr) {
    return false;
  }
  for (const ExportDecl &exportDecl : module->file.exports) {
    if (this->isExportingName(exportDecl, name)) {
      return true;
    }
  }

  return false;
}

bool VarChecker::isExportingName(const ExportDecl &exportDecl, llvm::StringRef name) const {
  const LocalExport *local = exportDecl.form.as<LocalExport>();
  if (local == nullptr) {
    return false;
  }
  for (const ModuleName &exported : local->names) {
    if (exported.name == name) {
      return true;
    }
  }

  return false;
}

void VarChecker::reportUncheckedResults() {
  auto readAt = llvm::DenseMap<const char *, bool>();
  auto resultVars = this->state.getExpressionMarks().getResultVars();
  for (const VarSymbol *variable : resultVars) {
    readAt[variable->loc.getPointer()] |= variable->read;
  }
  for (const VarSymbol *variable : resultVars) {
    bool &isDone = readAt[variable->loc.getPointer()];
    if (!isDone) {
      isDone = true;
      this->reportUncheckedResult(variable);
    }
  }
  this->state.getExpressionMarks().forgetResultVars();
}

void VarChecker::reportUncheckedResult(const VarSymbol *variable) {
  auto checked = this->resultChecked(variable);
  if (checked.isErr()) {
    this->state.getDiagnostics().error(variable->loc, checked.error().message);
  }
}

void VarChecker::noteUntyped(VarSymbol *variable, bool initUndefined) {
  this->state.getExpressionMarks().addUntyped(variable, initUndefined);
}

void VarChecker::assignedUntyped(const VarSymbol *variable, const Type *type) {
  if (!type->isError() && !type->isVoid()) {
    this->state.getExpressionMarks().addUntypedAssignment(variable, type);
  }
}

void VarChecker::reportUntyped() {
  auto &marks = this->state.getExpressionMarks();
  for (const Untyped &untyped : marks.getUntyped()) {
    if (this->isUntypedHere(untyped)) {
      this->reportUntypedVariable(untyped);
      marks.forgetUntyped(untyped.var);
    }
  }
}

bool VarChecker::isUntypedHere(const Untyped &untyped) const {
  auto found = this->state.getScopeStack().findInCurrentScope(untyped.var->name);

  return found.has_value() && found->var() == untyped.var;
}

void VarChecker::reportUntypedVariable(const Untyped &untyped) {
  const auto *type = this->assignedType(untyped);
  auto typed = this->varTyped(untyped, type);
  if (typed.isErr()) {
    this->state.getDiagnostics().error(untyped.var->loc, typed.error().message);
  }
}

const Type *VarChecker::assignedType(const Untyped &untyped) const {
  const Type *type = untyped.initUndefined ? Type::undefinedTy() : nullptr;
  for (const Type *candidate : untyped.assigned) {
    const auto *common = this->commonAssigned(type, candidate);
    type = common != nullptr ? common : Type::error();
  }

  return type;
}

const Type *VarChecker::commonAssigned(const Type *type, const Type *candidate) const {
  if (type == nullptr) {
    return candidate;
  }
  const auto *common = this->typeRelations->unify(type, candidate);
  if (common != nullptr) {
    return common;
  }
  bool isNumbers = type->nonOptional()->isNumeric() && candidate->isNumeric();
  if (!isNumbers) {
    return nullptr;
  }
  if (type->nonOptional()->isFloat()) {
    return type;
  }

  return candidate->isFloat() ? candidate : nullptr;
}

}
