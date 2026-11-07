#include "compiler/check/scopes/scopes.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"
#include <cmath>


namespace tz {

Scopes::Scopes(Checker &owner)
    : checker(owner) {}

void Scopes::declare(llvm::StringRef name, llvm::SMLoc loc, ScopeEntry entry) {
  if (name == "undefined") {
    this->checker.diags_.error(loc, "'undefined' cannot be used as a name");
    return;
  }
  auto [it, inserted] = this->checker.scopes_.back().try_emplace(name, entry);
  if (inserted) {
    return;
  }
  const ScopeEntry &previous = it->second;
  this->checker.diags_.error(
      loc, "duplicate declaration of '" + name + "'" +
               (entry.func() && previous.func()
                    ? "; plain functions are not overloaded: give each variant its own name"
                    : ""));
  this->checker.diags_.note(previous.loc(), "previously declared here");
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
  this->declare(name, loc, {VarEntry{.var = var}});

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

void Scopes::checkVisible(NameExpr *name, const VarSymbol *var) {
  if (this->checker.otherCaseVars_.contains(var)) {
    this->checker.diags_.error(name->loc(),
                               "'" + name->name +
                                   "' is declared in an earlier case of the 'switch'; declare it "
                                   "before the 'switch' to use it in several cases");
  }
}

}
