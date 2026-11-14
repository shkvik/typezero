#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/program/ast/ast_cloner/ast_cloner.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "compiler/semantic/library/async/async_checker/async_checker.h"

namespace tz {

ReturnRules::ReturnRules(SemanticState &owner)
    : state(owner) {}

InstanceRecord *ReturnRules::lateOwnerOf(const FunctionDecl *functionDecl) const {
  if (functionDecl == nullptr || functionDecl->cls() == nullptr) {
    return nullptr;
  }

  return this->state.code.late.findLateOwner(functionDecl->cls());
}

const Type *ReturnRules::collectedType(const FunctionDecl *functionDecl, const FuncSymbol *function,
                                       const CollectedReturns &collected, bool isFallingOff,
                                       ReturnErrors errors) {
  if (functionDecl->isGenerator) {
    return this->inferGeneratorType(function, collected.yields, collected.returns, isFallingOff,
                                    errors);
  }
  if (functionDecl->isAsync) {
    return this->state.library.async.inferAsyncType(function, collected.returns, isFallingOff,
                                                    errors);
  }

  return this->unifyReturns(function, collected.returns, isFallingOff, errors);
}

bool ReturnRules::isIntLiteralReturn(const ReturnStmt *returnStmt) const {
  const Expr *value = returnStmt->value;
  if (value == nullptr || value->type == nullptr || !value->type->isInteger()) {
    return false;
  }

  return this->state.code.conversions.isIntLiteralTree(value);
}

std::vector<ReturnStmt *> ReturnRules::literalsLast(llvm::ArrayRef<ReturnStmt *> returns) const {
  auto ordered = std::vector<ReturnStmt *>();
  auto literals = std::vector<ReturnStmt *>();
  for (ReturnStmt *returnStmt : returns) {
    if (this->isIntLiteralReturn(returnStmt)) {
      literals.push_back(returnStmt);
    } else {
      ordered.push_back(returnStmt);
    }
  }
  ordered.insert(ordered.end(), literals.begin(), literals.end());

  return ordered;
}

ReturnRules::ResultReturnsUnified ReturnRules::commonReturn(const FuncSymbol *function,
                                                            const Type *before,
                                                            const ReturnStmt *returnStmt) {
  const Type *found = returnStmt->value->type;
  if (before == nullptr) {
    return ok(found);
  }
  bool isAdapting =
      before != found && this->state.code.conversions.isLiteralAdapting(returnStmt->value, before);
  if (isAdapting) {
    return ok(before);
  }
  const auto *common = this->state.types.literals.unionUnify(before, found);
  if (common == nullptr || common->isError()) {
    auto message = ErrorTextCommonReturn::differ(function->name, found->name(), before->name());

    return err(InferenceReturnsRegistryCommonReturnError{
        .code = InferenceReturnsRegistryCommonReturnErrorCode::DIFFERENT_TYPES,
        .message = message,
    });
  }

  return ok(common);
}

bool ReturnRules::hasValuedReturn(llvm::ArrayRef<ReturnStmt *> returns) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (returnStmt->value != nullptr) {
      return true;
    }
  }

  return false;
}

ReturnRules::ResultReturnsDefined ReturnRules::definedReturn(const FuncSymbol *function,
                                                             llvm::ArrayRef<ReturnStmt *> returns,
                                                             bool isUndefined) const {
  if (isUndefined && this->hasValuedReturn(returns)) {
    auto message = ErrorTextDefinedReturn::undefined(function->name);

    return err(InferenceReturnsRegistryDefinedReturnError{
        .code = InferenceReturnsRegistryDefinedReturnErrorCode::ONLY_UNDEFINED,
        .message = message,
    });
  }

  return ok(function);
}

bool ReturnRules::isUndefinedReturn(const ReturnStmt *returnStmt) const {
  return returnStmt->value == nullptr || returnStmt->value->type->isUndefined();
}

bool ReturnRules::hasUndefinedReturn(llvm::ArrayRef<ReturnStmt *> returns) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (this->isUndefinedReturn(returnStmt)) {
      return true;
    }
  }

  return false;
}

std::vector<const ReturnStmt *>
ReturnRules::typedReturns(llvm::ArrayRef<ReturnStmt *> returns) const {
  auto typed = std::vector<const ReturnStmt *>();
  for (const ReturnStmt *returnStmt : this->literalsLast(returns)) {
    bool isTyped = !this->isUndefinedReturn(returnStmt) && !returnStmt->value->type->isError();
    if (isTyped) {
      typed.push_back(returnStmt);
    }
  }

  return typed;
}

const Type *ReturnRules::unifyReturns(const FuncSymbol *function,
                                      llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                      ReturnErrors errors) {
  const Type *type = nullptr;
  for (const ReturnStmt *returnStmt : this->typedReturns(returns)) {
    auto common = this->commonReturn(function, type, returnStmt);
    if (common.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.diagnostics.error(returnStmt->value->loc(), common.error().message);
    }
    if (common.isErr()) {
      return Type::error();
    }
    type = common.value();
  }
  bool isUndefined = isFallingOff || this->hasUndefinedReturn(returns);
  if (type != nullptr && isUndefined && !type->isVoid()) {
    return Type::optional(type);
  }
  if (type != nullptr) {
    return type;
  }
  auto defined = this->definedReturn(function, returns, isUndefined);
  if (defined.isErr() && errors == ReturnErrors::REPORTED) {
    this->state.diagnostics.error(function->loc, defined.error().message);
  }

  return errors == ReturnErrors::REPORTED ? Type::error() : nullptr;
}

const Type *ReturnRules::inferGeneratorType(const FuncSymbol *function,
                                            llvm::ArrayRef<YieldExpr *> yields,
                                            llvm::ArrayRef<ReturnStmt *> returns, bool isFallingOff,
                                            ReturnErrors errors) {
  bool isAsync = function->decl != nullptr && function->decl->isAsync;
  auto join = this->joinYields(function, yields, isAsync, errors);
  if (join.hasFailed) {
    return Type::error();
  }
  if (join.yielded == nullptr || join.yielded->isUndefined()) {
    return this->unknownYields(function, yields, join, errors);
  }
  auto returned = this->joinedReturns(function, returns, isFallingOff, isAsync, errors);
  if (!returned.has_value()) {
    return Type::error();
  }
  if (isAsync) {
    return Type::asyncIterator(join.yielded, *returned, Type::voidTy());
  }

  return Type::iterator(join.yielded, *returned, Type::voidTy());
}

ReturnRules::YieldJoin ReturnRules::joinYields(const FuncSymbol *function,
                                               llvm::ArrayRef<YieldExpr *> yields, bool isAsync,
                                               ReturnErrors errors) {
  auto known = this->knownYields(yields, isAsync);
  bool isBroken = known.size() != yields.size();
  const Type *yielded = nullptr;
  for (const KnownYield &value : known) {
    auto joined = this->checkYieldsJoin(yielded, value.type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.diagnostics.error(value.yield->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      return YieldJoin{
          .yielded = yielded,
          .isBroken = isBroken,
          .hasFailed = true,
      };
    }
    yielded = joined.value();
  }

  return YieldJoin{
      .yielded = yielded,
      .isBroken = isBroken,
      .hasFailed = false,
  };
}

std::vector<ReturnRules::KnownYield> ReturnRules::knownYields(llvm::ArrayRef<YieldExpr *> yields,
                                                              bool isAsync) const {
  auto known = std::vector<KnownYield>();
  for (const YieldExpr *yield : yields) {
    const auto *type = this->yieldedValueType(yield, isAsync);
    if (type != nullptr && !type->isError()) {
      known.push_back(KnownYield{.yield = yield, .type = type});
    }
  }

  return known;
}

const Type *ReturnRules::yieldedValueType(const YieldExpr *yield, bool isAsync) const {
  if (yield->delegate) {
    return yield->element;
  }
  const Type *type = yield->value != nullptr ? yield->value->type : Type::undefinedTy();
  if (!isAsync || type == nullptr || type->isError()) {
    return type;
  }

  return AsyncChecker::awaitedType(type);
}

const Type *ReturnRules::unknownYields(const FuncSymbol *function,
                                       llvm::ArrayRef<YieldExpr *> yields, const YieldJoin &join,
                                       ReturnErrors errors) {
  if (errors == ReturnErrors::IGNORED) {
    return nullptr;
  }
  if (join.isBroken && join.yielded == nullptr) {
    return Type::error();
  }
  auto known = this->checkYieldsKnown(join.yielded, function, yields.empty());
  if (known.isErr()) {
    this->state.diagnostics.error(function->loc, known.error().message);
  }

  return Type::error();
}

std::optional<const Type *> ReturnRules::joinedReturns(const FuncSymbol *function,
                                                       llvm::ArrayRef<ReturnStmt *> returns,
                                                       bool isFallingOff, bool isAsync,
                                                       ReturnErrors errors) {
  bool isUndefined = isFallingOff || this->hasUndefinedGeneratorReturn(returns, isAsync);
  const Type *returned = nullptr;
  auto known = this->knownReturns(returns, isAsync);
  for (const KnownReturn &value : known) {
    auto joined = this->checkReturnsJoin(returned, value.type, function);
    if (joined.isErr() && errors == ReturnErrors::REPORTED) {
      this->state.diagnostics.error(value.returnStmt->value->loc(), joined.error().message);
    }
    if (joined.isErr()) {
      return std::nullopt;
    }
    returned = joined.value();
  }
  if (returned == nullptr) {
    return Type::voidTy();
  }
  if (isUndefined && !returned->isVoid()) {
    return Type::optional(returned);
  }

  return returned;
}

bool ReturnRules::hasUndefinedGeneratorReturn(llvm::ArrayRef<ReturnStmt *> returns,
                                              bool isAsync) const {
  for (const ReturnStmt *returnStmt : returns) {
    if (this->returnedValueType(returnStmt, isAsync) == nullptr) {
      return true;
    }
  }

  return false;
}

std::vector<ReturnRules::KnownReturn>
ReturnRules::knownReturns(llvm::ArrayRef<ReturnStmt *> returns, bool isAsync) const {
  auto known = std::vector<KnownReturn>();
  for (const ReturnStmt *returnStmt : returns) {
    const auto *type = this->returnedValueType(returnStmt, isAsync);
    if (type != nullptr && !type->isError()) {
      known.push_back(KnownReturn{.returnStmt = returnStmt, .type = type});
    }
  }

  return known;
}

const Type *ReturnRules::returnedValueType(const ReturnStmt *returnStmt, bool isAsync) const {
  if (returnStmt->value == nullptr || returnStmt->value->type->isUndefined()) {
    return nullptr;
  }
  const Type *type = returnStmt->value->type;
  if (type->isError() || !isAsync) {
    return type;
  }
  const auto *awaited = AsyncChecker::awaitedType(type);
  if (awaited == nullptr || awaited->isUndefined()) {
    return nullptr;
  }

  return awaited;
}

ReturnRules::ResultYieldedTypesJoin
ReturnRules::checkYieldsJoin(const Type *yielded, const Type *type, const FuncSymbol *function) {
  const Type *common =
      yielded != nullptr ? this->state.types.literals.unionUnify(yielded, type) : type;
  if (common == nullptr || common->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYields::differ(name, type->name(), yielded->name());

    return err(InferenceReturnsRegistryYieldsJoinError{
        .code = InferenceReturnsRegistryYieldsJoinErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

ReturnRules::ResultReturnsOfOneType
ReturnRules::checkReturnsJoin(const Type *returned, const Type *type, const FuncSymbol *function) {
  const Type *common =
      returned != nullptr ? this->state.types.literals.unionUnify(returned, type) : type;
  if (common == nullptr || common->isError()) {
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextGeneratorReturns::differ(name, type->name(), returned->name());

    return err(InferenceReturnsRegistryGeneratorReturnsError{
        .code = InferenceReturnsRegistryGeneratorReturnsErrorCode::TYPES_DIFFER,
        .message = message,
    });
  }

  return ok(common);
}

ReturnRules::ResultYieldedTypeKnown ReturnRules::checkYieldsKnown(const Type *yielded,
                                                                  const FuncSymbol *function,
                                                                  bool hasNoYields) const {
  if (yielded == nullptr || yielded->isUndefined()) {
    auto why = this->nothingYieldedReason(yielded, hasNoYields);
    auto name = ErrorTextFunctionName::quoted(function->name);
    auto message = ErrorTextYields::unknown(name, why);

    return err(InferenceReturnsRegistryYieldsKnownError{
        .code = InferenceReturnsRegistryYieldsKnownErrorCode::NOTHING_YIELDED,
        .message = message,
    });
  }

  return ok(yielded);
}

std::string ReturnRules::nothingYieldedReason(const Type *yielded, bool hasNoYields) const {
  if (hasNoYields) {
    return ErrorTextYields::noYield();
  }
  if (yielded != nullptr) {
    return ErrorTextYields::onlyUndef();
  }

  return "";
}

FunctionDecl *ReturnRules::freshCopy(const FunctionDecl *functionDecl) {
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

void ReturnRules::copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
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

void ReturnRules::copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const {
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

}
