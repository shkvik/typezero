#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

Scopes::Scopes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->varChecker = &all.varChecker;
  this->returnInferrer = &all.returnInferrer;
}

Scopes::ResultEntryDeclared Scopes::declare(llvm::StringRef name, ScopeEntry entry) {
  auto allowed = this->checkNameAllowed(name);
  if (allowed.isErr()) {
    return err(allowed.error());
  }
  auto function = this->checkFunctionOnce(name, entry);
  if (function.isErr()) {
    return err(function.error());
  }
  auto unique = this->checkNotDeclared(name);
  if (unique.isErr()) {
    return err(unique.error());
  }
  this->state.getScopeStack().addToCurrentScope(name, entry);

  return ok(entry);
}

Scopes::ResultNameIsAllowed Scopes::checkNameAllowed(llvm::StringRef name) const {
  if (TypeNameTable::isName(name, TypeName::UNDEFINED)) {
    auto message = ErrorTextDeclare::undefined();

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::UNDEFINED_AS_NAME,
        .message = message,
    });
  }

  return ok();
}

Scopes::ResultNameIsAllowed Scopes::checkFunctionOnce(llvm::StringRef name,
                                                      const ScopeEntry &entry) const {
  auto previous = this->state.getScopeStack().findInCurrentScope(name);
  bool isSecondFunction =
      previous.has_value() && entry.func() != nullptr && previous->func() != nullptr;
  if (isSecondFunction) {
    auto message = ErrorTextDeclare::function(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_FUNCTION,
        .message = message,
    });
  }

  return ok();
}

Scopes::ResultNameIsAllowed Scopes::checkNotDeclared(llvm::StringRef name) const {
  auto previous = this->state.getScopeStack().findInCurrentScope(name);
  if (previous.has_value()) {
    auto message = ErrorTextDeclare::duplicate(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_DECLARATION,
        .message = message,
    });
  }

  return ok();
}

ScopeEntry Scopes::lookup(llvm::StringRef name) const {
  return this->state.getScopeStack().findName(name);
}

void Scopes::closeScope() {
  if (this->state.getExpressionMarks().hasUntyped()) {
    this->varChecker->reportUntyped();
  }
  this->state.getScopeStack().closeScope();
}

void Scopes::openScope() {
  this->state.getScopeStack().openScope();
}

VarSymbol *Scopes::declareConst(llvm::StringRef name, llvm::SMLoc loc, const Type *type) {
  bool isConst = true;

  return this->declareVar(name, loc, type, isConst);
}

VarSymbol *Scopes::declareLet(llvm::StringRef name, llvm::SMLoc loc, const Type *type) {
  bool isConst = false;

  return this->declareVar(name, loc, type, isConst);
}

VarSymbol *Scopes::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type,
                              bool isConst) {
  auto *variable = this->state.getAst().newVar();
  variable->name = name;
  variable->loc = loc;
  variable->type = type;
  variable->isConst = isConst;
  this->placeVariable(variable);
  auto added = this->declare(name, {VarEntry{.var = variable}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    auto previousEntry = this->lookup(name);
    auto declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }

  return variable;
}

void Scopes::placeVariable(VarSymbol *variable) {
  if (this->state.getScopeStack().isTopLevel()) {
    bool isUsedInFunction = this->state.getScopeStack().isUsedInFunction(variable->name);
    variable->place = GlobalVar{.usedInFunction = isUsedInFunction};
    return;
  }
  unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
  auto *currentDecl = this->state.getFunctionStack().getCurrentDecl();
  variable->place = VarSymbol::localPlace(currentDecl, declSeq);
}

const Type *Scopes::varType(VarSymbol *variable) {
  if (variable->type != nullptr) {
    return variable->type;
  }
  FunctionDecl *functionDecl =
      variable->function() != nullptr ? variable->function() : variable->lambda();
  if (functionDecl == nullptr || functionDecl->sym == nullptr) {
    return Type::error();
  }
  const auto *type = this->returnInferrer->functionType(functionDecl->sym);
  if (variable->function() != nullptr && functionDecl->sym->hasReturnType()) {
    variable->type = type;
  }

  return type;
}

Scopes::ResultNameIsVisible Scopes::checkVisible(NameExpr *name, const VarSymbol *variable) const {
  if (this->state.getBlockPosition().isOtherCaseVar(variable)) {
    auto message = ErrorTextVisible::otherCase(name->name);

    return err(ScopesCheckVisibleError{
        .code = ScopesCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(variable);
}

}
