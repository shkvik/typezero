#include "compiler/semantic/declaration/functions/declaration_functions_registry.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/iterators/generator_checker/generator_checker.h"
#include "compiler/semantic/bodies/iterators/iterable_checker/iterable_checker.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "compiler/semantic/generics/generic_class_checker/generic_class_checker.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"
#include "compiler/semantic/library/stream/chunk_types/chunk_types.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include <cmath>
#include <functional>

using llvm::dyn_cast;

namespace tz {

DeclarationFunctionsRegistry::DeclarationFunctionsRegistry(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->chunkTypes = &all.chunkTypes;
  this->typeParams = &all.typeParams;
  this->asyncChecker = &all.asyncChecker;
  this->typeResolver = &all.typeResolver;
  this->iterableChecker = &all.iterableChecker;
  this->generatorChecker = &all.generatorChecker;
  this->genericClassChecker = &all.genericClassChecker;
}

DeclarationFunctionsRegistry::ResultTopLevelGenerics
DeclarationFunctionsRegistry::checkTopLevelGeneric(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    auto message = ErrorTextFunctionDecl::nested();

    return err(DeclarationFunctionsRegistryTopLevelGenericError{
        .code = DeclarationFunctionsRegistryTopLevelGenericErrorCode::NESTED_GENERIC,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationFunctionsRegistry::ResultDecoratorAllowed
DeclarationFunctionsRegistry::checkParamDecorators(const FunctionDecl *functionDecl,
                                                   const Param &param) const {
  bool isClassMethod = functionDecl->cls() != nullptr && functionDecl->cls()->literal() == nullptr;
  if (!param.decorators.empty() && !isClassMethod && !functionDecl->isInstance) {
    auto message = ErrorTextFunctionDecl::decorator();

    return err(DeclarationFunctionsRegistryParamDecoratorsError{
        .code = DeclarationFunctionsRegistryParamDecoratorsErrorCode::DECORATOR_NOT_ALLOWED,
        .message = message,
    });
  }

  return ok(&param);
}

DeclarationFunctionsRegistry::ResultThisParamAllowed
DeclarationFunctionsRegistry::checkThisParam(const FunctionDecl *functionDecl) const {
  if (functionDecl->isArrow()) {
    auto message = ErrorTextFunctionDecl::arrowThis();

    return err(DeclarationFunctionsRegistryThisParamError{
        .code = DeclarationFunctionsRegistryThisParamErrorCode::ARROW_THIS,
        .message = message,
    });
  }
  if (functionDecl->cls() != nullptr) {
    auto message = ErrorTextFunctionDecl::methodThis();

    return err(DeclarationFunctionsRegistryThisParamError{
        .code = DeclarationFunctionsRegistryThisParamErrorCode::METHOD_THIS,
        .message = message,
    });
  }

  return ok(functionDecl);
}

DeclarationFunctionsRegistry::ResultRestParamIsArray
DeclarationFunctionsRegistry::checkRestType(const Type *type, const Param &param) const {
  bool isList = type->isArray() || type->isListParam() || type->isTuple() || type->isError();
  if (!isList) {
    auto message = ErrorTextFunctionDecl::restArray(param.name);

    return err(DeclarationFunctionsRegistryRestTypeError{
        .code = DeclarationFunctionsRegistryRestTypeErrorCode::REST_NOT_ARRAY,
        .message = message,
    });
  }

  return ok(type);
}

DeclarationFunctionsRegistry::ResultParamOrderIsKept
DeclarationFunctionsRegistry::checkRequiredOrder(const Param &param, bool isAfterOptional) const {
  if (isAfterOptional) {
    auto message = ErrorTextFunctionDecl::required();

    return err(DeclarationFunctionsRegistryRequiredOrderError{
        .code = DeclarationFunctionsRegistryRequiredOrderErrorCode::REQUIRED_AFTER_OPTIONAL,
        .message = message,
    });
  }

  return ok(&param);
}

DeclarationFunctionsRegistry::ResultParamTypeIsKnown
DeclarationFunctionsRegistry::checkParamTypeKnown(const Param &param) const {
  const Expr *value = param.defaultValue();
  bool isLiteral = value != nullptr && SharedHelpers::literalType(value) != nullptr;
  if (param.type || isLiteral || param.property) {
    return ok(&param);
  }
  if (value != nullptr) {
    auto message = ErrorTextFunctionDecl::defaultNeedsType(param.label());

    return err(DeclarationFunctionsRegistryParamTypeKnownError{
        .code = DeclarationFunctionsRegistryParamTypeKnownErrorCode::DEFAULT_NOT_LITERAL,
        .message = message,
    });
  }
  auto message = ErrorTextFunctionDecl::needsType(param.label());

  return err(DeclarationFunctionsRegistryParamTypeKnownError{
      .code = DeclarationFunctionsRegistryParamTypeKnownErrorCode::NEEDS_TYPE,
      .message = message,
  });
}

void DeclarationFunctionsRegistry::declareFunction(FunctionDecl *functionDecl,
                                                   VarSymbol *variable) {
  this->declareSymbol(functionDecl, variable);
  if (!functionDecl->name.empty()) {
    this->declareFunctionName(functionDecl, functionDecl->sym, variable);
  }
}

void DeclarationFunctionsRegistry::declareSignature(FunctionDecl *functionDecl) {
  this->declareSymbol(functionDecl, nullptr);
}

void DeclarationFunctionsRegistry::declareSymbol(FunctionDecl *functionDecl, VarSymbol *variable) {
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  bool isGeneric = this->isGenericDeclaration(functionDecl);
  bool isClassParams = functionDecl->cls() != nullptr && !functionDecl->isStatic() &&
                       !functionDecl->cls()->typeParams.empty();
  if (isGeneric || isClassParams) {
    this->state.walk.scopes.openTypeScope();
  }
  if (isClassParams) {
    this->genericClassChecker->bindClassTypeParams(functionDecl->cls());
  }
  if (isGeneric) {
    this->declareFunctionTypeParams(functionDecl, function, variable);
  }
  std::optional<std::vector<const Type *>> hook =
      functionDecl->cls() != nullptr ? this->chunkTypes->streamHookParams(functionDecl)
                                     : std::nullopt;
  this->padHookParams(functionDecl, hook);
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    const Type *hooked = hook.has_value() && index < hook->size() ? (*hook)[index] : nullptr;
    this->declareFunctionParam(functionDecl, function, index, hooked);
  }
  this->declareFunctionReturn(functionDecl, function);
  if (isGeneric || isClassParams) {
    this->state.walk.scopes.closeTypeScope();
  }
  this->expandArgumentList(functionDecl, function);
  functionDecl->sym = function;
}

bool DeclarationFunctionsRegistry::isGenericDeclaration(const FunctionDecl *functionDecl) {
  if (!functionDecl->typeParams.empty()) {
    return true;
  }
  for (const Param &param : functionDecl->params) {
    if (param.type.has_value() && this->iterableChecker->isIterableRef(*param.type)) {
      return true;
    }
  }

  return false;
}

void DeclarationFunctionsRegistry::declareFunctionTypeParams(FunctionDecl *functionDecl,
                                                             FuncSymbol *function,
                                                             VarSymbol *variable) {
  if (variable == nullptr) {
    function->typeParams =
        this->typeParams->declareTypeParams(functionDecl->typeParams, &function->typeDefaults);
    functionDecl->typeArgs = function->typeParams;
    return;
  }
  auto topLevel = this->checkTopLevelGeneric(functionDecl);
  if (topLevel.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), topLevel.error().message);
  }
  functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
  this->typeParams->bindTypeParams(functionDecl->typeParams, functionDecl->typeArgs);
}

void DeclarationFunctionsRegistry::padHookParams(
    FunctionDecl *functionDecl, const std::optional<std::vector<const Type *>> &hook) {
  if (!hook.has_value() || this->hasRestParam(functionDecl)) {
    return;
  }
  for (size_t index = functionDecl->params.size(); index < hook->size(); ++index) {
    auto hidden = Param{
        .name = this->state.ast.hiddenName(),
        .loc = functionDecl->loc(),
    };
    functionDecl->params.push_back(hidden);
  }
}

bool DeclarationFunctionsRegistry::hasRestParam(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    if (param.rest()) {
      return true;
    }
  }

  return false;
}

void DeclarationFunctionsRegistry::declareFunctionParam(FunctionDecl *functionDecl,
                                                        FuncSymbol *function, size_t index,
                                                        const Type *hooked) {
  Param &param = functionDecl->params[index];
  auto decorated = this->checkParamDecorators(functionDecl, param);
  if (decorated.isErr()) {
    this->state.diagnostics.error(param.decorators[0].loc, decorated.error().message);
  }
  const auto *declared = this->declaredParamType(functionDecl, function, param, hooked);
  const Type *type = param.optional() ? Type::optional(declared) : declared;
  param.declaredType = type;
  if (index == 0) {
    this->walkThisParam(functionDecl, function, param);
  }
  if (param.rest()) {
    return this->declareRestParam(function, param, type);
  }
  bool isOmittable = param.optional() || param.defaultValue() != nullptr;
  const Type *passed = isOmittable ? Type::optional(type) : type;
  function->params.push_back(passed);
  if (isOmittable) {
    return;
  }
  bool isAfterOptional = this->hasOptionalBefore(functionDecl, index);
  auto order = this->checkRequiredOrder(param, isAfterOptional);
  if (order.isErr()) {
    this->state.diagnostics.error(param.loc, order.error().message);
  }
  function->minArgs = index + 1;
}

void DeclarationFunctionsRegistry::declareRestParam(FuncSymbol *function, Param &param,
                                                    const Type *type) {
  auto rest = this->checkRestType(type, param);
  if (rest.isErr()) {
    this->state.diagnostics.error(param.loc, rest.error().message);
    param.declaredType = Type::error();
  }
  function->hasRest = true;
  function->params.push_back(param.declaredType);
}

bool DeclarationFunctionsRegistry::hasOptionalBefore(const FunctionDecl *functionDecl,
                                                     size_t index) const {
  for (size_t earlier = 0; earlier < index; ++earlier) {
    const Param &param = functionDecl->params[earlier];
    if (!param.rest() && param.optional()) {
      return true;
    }
  }

  return false;
}

const Type *DeclarationFunctionsRegistry::declaredParamType(FunctionDecl *functionDecl,
                                                            FuncSymbol *function,
                                                            const Param &param,
                                                            const Type *hooked) {
  if (param.type.has_value()) {
    return this->writtenParamType(functionDecl, function, param, hooked);
  }
  if (hooked != nullptr && !param.rest()) {
    return hooked;
  }
  const Type *literal =
      param.defaultValue() != nullptr ? SharedHelpers::literalType(param.defaultValue()) : nullptr;
  if (literal != nullptr) {
    return literal;
  }
  auto known = this->checkParamTypeKnown(param);
  if (known.isErr()) {
    this->state.diagnostics.error(param.loc, known.error().message);
  }

  return Type::error();
}

const Type *DeclarationFunctionsRegistry::writtenParamType(FunctionDecl *functionDecl,
                                                           FuncSymbol *function, const Param &param,
                                                           const Type *hooked) {
  const auto *type = this->resolvedParamType(functionDecl, function, param);
  bool isIntHook = hooked != nullptr && type != nullptr && type->isFloat() && hooked->isInt();

  return isIntHook ? hooked : type;
}

const Type *DeclarationFunctionsRegistry::resolvedParamType(FunctionDecl *functionDecl,
                                                            FuncSymbol *function,
                                                            const Param &param) {
  auto iterable = this->iterableChecker->checkIterableParam(functionDecl, function, *param.type);
  if (iterable.isOk()) {
    return iterable.value();
  }
  if (iterable.error().code != IterableCheckerIterableParamErrorCode::NOT_ITERABLE) {
    this->state.diagnostics.error(param.type->loc, iterable.error().message);
    return Type::error();
  }
  this->state.types.context.enterListContext(param.rest());
  const auto *type = this->typeResolver->resolveType(*param.type);
  this->state.types.context.leaveListContext();

  return type;
}

void DeclarationFunctionsRegistry::walkThisParam(const FunctionDecl *functionDecl,
                                                 FuncSymbol *function, const Param &param) {
  if (param.name != KeywordTable::getName(Keyword::THIS) || param.rest() ||
      !param.type.has_value()) {
    return;
  }
  auto thisAllowed = this->checkThisParam(functionDecl);
  if (thisAllowed.isErr()) {
    this->state.diagnostics.error(param.loc, thisAllowed.error().message);
    return;
  }
  function->hasThis = !param.optional() && param.defaultValue() == nullptr;
}

void DeclarationFunctionsRegistry::declareFunctionReturn(FunctionDecl *functionDecl,
                                                         FuncSymbol *function) {
  if (functionDecl->returnType.has_value()) {
    return this->declareWrittenReturn(functionDecl, function);
  }
  if (functionDecl->isGenerator || this->isReturningValue(functionDecl->body)) {
    return;
  }
  const Type *nothing = functionDecl->isAsync ? Type::promise(Type::voidTy()) : Type::voidTy();
  function->setReturnType(nothing);
}

void DeclarationFunctionsRegistry::declareWrittenReturn(FunctionDecl *functionDecl,
                                                        FuncSymbol *function) {
  const auto *declared = this->resolveReturnType(functionDecl, function);
  auto generator = this->generatorChecker->checkGeneratorReturnType(functionDecl, declared);
  if (generator.isErr()) {
    this->state.diagnostics.error(functionDecl->returnType->loc, generator.error().message);
  }
  const Type *returned = generator.isOk() ? generator.value() : Type::error();
  const auto *returnType = this->asyncChecker->asyncReturnType(functionDecl, returned);
  function->setReturnType(returnType);
}

void DeclarationFunctionsRegistry::declareFunctionName(FunctionDecl *functionDecl,
                                                       FuncSymbol *function, VarSymbol *variable) {
  auto added = this->state.walk.scopes.declare(functionDecl->name,
                                               {FunctionEntry{.func = function, .var = variable}});
  if (added.isOk()) {
    return;
  }
  this->state.diagnostics.error(functionDecl->loc(), added.error().message);
  if (added.error().code == ScopeStackContextDeclareErrorCode::UNDEFINED_AS_NAME) {
    return;
  }
  auto previousEntry = this->state.walk.scopes.findName(functionDecl->name);
  auto declaredHere = ErrorTextDeclareName::previous();
  this->state.diagnostics.note(previousEntry.loc(), declaredHere);
}

DeclarationFunctionsRegistry::ResultNotAsserting
DeclarationFunctionsRegistry::notAsserting(const FunctionDecl *functionDecl, const TypeRef &ref) {
  if (!ref.asserts()) {
    return ok(&ref);
  }

  const auto *type = this->predicateType(ref);
  auto call = this->callText(functionDecl);
  auto shown = this->shownPredicate(ref, type);
  auto message = ErrorTextAsserts::notAllowed(shown, type != nullptr, call);

  return err(DeclarationFunctionsRegistryNotAssertingError{
      .code = DeclarationFunctionsRegistryNotAssertingErrorCode::ASSERTS_PREDICATE,
      .message = message,
  });
}

const Type *DeclarationFunctionsRegistry::predicateType(const TypeRef &ref) const {
  if (ref.args.empty()) {
    return nullptr;
  }

  return this->typeResolver->resolveType(ref.args[0]);
}

std::string DeclarationFunctionsRegistry::callText(const FunctionDecl *functionDecl) const {
  auto names = std::vector<llvm::StringRef>();
  for (const Param &param : functionDecl->params) {
    names.push_back(param.name);
  }
  auto args = llvm::join(names, ", ");
  auto call = functionDecl->name.str() + "(" + args + ")";
  if (!functionDecl->takesThis()) {
    return call;
  }
  auto object = this->receiverText(functionDecl);

  return object + "." + call;
}

std::string DeclarationFunctionsRegistry::receiverText(const FunctionDecl *functionDecl) const {
  auto object = functionDecl->cls()->name.str();
  object[0] = llvm::toLower(object[0]);

  return object;
}

std::string DeclarationFunctionsRegistry::shownPredicate(const TypeRef &ref,
                                                         const Type *type) const {
  if (type == nullptr) {
    return ref.name.str();
  }

  return ref.name.str() + " is " + type->name().str();
}

DeclarationFunctionsRegistry::ResultPredicateArg
DeclarationFunctionsRegistry::predicateArg(const FunctionDecl *functionDecl,
                                           const TypeRef &ref) const {
  bool isThis = ref.name == KeywordTable::getName(Keyword::THIS);
  bool isMethod =
      functionDecl->cls() != nullptr && !functionDecl->isStatic() && !functionDecl->isConstructor();
  if (isThis && !isMethod) {
    auto message = ErrorTextPredicateArg::thisOutside();

    return err(DeclarationFunctionsRegistryPredicateArgError{
        .code = DeclarationFunctionsRegistryPredicateArgErrorCode::THIS_OUTSIDE_METHOD,
        .message = message,
    });
  }
  if (isThis) {
    return ok(kThisPosition);
  }

  auto param = this->lastParamNamed(functionDecl, ref.name);
  if (!param.has_value()) {
    auto message = ErrorTextPredicateArg::notParam(ref.name);

    return err(DeclarationFunctionsRegistryPredicateArgError{
        .code = DeclarationFunctionsRegistryPredicateArgErrorCode::NOT_A_PARAMETER,
        .message = message,
    });
  }

  return ok(*param);
}

std::optional<int> DeclarationFunctionsRegistry::lastParamNamed(const FunctionDecl *functionDecl,
                                                                llvm::StringRef name) const {
  auto found = std::optional<int>();
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    if (functionDecl->params[index].name == name) {
      found = int(index);
    }
  }

  return found;
}

const Type *DeclarationFunctionsRegistry::resolveReturnType(FunctionDecl *functionDecl,
                                                            FuncSymbol *function) {
  const TypeRef &ref = *functionDecl->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return this->typeResolver->resolveTypeOrVoid(ref);
  }
  bool isReporting = function != nullptr;
  if (isReporting) {
    this->reportAsserting(functionDecl, ref);
  }
  if (ref.asserts()) {
    return Type::voidTy();
  }
  auto named = this->predicateArg(functionDecl, ref);
  if (named.isErr() && isReporting) {
    this->state.diagnostics.error(ref.loc, named.error().message);
  }
  const auto *type = this->predicateType(ref);
  bool isRecorded = isReporting && named.isOk() && type != nullptr && !type->isError();
  if (isRecorded) {
    function->predicate = TypePredicate{.param = named.value(), .type = type};
  }

  return Type::boolTy();
}

void DeclarationFunctionsRegistry::reportAsserting(const FunctionDecl *functionDecl,
                                                   const TypeRef &ref) {
  auto plain = this->notAsserting(functionDecl, ref);
  if (plain.isErr()) {
    this->state.diagnostics.error(ref.loc, plain.error().message);
  }
}

bool DeclarationFunctionsRegistry::isExpandableRest(const FunctionDecl *functionDecl,
                                                    const FuncSymbol *function) const {
  if (!function->hasRest || functionDecl->params.empty()) {
    return false;
  }

  return functionDecl->params.back().rest() && functionDecl->listName.empty();
}

void DeclarationFunctionsRegistry::expandArgumentList(FunctionDecl *functionDecl,
                                                      FuncSymbol *function) {
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

Param DeclarationFunctionsRegistry::listPartParam(const Param &rest, const Type *list,
                                                  size_t index) {
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

Param DeclarationFunctionsRegistry::listRestParam(const Param &rest, const Type *more) {
  std::string name = kHiddenPrefix.str() + rest.name.str() + kRestPartSuffix.str();
  Param param;
  param.name = this->state.ast.keepName(name);
  param.loc = rest.loc;
  param.form = RestParam{};
  param.declaredType = more;

  return param;
}

bool DeclarationFunctionsRegistry::isReturningValue(const Stmt *body) const {
  auto nodes = PlacedNodes::collect({body}, {});
  for (const PlacedStmt &placed : nodes.getStmts()) {
    auto *returnStmt = dyn_cast<ReturnStmt>(placed.stmt);
    bool isOwn = returnStmt != nullptr && !placed.place.isInsideFunction();
    if (isOwn && this->isGivingValue(returnStmt)) {
      return true;
    }
  }

  return false;
}

bool DeclarationFunctionsRegistry::isGivingValue(const ReturnStmt *returnStmt) const {
  if (returnStmt->value == nullptr) {
    return false;
  }
  auto *unary = dyn_cast<UnaryExpr>(returnStmt->value);
  bool isDiscarded = unary != nullptr && unary->op == UnaryOp::Void;

  return !isDiscarded;
}

void DeclarationFunctionsRegistry::declareItemFunction(Stmt *item, Module *module,
                                                       std::vector<FunctionDecl *> &functions) {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl == nullptr) {
    return;
  }
  this->declareFunction(functionDecl);
  this->state.declaration.modules.rememberDeclModule(functionDecl, module);
  functions.push_back(functionDecl);
}

}
