#include "compiler/check/scopes/scopes.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>


namespace tz {

Scopes::Scopes(Checker &owner)
    : checker(owner) {}

Scopes::ResultEntryDeclared Scopes::declare(llvm::StringRef name, llvm::SMLoc loc,
                                            ScopeEntry entry) {
  if (name == "undefined") {
    std::string message = ErrorTextDeclare::undefined();

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::UNDEFINED_AS_NAME,
        .message = message,
    });
  }

  auto [it, inserted] = this->checker.scopes_.back().try_emplace(name, entry);
  const ScopeEntry &previous = it->second;
  if (!inserted && entry.func() && previous.func()) {
    std::string message = ErrorTextDeclare::function(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_FUNCTION,
        .message = message,
    });
  }
  if (!inserted) {
    std::string message = ErrorTextDeclare::duplicate(name);

    return err(ScopesDeclareError{
        .code = ScopesDeclareErrorCode::DUPLICATE_DECLARATION,
        .message = message,
    });
  }

  return ok(entry);
}

ScopeEntry Scopes::lookup(llvm::StringRef name) const {
  for (auto scope = this->checker.scopes_.rbegin(); scope != this->checker.scopes_.rend();
       ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }
  return {};
}

VarSymbol *Scopes::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type,
                              bool isConst) {
  VarSymbol *var = this->checker.ctx_.newVar();
  var->name = name;
  var->loc = loc;
  var->type = type;
  var->isConst = isConst;
  if (this->checker.scopes_.size() == 1) {
    bool usedInFunction = this->checker.namesInFunctions_.contains(name);
    var->place = GlobalVar{.usedInFunction = usedInFunction};
  } else {
    unsigned declSeq = ++this->checker.seq_;
    var->place = VarSymbol::localPlace(this->checker.currentDecl(), declSeq);
  }
  ResultEntryDeclared added = this->declare(name, loc, {VarEntry{.var = var}});
  if (added.isErr()) {
    this->checker.diags_.error(loc, added.error().message);
  }
  if (added.isErr() && added.error().code != ScopesDeclareErrorCode::UNDEFINED_AS_NAME) {
    ScopeEntry previousEntry = this->lookup(name);
    std::string declaredHere = ErrorTextDeclare::previous();
    this->checker.diags_.note(previousEntry.loc(), declaredHere);
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
  const Type *type = this->checker.returnInferrer().functionType(fn->sym);
  if (var->function() && fn->sym->ret) {
    var->type = type;
  }

  return type;
}

Scopes::ResultNameIsVisible Scopes::checkVisible(NameExpr *name, const VarSymbol *var) const {
  if (this->checker.otherCaseVars_.contains(var)) {
    std::string message = ErrorTextVisible::otherCase(name->name);

    return err(ScopesCheckVisibleError{
        .code = ScopesCheckVisibleErrorCode::DECLARED_IN_OTHER_CASE,
        .message = message,
    });
  }

  return ok(var);
}

}
