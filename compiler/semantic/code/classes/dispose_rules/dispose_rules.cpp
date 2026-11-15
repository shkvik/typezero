#include "compiler/semantic/code/classes/dispose_rules/dispose_rules.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/builtin_names/keyword_table/keyword_table.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <functional>
#include <iterator>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr llvm::StringLiteral kDisposeMethod = "[Symbol.dispose]";

}

DisposeRules::DisposeRules(SemanticState &owner)
    : state(owner) {}

void DisposeRules::walkDispose(const ClassDecl *classDecl) {
  const FunctionDecl *dispose = this->ownDispose(classDecl);
  if (dispose == nullptr) {
    return;
  }
  auto shape = this->checkDisposeShape(dispose);
  if (shape.isErr()) {
    this->state.diagnostics.error(dispose->loc(), shape.error().message);
  }
  auto super = this->checkSuperDisposed(classDecl, dispose);
  if (super.isErr()) {
    this->state.diagnostics.error(dispose->loc(), super.error().message);
  }
  auto kept = this->checkThisKeptIn(dispose);
  if (kept.isErr()) {
    this->state.diagnostics.error(dispose->loc(), kept.error().message);
  }
}

void DisposeRules::walkUsing(const VarDeclStmt *varDecl, const Type *type,
                             llvm::ArrayRef<const Stmt *> following) {
  auto usable = this->checkUsingType(type);
  if (usable.isErr()) {
    this->state.diagnostics.error(varDecl->loc(), usable.error().message);
  }
  for (const VarDeclarator &declarator : varDecl->decls) {
    auto stays = this->checkUsingStays(declarator.name, following);
    if (stays.isErr()) {
      this->state.diagnostics.error(declarator.loc, stays.error().message);
    }
  }
}

bool DisposeRules::isInsideDispose() const {
  const FuncSymbol *current = this->state.walk.functions.getCurrentFunction();

  return current != nullptr && current->decl != nullptr && current->decl->name == kDisposeMethod;
}

const FunctionDecl *DisposeRules::findDispose(const ClassDecl *classDecl) const {
  for (const ClassDecl *owner = classDecl; owner != nullptr; owner = owner->parent) {
    const FunctionDecl *dispose = this->ownDispose(owner);
    if (dispose != nullptr) {
      return dispose;
    }
  }

  return nullptr;
}

DisposeRules::ResultDisposeShape
DisposeRules::checkDisposeShape(const FunctionDecl *dispose) const {
  const Type *returned = dispose->sym != nullptr ? dispose->sym->getKnownReturnType() : nullptr;
  bool isGivingValue = returned != nullptr && !returned->isVoid() && !returned->isError();
  bool isWrong = !dispose->params.empty() || dispose->isStatic() || dispose->isAsync ||
                 dispose->isGenerator || isGivingValue;
  if (isWrong) {
    auto message = ErrorTextDispose::shape();

    return err(DisposeRulesError{
        .code = DisposeRulesErrorCode::WRONG_SHAPE,
        .message = message,
    });
  }

  return ok(dispose);
}

DisposeRules::ResultSuperDispose
DisposeRules::checkSuperDisposed(const ClassDecl *classDecl, const FunctionDecl *dispose) const {
  const FunctionDecl *inherited = this->findDispose(classDecl->parent);
  if (inherited != nullptr && !this->hasSuperDisposeCall(dispose)) {
    auto message = ErrorTextDispose::noSuper(inherited->cls()->name);

    return err(DisposeRulesError{
        .code = DisposeRulesErrorCode::NO_SUPER_CALL,
        .message = message,
    });
  }

  return ok(dispose);
}

DisposeRules::ResultThisIsKeptIn DisposeRules::checkThisKeptIn(const FunctionDecl *dispose) const {
  if (this->isLeavingThis(dispose)) {
    auto message = ErrorTextDispose::thisLeaves();

    return err(DisposeRulesError{
        .code = DisposeRulesErrorCode::THIS_ESCAPES,
        .message = message,
    });
  }

  return ok(dispose);
}

DisposeRules::ResultUsingIsValid DisposeRules::checkUsingType(const Type *type) const {
  if (type == nullptr || type->isError()) {
    return ok(type);
  }
  const ClassDecl *classDecl = type->nonOptional()->classDecl();
  if (classDecl == nullptr || this->findDispose(classDecl) == nullptr) {
    auto message = ErrorTextDispose::notUsable(type->name());

    return err(DisposeRulesError{
        .code = DisposeRulesErrorCode::NOT_DISPOSABLE,
        .message = message,
    });
  }

  return ok(type);
}

DisposeRules::ResultUsingStaysIn
DisposeRules::checkUsingStays(llvm::StringRef name, llvm::ArrayRef<const Stmt *> following) const {
  auto isEscaping = std::bind_front(&DisposeRules::isEscaping, this, name);
  bool hasEscape = llvm::any_of(following, isEscaping);
  if (hasEscape) {
    auto message = ErrorTextDispose::outlives(name);

    return err(DisposeRulesError{
        .code = DisposeRulesErrorCode::OUTLIVES_BLOCK,
        .message = message,
    });
  }

  return ok(name);
}

const FunctionDecl *DisposeRules::ownDispose(const ClassDecl *classDecl) const {
  if (classDecl == nullptr) {
    return nullptr;
  }
  auto found = classDecl->findMethodsNamed(kDisposeMethod);

  return found.empty() ? nullptr : found.front();
}

bool DisposeRules::hasSuperDisposeCall(const FunctionDecl *dispose) const {
  if (dispose->body == nullptr) {
    return false;
  }
  auto exprs = AstWalker::ownExprsIn(dispose->body);
  auto isCall = std::bind_front(&DisposeRules::isSuperDisposeCall, this);

  return llvm::any_of(exprs, isCall);
}

bool DisposeRules::isSuperDisposeCall(const Expr *expr) const {
  const auto *call = dyn_cast<CallExpr>(expr);
  const Expr *callee = call != nullptr ? call->callee : nullptr;
  const auto *member = dyn_cast_or_null<MemberExpr>(callee);
  const auto *index = dyn_cast_or_null<IndexExpr>(callee);
  if (member != nullptr && member->name == kDisposeMethod) {
    return true;
  }
  const auto *key = index != nullptr ? dyn_cast<MemberExpr>(index->index) : nullptr;
  bool isDisposeKey =
      key != nullptr && this->state.isGlobalMember(key, GlobalMember::SYMBOL_DISPOSE);

  return isDisposeKey && this->isSuperName(index->object);
}

bool DisposeRules::isSuperName(const Expr *expr) const {
  return this->isNamed(expr, KeywordTable::getName(Keyword::SUPER));
}

bool DisposeRules::isNamed(const Expr *expr, llvm::StringRef name) const {
  const auto *named = dyn_cast<NameExpr>(expr);

  return named != nullptr && named->name == name;
}

bool DisposeRules::isNamedAs(llvm::StringRef name, const Expr *expr) const {
  return this->isNamed(expr, name);
}

bool DisposeRules::isLeavingThis(const FunctionDecl *dispose) const {
  if (dispose->body == nullptr) {
    return false;
  }
  auto thisName = KeywordTable::getName(Keyword::THIS);
  auto exprs = AstWalker::ownExprsIn(dispose->body);
  auto reads = llvm::SmallPtrSet<const Expr *, 16>();
  for (const Expr *expr : exprs) {
    const auto *member = dyn_cast<MemberExpr>(expr);
    if (member != nullptr && this->isNamed(member->object, thisName)) {
      reads.insert(member->object);
    }
  }
  for (const Expr *expr : exprs) {
    bool isLooseThis = this->isNamed(expr, thisName) && !reads.contains(expr);
    if (isLooseThis || this->isCaptured(expr, thisName)) {
      return true;
    }
  }

  return false;
}

bool DisposeRules::isEscaping(llvm::StringRef name, const Stmt *stmt) const {
  auto *body = const_cast<Stmt *>(stmt);
  for (const Expr *value : AstWalker::returnValuesIn(body)) {
    if (this->isNamed(value, name)) {
      return true;
    }
  }
  for (const Expr *expr : AstWalker::ownExprsIn(stmt)) {
    auto parts = this->heldParts(expr);
    auto isNamed = std::bind_front(&DisposeRules::isNamedAs, this, name);
    bool isHeld = llvm::any_of(parts, isNamed);
    if (isHeld || this->isCaptured(expr, name)) {
      return true;
    }
  }

  return false;
}

bool DisposeRules::isCaptured(const Expr *expr, llvm::StringRef name) const {
  if (dyn_cast<FunctionExpr>(expr) == nullptr) {
    return false;
  }
  auto inner = AstWalker::allExprsIn(expr);
  auto isNamed = std::bind_front(&DisposeRules::isNamedAs, this, name);

  return llvm::any_of(inner, isNamed);
}

std::vector<const Expr *> DisposeRules::heldParts(const Expr *expr) const {
  auto parts = std::vector<const Expr *>();
  const auto *assign = dyn_cast<AssignExpr>(expr);
  const auto *array = dyn_cast<ArrayLitExpr>(expr);
  const auto *object = dyn_cast<ObjectLitExpr>(expr);
  const auto *made = dyn_cast<NewExpr>(expr);
  const auto *yielded = dyn_cast<YieldExpr>(expr);
  if (assign != nullptr) {
    parts.push_back(assign->value);
  }
  if (array != nullptr) {
    llvm::transform(array->elements, std::back_inserter(parts),
                    std::mem_fn(&ArrayLitExpr::Element::expr));
  }
  if (object != nullptr) {
    llvm::transform(object->props, std::back_inserter(parts),
                    std::mem_fn(&ObjectLitExpr::Property::value));
  }
  if (made != nullptr) {
    parts.insert(parts.end(), made->args.begin(), made->args.end());
  }
  if (yielded != nullptr) {
    parts.push_back(yielded->value);
  }

  return parts;
}

}
