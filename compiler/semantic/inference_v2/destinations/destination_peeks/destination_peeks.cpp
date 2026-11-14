#include "compiler/semantic/inference_v2/destinations/destination_peeks/destination_peeks.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/global_member_table/global_member_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/dict/dict_method_table/dict_method_table.h"
#include "compiler/program/library/library_module_names/library_module_names.h"
#include "compiler/semantic/bodies/shared_helpers/shared_helpers.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

DestinationPeeks::DestinationPeeks(SemanticState &owner)
    : state(owner) {}

const Type *DestinationPeeks::peekName(llvm::StringRef name,
                                       const DestinationSearch &search) const {
  auto local = search.findLocal(name);
  if (local.has_value()) {
    return *local;
  }
  auto entry = this->state.walk.scopes.findName(name);
  VarSymbol *variable = entry.var();
  if (variable == nullptr) {
    return nullptr;
  }

  return variable->type;
}

const Type *DestinationPeeks::peekType(const Expr *expr, const DestinationSearch &search) const {
  auto answers = std::vector<const Type *>();
  for (const Expr *node : ChildrenFirst::order(expr, this, &DestinationPeeks::peekParts)) {
    const auto *type = this->nodePeekType(node, search, answers);
    answers.push_back(type);
  }

  return answers.back();
}

NodeChildren<const Expr *> DestinationPeeks::peekParts(const Expr *expr) const {
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (member != nullptr) {
    return {member->object};
  }
  if (index != nullptr) {
    return {index->object};
  }
  if (conditional != nullptr && this->state.inference.destinations.isLiteralShaped(conditional)) {
    return {conditional->thenExpr, conditional->elseExpr};
  }
  if (call != nullptr && this->state.inference.destinations.isResultLiteral(call) &&
      !call->args.empty()) {
    return {call->args[0]};
  }

  return {};
}

const Type *DestinationPeeks::nodePeekType(const Expr *expr, const DestinationSearch &search,
                                           std::vector<const Type *> &answers) const {
  auto *name = dyn_cast<NameExpr>(expr);
  auto *member = dyn_cast<MemberExpr>(expr);
  auto *index = dyn_cast<IndexExpr>(expr);
  auto *conditional = dyn_cast<ConditionalExpr>(expr);
  auto *call = dyn_cast<CallExpr>(expr);

  if (name != nullptr) {
    return this->namePeekType(name, search);
  }
  if (member != nullptr) {
    const Type *object = ChildrenFirst::takeAnswer(answers);

    return this->memberPeekType(member, object);
  }
  if (index != nullptr) {
    const Type *object = ChildrenFirst::takeAnswer(answers);

    return this->elementPeekType(object);
  }
  if (conditional != nullptr && this->state.inference.destinations.isLiteralShaped(conditional)) {
    return this->choicePeekType(answers);
  }
  if (call != nullptr && this->state.inference.destinations.isResultLiteral(call)) {
    return this->resultPeekType(call, answers);
  }

  return nullptr;
}

const Type *DestinationPeeks::namePeekType(const NameExpr *name,
                                           const DestinationSearch &search) const {
  if (name->name == search.getName()) {
    return nullptr;
  }

  return this->peekName(name->name, search);
}

const Type *DestinationPeeks::elementPeekType(const Type *object) const {
  if (object == nullptr) {
    return nullptr;
  }
  if (object->isRecord()) {
    return object->valueType();
  }
  if (object->isArray()) {
    return object->inner();
  }

  return nullptr;
}

const Type *DestinationPeeks::choicePeekType(std::vector<const Type *> &answers) const {
  const Type *second = ChildrenFirst::takeAnswer(answers);
  const Type *first = ChildrenFirst::takeAnswer(answers);
  if (first == nullptr || second == nullptr) {
    return nullptr;
  }
  const Type *common = this->state.types.relations.unify(first, second);
  if (common == nullptr || common->isError()) {
    return nullptr;
  }

  return common;
}

const Type *DestinationPeeks::memberPeekType(const MemberExpr *member, const Type *object) const {
  if (this->state.isGlobalMember(member, GlobalMember::CRYPTO_SUBTLE)) {
    return this->peekSubtle();
  }
  if (object == nullptr) {
    return nullptr;
  }
  const Type *plain = object->nonOptional();
  if (plain->isRecord()) {
    return plain->valueType();
  }
  if (!plain->isObject()) {
    return nullptr;
  }
  const Field *field = plain->field(member->name);
  if (field == nullptr) {
    return nullptr;
  }

  return field->type;
}

const Type *DestinationPeeks::peekSubtle() const {
  auto entry = this->state.walk.scopes.findName(LibraryModuleNames::kSubtleName);
  VarSymbol *subtle = entry.var();
  if (subtle == nullptr) {
    return nullptr;
  }

  return subtle->type;
}

const Type *DestinationPeeks::resultPeekType(const CallExpr *call,
                                             std::vector<const Type *> &answers) const {
  const Type *half = this->resultHalf(call, answers);
  if (half == nullptr) {
    return nullptr;
  }
  auto made = GlobalNameTable::find(cast<NameExpr>(call->callee)->name);
  if (made == GlobalName::OK) {
    return Type::result(half, Type::never());
  }

  return Type::result(Type::never(), half);
}

const Type *DestinationPeeks::resultHalf(const CallExpr *call,
                                         std::vector<const Type *> &answers) const {
  if (call->args.empty()) {
    return Type::voidTy();
  }
  const Type *peeked = ChildrenFirst::takeAnswer(answers);
  if (peeked != nullptr) {
    return peeked;
  }

  return SharedHelpers::literalType(call->args[0]);
}

std::vector<const Type *> DestinationPeeks::calleeParams(const CallExpr *call,
                                                         const DestinationSearch &search) {
  auto *name = dyn_cast<NameExpr>(call->callee);
  auto *member = dyn_cast<MemberExpr>(call->callee);

  if (name != nullptr) {
    return this->nameCalleeParams(call, name, search);
  }
  if (member != nullptr) {
    return this->memberCalleeParams(call, member, search);
  }

  return {};
}

std::vector<const Type *> DestinationPeeks::spreadParams(llvm::ArrayRef<const Type *> params,
                                                         bool hasRest, size_t argCount) const {
  auto types = std::vector<const Type *>(params.begin(), params.end());
  if (!hasRest || types.empty()) {
    return types;
  }
  const Type *rest = types.back();
  types.pop_back();
  bool isArrayRest = rest != nullptr && rest->isArray();
  const Type *element = isArrayRest ? rest->inner() : nullptr;
  while (types.size() < argCount) {
    types.push_back(element);
  }

  return types;
}

std::vector<const Type *> DestinationPeeks::nameCalleeParams(const CallExpr *call,
                                                             const NameExpr *name,
                                                             const DestinationSearch &search) {
  size_t argCount = call->args.size();
  if (search.isHidden(name->name)) {
    const auto *local = this->peekName(name->name, search);

    return this->functionTypeParams(local, argCount);
  }
  auto entry = this->state.walk.scopes.findName(name->name);
  VarSymbol *variable = entry.var();
  const FuncSymbol *function = this->calledFunction(entry);
  if (function != nullptr) {
    return this->spreadParams(function->params, function->hasRest, argCount);
  }
  if (variable == nullptr) {
    return {};
  }

  return this->functionTypeParams(variable->type, argCount);
}

const FuncSymbol *DestinationPeeks::calledFunction(const ScopeEntry &entry) const {
  VarSymbol *variable = entry.var();
  if (variable != nullptr && variable->function() != nullptr) {
    return variable->function()->sym;
  }

  return entry.func();
}

std::vector<const Type *> DestinationPeeks::functionTypeParams(const Type *type,
                                                               size_t argCount) const {
  if (type == nullptr || !type->isFunction()) {
    return {};
  }

  return this->spreadParams(type->params(), type->hasRest(), argCount);
}

std::vector<const Type *> DestinationPeeks::memberCalleeParams(const CallExpr *call,
                                                               const MemberExpr *member,
                                                               const DestinationSearch &search) {
  size_t argCount = call->args.size();
  const ClassDecl *owner = this->namedClass(member->object, search);
  if (owner != nullptr) {
    auto methods = owner->findStaticMethodsNamed(member->name);

    return this->soleMethodParams(methods, argCount);
  }
  const auto *peeked = this->peekType(member->object, search);
  if (peeked == nullptr) {
    return {};
  }
  const Type *object = peeked->nonOptional();
  const ClassDecl *classDecl = object->classDecl();
  if (classDecl != nullptr) {
    auto methods = classDecl->findMethodsNamed(member->name);

    return this->soleMethodParams(methods, argCount);
  }
  if (object->isArray()) {
    return this->arrayMethodParams(object, member->name, argCount);
  }
  if (this->hasFunctionField(object, member->name)) {
    return this->functionTypeParams(object->field(member->name)->type, argCount);
  }

  return this->dictMethodParams(object, member->name);
}

const ClassDecl *DestinationPeeks::namedClass(const Expr *object,
                                              const DestinationSearch &search) const {
  auto *owner = dyn_cast<NameExpr>(object);
  if (owner == nullptr || search.isHidden(owner->name)) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(owner->name);

  return entry.cls();
}

bool DestinationPeeks::hasFunctionField(const Type *object, llvm::StringRef name) const {
  if (!object->isObject()) {
    return false;
  }
  const Field *field = object->field(name);

  return field != nullptr && field->type->isFunction();
}

std::vector<const Type *> DestinationPeeks::soleMethodParams(llvm::ArrayRef<FunctionDecl *> methods,
                                                             size_t argCount) const {
  if (methods.size() != 1 || methods[0]->sym == nullptr) {
    return {};
  }
  const FuncSymbol *method = methods[0]->sym;

  return this->spreadParams(method->params, method->hasRest, argCount);
}

std::vector<const Type *> DestinationPeeks::arrayMethodParams(const Type *array,
                                                              llvm::StringRef name,
                                                              size_t argCount) const {
  const auto *method = ArrayMethodTable::find(name);
  if (method == nullptr) {
    return {};
  }
  ArrayMethod which = method->method;
  bool isAdding = which == ArrayMethod::PUSH || which == ArrayMethod::UNSHIFT;
  bool isSearching = which == ArrayMethod::INCLUDES || which == ArrayMethod::INDEX_OF ||
                     which == ArrayMethod::LAST_INDEX_OF;
  if (isAdding || isSearching) {
    return std::vector<const Type *>(argCount, array->inner());
  }
  if (which == ArrayMethod::FILL) {
    return {array->inner()};
  }

  return {};
}

std::vector<const Type *> DestinationPeeks::dictMethodParams(const Type *object,
                                                             llvm::StringRef name) const {
  const auto *dictMethod = DictMethodTable::find(name);
  if (dictMethod == nullptr) {
    return {};
  }
  DictMethod method = dictMethod->method;
  if (object->isMap() && method == DictMethod::SET) {
    return {object->keyType(), object->valueType()};
  }
  bool isTakingKey = method == DictMethod::GET || method == DictMethod::HAS ||
                     method == DictMethod::DELETE_KEY || method == DictMethod::ADD;
  if (object->isDict() && !object->isRecord() && isTakingKey) {
    return {object->keyType()};
  }

  return {};
}

const FuncSymbol *DestinationPeeks::soleConstructor(const NewExpr *create,
                                                    const DestinationSearch &search) const {
  if (search.isHidden(create->name)) {
    return nullptr;
  }
  auto entry = this->state.walk.scopes.findName(create->name);
  ClassDecl *classDecl = entry.cls();
  if (classDecl == nullptr || classDecl->constructors.size() != 1) {
    return nullptr;
  }

  return classDecl->constructors[0]->sym;
}

}
