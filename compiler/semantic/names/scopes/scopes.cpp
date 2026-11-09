#include "compiler/semantic/names/scopes/scopes.h"
#include "compiler/semantic/bodies/stmt/var_checker/var_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>

namespace tz {

Scopes::Scopes(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->varChecker = &all.getVarChecker();
  this->returnInferrer = &all.getReturnInferrer();
}

Scopes::ResultEntryDeclared Scopes::declare(llvm::StringRef name, ScopeEntry entry) {
  if (name == "undefined") {
    std::string message = ErrorTextDeclare::undefined();

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::UNDEFINED_AS_NAME,
        .message = message,
    });
  }

  std::optional<ScopeEntry> previous = this->state.getScopeStack().findInCurrentScope(name);
  if (!previous) {
    this->state.getScopeStack().addToCurrentScope(name, entry);
  }
  if (previous && entry.func() && previous->func()) {
    std::string message = ErrorTextDeclare::function(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_FUNCTION,
        .message = message,
    });
  }
  if (previous) {
    std::string message = ErrorTextDeclare::duplicate(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_DECLARATION,
        .message = message,
    });
  }

  return ok(entry);
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

VarSymbol *Scopes::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type,
                              bool isConst) {
  VarSymbol *var = this->state.getAst().newVar();
  var->name = name;
  var->loc = loc;
  var->type = type;
  var->isConst = isConst;
  if (this->state.getScopeStack().isTopLevel()) {
    bool usedInFunction = this->state.getScopeStack().isUsedInFunction(name);
    var->place = GlobalVar{.usedInFunction = usedInFunction};
  } else {
    unsigned declSeq = this->state.getReturnInference().takeSequenceNumber();
    var->place = VarSymbol::localPlace(this->state.getFunctionStack().getCurrentDecl(), declSeq);
  }
  ResultEntryDeclared added = this->declare(name, {VarEntry{.var = var}});
  if (added.isErr()) {
    this->state.getDiagnostics().error(loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->lookup(name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->state.getDiagnostics().note(previousEntry.loc(), declaredHere);
  }

  return var;
}

const Type *Scopes::varType(VarSymbol *var) {
  if (var->type) {
    return var->type;
  }
  FunctionDecl *fn = var->function() ? var->function() : var->lambda();
  if (!fn || !fn->sym) {
    return Type::error();
  }
  const Type *type = this->returnInferrer->functionType(fn->sym);
  if (var->function() && fn->sym->hasReturnType()) {
    var->type = type;
  }

  return type;
}

Scopes::ResultNameIsVisible Scopes::checkVisible(NameExpr *name, const VarSymbol *var) const {
  if (this->state.getBlockPosition().isOtherCaseVar(var)) {
    std::string message = ErrorTextVisible::otherCase(name->name);

    return err(ScopesCheckVisibleError{
        .code = ScopesCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(var);
}

}
