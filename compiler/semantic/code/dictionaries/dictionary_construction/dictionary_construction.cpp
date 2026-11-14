#include "compiler/semantic/code/dictionaries/dictionary_construction/dictionary_construction.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

DictionaryConstruction::DictionaryConstruction(SemanticState &owner,
                                               DictionaryRules &sharedDictionaryRules)
    : state(owner),
      dictionaryRules(sharedDictionaryRules) {}

void DictionaryConstruction::resolveDictType(const ResolveDictType &task) {
  const TypeRef &ref = *task.ref;
  if (TypeNameTable::isName(ref.name, TypeName::WEAK_REF)) {
    return this->state.walk.tasks.pushTask(ResolveWeakRefType{.ref = task.ref});
  }
  auto shaped = this->checkDictTypeShape(ref);
  if (shaped.isErr()) {
    this->state.diagnostics.error(ref.loc, shaped.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }
  this->state.walk.tasks.pushTask(
      FinishDictTypeAnswer{.ref = task.ref, .collection = shaped.value()});

  return this->pushTypeArguments(ref.args, shaped.value());
}

void DictionaryConstruction::finishDictTypeAnswer(const FinishDictTypeAnswer &task) {
  const TypeRef &ref = *task.ref;
  bool isSet = task.collection == Collection::SET;
  const Type *value = isSet ? Type::voidTy() : this->state.walk.tasks.takeAnswer();
  const Type *key = this->state.walk.tasks.takeAnswer();
  auto typed = this->checkCollectionType(task.collection, key, value);
  if (typed.isErr()) {
    bool isOnKey = this->isKeyArgument(typed.error().code);
    llvm::SMLoc at = isOnKey ? ref.args[0].loc : ref.loc;
    this->state.diagnostics.error(at, typed.error().message);
    return this->state.walk.tasks.pushAnswer(Type::error());
  }

  return this->state.walk.tasks.pushAnswer(typed.value());
}

void DictionaryConstruction::pushTypeArguments(const std::vector<TypeRef> &args,
                                               Collection collection) {
  if (collection != Collection::SET) {
    this->state.walk.tasks.pushTask(ResolveTypeAnswer{.ref = &args[1], .use = TypeUse::STANDALONE});
  }

  return this->state.walk.tasks.pushTask(
      ResolveTypeAnswer{.ref = &args[0], .use = TypeUse::STANDALONE});
}

DictionaryConstruction::ResultDictTypeShape
DictionaryConstruction::checkDictTypeShape(const TypeRef &ref) const {
  const auto *builtin = TypeNameTable::find(ref.name);
  if (builtin->form.is<WeakCollectionTypeName>()) {
    auto message = ErrorTextDictType::weak(ref.name, ref.name.drop_front(4));

    return err(CodeDictionariesRegistryResolveDictTypeError{
        .code = CodeDictionariesRegistryResolveDictTypeErrorCode::WEAK_COLLECTION,
        .message = message,
    });
  }

  return this->checkTypeArgumentCount(ref.name, ref.args.size());
}

DictionaryConstruction::ResultDictTypeShape
DictionaryConstruction::checkTypeArgumentCount(llvm::StringRef name, size_t count) const {
  const auto *builtin = TypeNameTable::find(name);
  Collection collection = builtin->form.as<CollectionTypeName>()->collection;
  size_t wanted = collection == Collection::SET ? 1 : 2;
  if (count != wanted) {
    auto message = this->typeArgumentsText(name, collection);

    return err(CodeDictionariesRegistryResolveDictTypeError{
        .code = CodeDictionariesRegistryResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS,
        .message = message,
    });
  }

  return ok(collection);
}

std::string DictionaryConstruction::typeArgumentsText(llvm::StringRef name,
                                                      Collection collection) const {
  switch (collection) {
    case Collection::SET:
      return ErrorTextDictType::setArgs(name);
    case Collection::MAP:
      return ErrorTextDictType::mapArgs(name);
    case Collection::RECORD:
      return ErrorTextDictType::recordArgs();
  }

  return ErrorTextDictType::recordArgs();
}

DictionaryConstruction::ResultDictTypeResolved
DictionaryConstruction::checkCollectionType(Collection collection, const Type *key,
                                            const Type *value) {
  if (key->isError() || value->isError()) {
    return ok(Type::error());
  }
  if (collection == Collection::RECORD) {
    return this->checkRecordType(key, value);
  }
  auto allowed = this->dictionaryRules.checkKeyType(key);
  if (allowed.isErr()) {
    return err(CodeDictionariesRegistryResolveDictTypeError{
        .code = CodeDictionariesRegistryResolveDictTypeErrorCode::BAD_KEY_TYPE,
        .message = allowed.error().message,
    });
  }
  auto kind = collection == Collection::MAP ? Type::DictKind::Map : Type::DictKind::Set;
  const auto *dict = Type::dict(kind, key, value);

  return ok(dict);
}

DictionaryConstruction::ResultDictTypeResolved
DictionaryConstruction::checkRecordType(const Type *key, const Type *value) const {
  if (this->isLiteralKeys(key)) {
    const auto *object = this->literalKeysObject(key, value);

    return ok(object);
  }
  if (!key->isString() && key->nonOptional()->isNumeric()) {
    auto message = ErrorTextDictType::numberKeys(key->name(), value->name());

    return err(CodeDictionariesRegistryResolveDictTypeError{
        .code = CodeDictionariesRegistryResolveDictTypeErrorCode::NUMBER_KEYS,
        .message = message,
    });
  }
  if (!key->isString()) {
    auto message = ErrorTextDictType::otherKeys(key->name(), value->name());

    return err(CodeDictionariesRegistryResolveDictTypeError{
        .code = CodeDictionariesRegistryResolveDictTypeErrorCode::OTHER_KEYS,
        .message = message,
    });
  }
  const auto *dict = Type::dict(Type::DictKind::Record, key, value);

  return ok(dict);
}

bool DictionaryConstruction::isLiteralKeys(const Type *key) const {
  if (key->isStringLiteral()) {
    return true;
  }
  if (!key->isUnion()) {
    return false;
  }
  for (const Type *member : key->members()) {
    if (!member->isStringLiteral()) {
      return false;
    }
  }

  return true;
}

const Type *DictionaryConstruction::literalKeysObject(const Type *key, const Type *value) const {
  llvm::ArrayRef<const Type *> literals =
      key->isUnion() ? key->members() : llvm::ArrayRef<const Type *>(key);
  auto fields = std::vector<Field>();
  for (const Type *member : literals) {
    auto text = ToUtf8::toUtf8(member->literalText());
    fields.push_back({text, value, false});
  }

  return Type::object(std::move(fields));
}

bool DictionaryConstruction::isKeyArgument(
    CodeDictionariesRegistryResolveDictTypeErrorCode code) const {
  switch (code) {
    case CodeDictionariesRegistryResolveDictTypeErrorCode::NUMBER_KEYS:
    case CodeDictionariesRegistryResolveDictTypeErrorCode::OTHER_KEYS:
    case CodeDictionariesRegistryResolveDictTypeErrorCode::BAD_KEY_TYPE:
      return true;
    case CodeDictionariesRegistryResolveDictTypeErrorCode::WEAK_COLLECTION:
    case CodeDictionariesRegistryResolveDictTypeErrorCode::WRONG_TYPE_ARGUMENTS:
      return false;
  }

  return false;
}

void DictionaryConstruction::checkDictNew(const CheckDictNew &task) {
  NewExpr *expr = task.newExpr;
  auto global = GlobalNameTable::find(expr->name);
  if (global == GlobalName::WEAK_MAP || global == GlobalName::WEAK_SET) {
    auto message = ErrorTextDictType::weak(expr->name, expr->name.drop_front(4));
    this->state.diagnostics.error(expr->loc(), message);
    return this->dictionaryRules.failDictNew(expr);
  }
  bool isMap = global == GlobalName::MAP;
  if (expr->typeArgs.empty()) {
    const auto *wanted = this->wantedDict(task.expected, isMap);
    return this->startDictNew(expr, wanted, isMap);
  }
  auto shaped = this->checkTypeArgumentCount(expr->name, expr->typeArgs.size());
  if (shaped.isErr()) {
    this->state.diagnostics.error(expr->loc(), shaped.error().message);
    return this->dictionaryRules.failDictNew(expr);
  }
  this->state.walk.tasks.pushTask(
      AfterDictNewTypeArgs{.newExpr = expr, .collection = shaped.value()});

  return this->pushTypeArguments(expr->typeArgs, shaped.value());
}

void DictionaryConstruction::afterDictNewTypeArgs(const AfterDictNewTypeArgs &task) {
  NewExpr *expr = task.newExpr;
  bool isMap = task.collection == Collection::MAP;
  const Type *value = isMap ? this->state.walk.tasks.takeAnswer() : Type::voidTy();
  const Type *key = this->state.walk.tasks.takeAnswer();
  auto typed = this->checkCollectionType(task.collection, key, value);
  if (typed.isErr()) {
    bool isOnKey = this->isKeyArgument(typed.error().code);
    llvm::SMLoc at = isOnKey ? expr->typeArgs[0].loc : expr->loc();
    this->state.diagnostics.error(at, typed.error().message);
    return this->dictionaryRules.failDictNew(expr);
  }
  if (typed.value()->isError()) {
    return this->dictionaryRules.failDictNew(expr);
  }

  return this->startDictNew(expr, typed.value(), isMap);
}

void DictionaryConstruction::startDictNew(NewExpr *expr, const Type *type, bool isMap) {
  auto single = this->checkArgumentCount(expr, isMap);
  if (single.isErr()) {
    this->state.diagnostics.error(expr->args[1]->loc(), single.error().message);
    return this->dictionaryRules.failDictNew(expr);
  }
  bool isEmpty = expr->args.empty() || isa<UndefinedExpr>(expr->args[0]);
  if (isEmpty) {
    return this->startEmptyDictNew(expr, type, isMap);
  }
  Expr *arg = expr->args[0];
  bool isInferring = type == nullptr && isMap;
  if (isInferring) {
    this->markPairs(arg);
  }
  auto *literal = dyn_cast<ArrayLitExpr>(arg);
  bool isPairList = isInferring && literal != nullptr && !literal->elements.empty() &&
                    llvm::all_of(literal->elements,
                                 std::bind_front(&DictionaryConstruction::isPairElement, this));
  if (isPairList) {
    auto draft = PairsDraft{
        .newExpr = expr,
        .literal = literal,
        .element = 0,
        .isValue = false,
        .key = nullptr,
        .value = nullptr,
        .isTyped = true,
    };
    return this->state.walk.tasks.pushTask(NextPairPart{.draft = draft});
  }

  return this->startDictSource(expr, type, isMap);
}

void DictionaryConstruction::startEmptyDictNew(NewExpr *expr, const Type *type, bool isMap) {
  auto given = this->checkTypesGiven(expr, type, isMap, !expr->args.empty());
  if (given.isErr()) {
    this->state.diagnostics.error(expr->loc(), given.error().message);
    return this->dictionaryRules.failDictNew(expr);
  }

  return this->dictionaryRules.finishDictNew(expr, type, {});
}

void DictionaryConstruction::startDictSource(NewExpr *expr, const Type *type, bool isMap) {
  const auto *wanted = this->dictionaryRules.wantedElement(type, isMap);
  const Type *hint = wanted != nullptr ? Type::array(wanted) : nullptr;
  this->state.walk.tasks.pushTask(AfterDictSource{.newExpr = expr, .type = type, .isMap = isMap});
  this->state.walk.tasks.pushTask(IterableOfSlot{.slot = &expr->args[0]});
  this->state.walk.tasks.pushTask(CheckExprSlot{.slot = &expr->args[0], .expected = hint});

  return this->state.walk.tasks.pushTask(MarkFused{.expr = expr->args[0]});
}

void DictionaryConstruction::markPairs(Expr *arg) {
  auto *call = dyn_cast<CallExpr>(arg);
  auto *member = call != nullptr ? dyn_cast<MemberExpr>(call->callee) : nullptr;
  auto *functionExpr =
      call != nullptr && call->args.size() == 1 ? dyn_cast<FunctionExpr>(call->args[0]) : nullptr;
  const ArrayMethodInfo *method =
      member != nullptr ? ArrayMethodTable::find(member->name) : nullptr;
  bool isMapping = method != nullptr && method->method == ArrayMethod::MAP;
  bool isInferredCallback = functionExpr != nullptr && !functionExpr->fn->returnType.has_value() &&
                            functionExpr->fn->body != nullptr;
  if (!isMapping || !isInferredCallback) {
    return;
  }
  auto returns = this->branchReturns(functionExpr->fn->body);
  bool isPlain =
      llvm::all_of(returns, std::bind_front(&DictionaryConstruction::isPairReturn, this));
  if (!isPlain || returns.empty()) {
    return;
  }
  for (const ReturnStmt *returnStmt : returns) {
    this->state.code.literals.markPairLiteral(returnStmt->value);
  }
}

std::vector<const ReturnStmt *> DictionaryConstruction::branchReturns(const Stmt *body) const {
  auto returns = std::vector<const ReturnStmt *>();
  auto stack = std::vector<const Stmt *>{body};
  while (!stack.empty()) {
    const Stmt *next = stack.back();
    stack.pop_back();
    auto *returnStmt = dyn_cast_or_null<ReturnStmt>(next);
    auto *block = dyn_cast_or_null<BlockStmt>(next);
    auto *branch = dyn_cast_or_null<IfStmt>(next);
    if (returnStmt != nullptr) {
      returns.push_back(returnStmt);
    }
    if (block != nullptr) {
      this->pushBlockStatements(block, stack);
    }
    if (branch != nullptr) {
      stack.push_back(branch->elseStmt);
      stack.push_back(branch->thenStmt);
    }
  }

  return returns;
}

void DictionaryConstruction::pushBlockStatements(const BlockStmt *block,
                                                 std::vector<const Stmt *> &stack) const {
  for (const Stmt *inner : llvm::reverse(block->body)) {
    stack.push_back(inner);
  }
}

bool DictionaryConstruction::isPairReturn(const ReturnStmt *returnStmt) const {
  auto *pair = dyn_cast_or_null<ArrayLitExpr>(returnStmt->value);

  return pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
         !pair->elements[1].spread;
}

DictionaryConstruction::ResultOneArgument
DictionaryConstruction::checkArgumentCount(const NewExpr *expr, bool isMap) const {
  if (expr->args.size() > 1) {
    auto message = ErrorTextDictArgs::oneArg(expr->name, isMap);

    return err(CodeDictionariesRegistryArgumentCountError{
        .code = CodeDictionariesRegistryArgumentCountErrorCode::TOO_MANY_ARGUMENTS,
        .message = message,
    });
  }

  return ok(expr);
}

DictionaryConstruction::ResultTypesStated
DictionaryConstruction::checkTypesGiven(const NewExpr *expr, const Type *type, bool isMap,
                                        bool isUndefinedArg) const {
  if (type == nullptr && isUndefinedArg) {
    auto message = ErrorTextDictTypes::undefinedArg(expr->name, isMap);

    return err(CodeDictionariesRegistryTypesGivenError{
        .code = CodeDictionariesRegistryTypesGivenErrorCode::UNDEFINED_NEEDS_TYPES,
        .message = message,
    });
  }
  if (type == nullptr && isMap) {
    auto message = ErrorTextDictTypes::needsMap();

    return err(CodeDictionariesRegistryTypesGivenError{
        .code = CodeDictionariesRegistryTypesGivenErrorCode::MAP_NEEDS_TYPES,
        .message = message,
    });
  }
  if (type == nullptr) {
    auto message = ErrorTextDictTypes::needsSet();

    return err(CodeDictionariesRegistryTypesGivenError{
        .code = CodeDictionariesRegistryTypesGivenErrorCode::SET_NEEDS_TYPES,
        .message = message,
    });
  }

  return ok(type);
}

bool DictionaryConstruction::isPairElement(const ArrayLitExpr::Element &element) const {
  auto *pair = dyn_cast<ArrayLitExpr>(element.expr);

  return !element.spread && pair != nullptr && pair->elements.size() == 2 &&
         !pair->elements[0].spread && !pair->elements[1].spread;
}

const Type *DictionaryConstruction::wantedDict(const Type *expected, bool isMap) const {
  bool isUnion = expected != nullptr && expected->nonOptional()->isUnion();
  const Type *target = isUnion ? this->onlyDictMember(expected->nonOptional(), isMap) : expected;
  const Type *wanted = target != nullptr ? target->nonOptional() : nullptr;
  bool isFitting =
      wanted != nullptr && wanted->isDict() && wanted->isMap() == isMap && !wanted->isRecord();

  return isFitting ? wanted : nullptr;
}

const Type *DictionaryConstruction::onlyDictMember(const Type *expected, bool isMap) const {
  const Type *only = nullptr;
  int count = 0;
  for (const Type *member : expected->members()) {
    if (member->isDict() && !member->isRecord() && member->isMap() == isMap) {
      only = member;
      ++count;
    }
  }

  return count == 1 ? only : nullptr;
}

}
