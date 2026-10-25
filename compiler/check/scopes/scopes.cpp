#include "compiler/check/scopes/scopes.h"
#include "compiler/check/errors/throw_checker/throw_checker.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/ErrorHandling.h"
#include <cmath>
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

Scopes::Scopes(Checker &checker) : checker_(checker) {}

void Scopes::declare(llvm::StringRef name, llvm::SMLoc loc, Checker::ScopeEntry entry) {
  if (name == "undefined") {
    checker_.diags_.error(loc, "'undefined' cannot be used as a name");
    return;
  }
  auto [it, inserted] = checker_.scopes_.back().try_emplace(name, entry);
  if (inserted) {
    return;
  }
  const Checker::ScopeEntry &previous = it->second;
  // Two functions with one name are not overloads: plain functions have none
  // (docs/overloads.md).
  checker_.diags_.error(
      loc, "duplicate declaration of '" + name + "'" +
               (entry.func && previous.func
                    ? "; plain functions are not overloaded: give each variant its own name"
                    : ""));
  checker_.diags_.note(previous.var        ? previous.var->loc
                       : previous.func     ? previous.func->loc
                       : previous.enumDecl ? previous.enumDecl->loc()
                                           : previous.cls->loc(),
                       "previously declared here");
}

Checker::ScopeEntry Scopes::lookup(llvm::StringRef name) const {
  for (auto scope = checker_.scopes_.rbegin(); scope != checker_.scopes_.rend(); ++scope) {
    auto found = scope->find(name);
    if (found != scope->end()) {
      return found->second;
    }
  }
  // Error and its kinds, which the program's own names hide.
  if (ClassDecl *kind = checker_.throwChecker().errorKindClass(name)) {
    return {nullptr, nullptr, nullptr, kind};
  }

  return {};
}

VarSymbol *Scopes::declareVar(llvm::StringRef name, llvm::SMLoc loc, const Type *type,
                              bool isConst) {
  VarSymbol *var = checker_.ctx_.newVar();
  var->name = name;
  var->loc = loc;
  var->type = type;
  var->isConst = isConst;
  var->isGlobal = checker_.scopes_.size() == 1;
  var->usedInFunction = var->isGlobal && checker_.namesInFunctions_.contains(name);
  var->owner = var->isGlobal ? nullptr : checker_.currentDecl();
  var->declSeq = ++checker_.seq_;
  declare(name, loc, {var, nullptr});

  return var;
}

const Type *Scopes::varType(VarSymbol *var) {
  if (var->type) {
    return var->type;
  }
  // A function's variable, while its type is being worked out.
  FunctionDecl *fn = var->function ? var->function : var->lambda;
  if (!fn || !fn->sym) {
    return Type::error();
  }
  const Type *type = checker_.returnInferrer().functionType(fn->sym);
  if (var->function && fn->sym->ret) {
    var->type = type;
  }

  return type;
}

void Scopes::checkVisible(NameExpr *name, const VarSymbol *var) {
  if (checker_.otherCaseVars_.contains(var)) {
    checker_.diags_.error(name->loc(),
                          "'" + name->name +
                              "' is declared in an earlier case of the 'switch'; declare it "
                              "before the 'switch' to use it in several cases");
  }
}

} // namespace tz
