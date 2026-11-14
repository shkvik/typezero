#include "compiler/semantic/declaration/functions/declaration_functions_registry.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/flow/placed_nodes/placed_nodes.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include <cmath>
#include <functional>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr int kThisPosition = -1;
constexpr llvm::StringLiteral kHiddenPrefix = "%";
constexpr llvm::StringLiteral kRestPartSuffix = ".rest";

}

DeclarationFunctionsRegistry::DeclarationFunctionsRegistry(SemanticState &owner)
    : state(owner) {}

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

void DeclarationFunctionsRegistry::startFunctionSymbol(const StartFunctionSymbol &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  auto *function = this->state.ast.newFunc();
  function->setCompleter(this->state.completer);
  function->name = functionDecl->name;
  function->loc = functionDecl->loc();
  function->decl = functionDecl;
  bool isGeneric = this->isGenericDeclaration(functionDecl);
  bool isClassParams = functionDecl->cls() != nullptr && !functionDecl->isStatic() &&
                       !functionDecl->cls()->typeParams.empty();
  auto draft = FunctionSymbolDraft{
      .functionDecl = functionDecl,
      .function = function,
      .variable = task.variable,
      .isNamed = task.isNamed,
      .isTypeScoped = isGeneric || isClassParams,
  };
  if (draft.isTypeScoped) {
    this->state.walk.scopes.openTypeScope();
  }
  this->state.walk.tasks.pushTask(FinishFunctionSymbol{.draft = draft});
  this->state.walk.tasks.pushTask(StartFunctionReturn{.draft = draft});
  this->state.walk.tasks.pushTask(StartFunctionParams{.draft = draft});
  if (functionDecl->cls() != nullptr) {
    this->state.walk.tasks.pushTask(FindStreamHookParams{.functionDecl = functionDecl});
  }
  if (isGeneric) {
    this->pushFunctionTypeParams(draft);
  }
  if (isClassParams) {
    this->state.walk.tasks.pushTask(BindClassTypeParams{.classDecl = functionDecl->cls()});
  }
}

void DeclarationFunctionsRegistry::finishFunctionSymbol(const FinishFunctionSymbol &task) {
  const FunctionSymbolDraft &draft = task.draft;
  FunctionDecl *functionDecl = draft.functionDecl;
  if (draft.isTypeScoped) {
    this->state.walk.scopes.closeTypeScope();
  }
  this->expandArgumentList(functionDecl, draft.function);
  functionDecl->sym = draft.function;
  if (draft.isNamed && !functionDecl->name.empty()) {
    this->declareFunctionName(functionDecl, draft.function, draft.variable);
  }
}

bool DeclarationFunctionsRegistry::isGenericDeclaration(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty()) {
    return true;
  }
  for (const Param &param : functionDecl->params) {
    if (param.type.has_value() && this->state.isIterableRef(*param.type)) {
      return true;
    }
  }

  return false;
}

void DeclarationFunctionsRegistry::pushFunctionTypeParams(const FunctionSymbolDraft &draft) {
  FunctionDecl *functionDecl = draft.functionDecl;
  if (draft.variable == nullptr) {
    return this->state.walk.tasks.pushTask(
        DeclareFunctionTypeParams{.functionDecl = functionDecl, .function = draft.function});
  }
  auto topLevel = this->checkTopLevelGeneric(functionDecl);
  if (topLevel.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), topLevel.error().message);
  }
  functionDecl->typeArgs.assign(functionDecl->typeParams.size(), Type::error());
  this->state.walk.tasks.pushTask(BindFunctionTypeParams{.functionDecl = functionDecl});
}

void DeclarationFunctionsRegistry::startFunctionParams(const StartFunctionParams &task) {
  FunctionDecl *functionDecl = task.draft.functionDecl;
  this->padHookParams(functionDecl);
  for (size_t index = functionDecl->params.size(); index > 0; --index) {
    this->state.walk.tasks.pushTask(StartFunctionParam{.draft = task.draft, .index = index - 1});
  }
}

void DeclarationFunctionsRegistry::padHookParams(FunctionDecl *functionDecl) {
  auto hook = this->state.library.streams.findHookParams(functionDecl);
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

const Type *DeclarationFunctionsRegistry::hookedParam(const FunctionDecl *functionDecl,
                                                      size_t index) const {
  auto hook = this->state.library.streams.findHookParams(functionDecl);
  bool isHooked = hook.has_value() && index < hook->size();

  return isHooked ? (*hook)[index] : nullptr;
}

bool DeclarationFunctionsRegistry::hasRestParam(const FunctionDecl *functionDecl) const {
  for (const Param &param : functionDecl->params) {
    if (param.rest()) {
      return true;
    }
  }

  return false;
}

void DeclarationFunctionsRegistry::startFunctionParam(const StartFunctionParam &task) {
  FunctionDecl *functionDecl = task.draft.functionDecl;
  Param &param = functionDecl->params[task.index];
  auto decorated = this->checkParamDecorators(functionDecl, param);
  if (decorated.isErr()) {
    this->state.diagnostics.error(param.decorators[0].loc, decorated.error().message);
  }
  if (!param.type.has_value()) {
    const auto *hooked = this->hookedParam(functionDecl, task.index);
    const auto *declared = this->unwrittenParamType(param, hooked);

    return this->placeFunctionParam(task.draft, task.index, declared);
  }
  TypeRef *ref = &*param.type;
  if (this->state.isIterableRef(*ref)) {
    this->state.walk.tasks.pushTask(
        FinishFunctionParam{.draft = task.draft, .index = task.index, .isListContext = false});

    return this->state.walk.tasks.pushTask(WalkIterableParam{
        .functionDecl = functionDecl,
        .function = task.draft.function,
        .ref = ref,
    });
  }
  this->state.types.context.enterListContext(param.rest());
  this->state.walk.tasks.pushTask(
      FinishFunctionParam{.draft = task.draft, .index = task.index, .isListContext = true});
  this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = ref, .isVoidAllowed = false});
}

void DeclarationFunctionsRegistry::finishFunctionParam(const FinishFunctionParam &task) {
  if (task.isListContext) {
    this->state.types.context.leaveListContext();
  }
  const FunctionDecl *functionDecl = task.draft.functionDecl;
  const Param &param = functionDecl->params[task.index];
  const auto *hooked = this->hookedParam(functionDecl, task.index);
  const auto *type = param.type->resolved;
  bool isIntHook = hooked != nullptr && type != nullptr && type->isFloat() && hooked->isInt();
  const Type *declared = isIntHook ? hooked : type;

  return this->placeFunctionParam(task.draft, task.index, declared);
}

const Type *DeclarationFunctionsRegistry::unwrittenParamType(const Param &param,
                                                             const Type *hooked) {
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

void DeclarationFunctionsRegistry::placeFunctionParam(const FunctionSymbolDraft &draft,
                                                      size_t index, const Type *declared) {
  FunctionDecl *functionDecl = draft.functionDecl;
  FuncSymbol *function = draft.function;
  Param &param = functionDecl->params[index];
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

void DeclarationFunctionsRegistry::startFunctionReturn(const StartFunctionReturn &task) {
  FunctionDecl *functionDecl = task.draft.functionDecl;
  if (functionDecl->returnType.has_value()) {
    this->state.walk.tasks.pushTask(FinishFunctionReturn{.draft = task.draft});

    return this->startReturnType(
        StartReturnType{.functionDecl = functionDecl, .function = task.draft.function});
  }
  if (functionDecl->isGenerator || this->isReturningValue(functionDecl->body)) {
    return;
  }
  const Type *nothing = functionDecl->isAsync ? Type::promise(Type::voidTy()) : Type::voidTy();
  task.draft.function->setReturnType(nothing);
}

void DeclarationFunctionsRegistry::finishFunctionReturn(const FinishFunctionReturn &task) {
  const FunctionDecl *functionDecl = task.draft.functionDecl;
  FuncSymbol *function = task.draft.function;
  const auto *declared = this->declaredReturnType(functionDecl);
  function->setReturnType(declared);
  this->state.walk.tasks.pushTask(
      WalkAsyncReturn{.functionDecl = functionDecl, .function = function});
  this->state.walk.tasks.pushTask(
      WalkGeneratorReturn{.functionDecl = functionDecl, .function = function});
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
DeclarationFunctionsRegistry::notAsserting(const FunctionDecl *functionDecl,
                                           const TypeRef &ref) const {
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

  return ref.args[0].resolved;
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

void DeclarationFunctionsRegistry::declareMethodReturn(const DeclareMethodReturn &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  this->state.walk.tasks.pushTask(FinishMethodReturn{.functionDecl = functionDecl});

  return this->startReturnType(
      StartReturnType{.functionDecl = functionDecl, .function = functionDecl->sym});
}

void DeclarationFunctionsRegistry::finishMethodReturn(const FinishMethodReturn &task) {
  const auto *declared = this->declaredReturnType(task.functionDecl);

  return task.functionDecl->sym->setReturnType(declared);
}

void DeclarationFunctionsRegistry::startReturnType(const StartReturnType &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  TypeRef &ref = *functionDecl->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &ref, .isVoidAllowed = true});
  }
  bool isReporting = task.function != nullptr;
  if (ref.asserts() && !isReporting) {
    return;
  }
  if (ref.asserts()) {
    this->state.walk.tasks.pushTask(FinishAssertsReturn{.functionDecl = functionDecl});

    return this->pushPredicateType(ref);
  }
  auto named = this->predicateArg(functionDecl, ref);
  if (named.isErr() && isReporting) {
    this->state.diagnostics.error(ref.loc, named.error().message);
  }
  this->state.walk.tasks.pushTask(
      FinishPredicateReturn{.functionDecl = functionDecl, .function = task.function});
  this->pushPredicateType(ref);
}

void DeclarationFunctionsRegistry::pushPredicateType(TypeRef &ref) {
  if (ref.args.empty()) {
    return;
  }
  this->state.walk.tasks.pushTask(ResolveTypeRef{.ref = &ref.args[0], .isVoidAllowed = false});
}

void DeclarationFunctionsRegistry::finishAssertsReturn(const FinishAssertsReturn &task) {
  const TypeRef &ref = *task.functionDecl->returnType;
  auto plain = this->notAsserting(task.functionDecl, ref);
  if (plain.isErr()) {
    this->state.diagnostics.error(ref.loc, plain.error().message);
  }
}

void DeclarationFunctionsRegistry::finishPredicateReturn(const FinishPredicateReturn &task) {
  const TypeRef &ref = *task.functionDecl->returnType;
  auto named = this->predicateArg(task.functionDecl, ref);
  const auto *type = this->predicateType(ref);
  bool isRecorded = task.function != nullptr && named.isOk() && type != nullptr && !type->isError();
  if (isRecorded) {
    task.function->predicate = TypePredicate{.param = named.value(), .type = type};
  }
}

const Type *
DeclarationFunctionsRegistry::declaredReturnType(const FunctionDecl *functionDecl) const {
  const TypeRef &ref = *functionDecl->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return ref.resolved;
  }
  if (ref.asserts()) {
    return Type::voidTy();
  }

  return Type::boolTy();
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

void DeclarationFunctionsRegistry::declareItemFunction(Stmt *item, Module *module) {
  auto *functionDecl = dyn_cast<FunctionDecl>(item);
  if (functionDecl == nullptr) {
    return;
  }
  this->state.declaration.modules.rememberDeclModule(functionDecl, module);
  this->state.inference.cycles.addDeclaredFunction(functionDecl);
  this->state.walk.tasks.pushTask(
      StartFunctionSymbol{.functionDecl = functionDecl, .variable = nullptr, .isNamed = true});
}

}
