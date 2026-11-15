#include "compiler/semantic/library/result/library_result.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;

namespace tz {

LibraryResult::LibraryResult(SemanticState &owner)
    : state(owner) {}

std::optional<GlobalName> LibraryResult::resultName(llvm::StringRef name) const {
  auto entry = this->state.walk.scopes.findName(name);
  if (entry.isFound()) {
    return std::nullopt;
  }
  static const GlobalName resultNames[] = {GlobalName::OK, GlobalName::ERR, GlobalName::RESULT,
                                           GlobalName::OK_TYPE, GlobalName::ERR_TYPE};
  auto global = GlobalNameTable::find(name);
  if (!global.has_value() || !llvm::is_contained(resultNames, *global)) {
    return std::nullopt;
  }

  return global;
}

void LibraryResult::addResultFacts(const Expr *cond, Flow &whenTrue, Flow &whenFalse) const {
  auto *call = dyn_cast<CallExpr>(cond);
  if (call == nullptr) {
    return;
  }
  Builtin builtin = call->builtin();
  if (builtin != Builtin::ResultIsOk && builtin != Builtin::ResultIsErr) {
    return;
  }
  const VarSymbol *tested = this->testedResult(call);
  if (tested == nullptr) {
    return;
  }
  if (builtin == Builtin::ResultIsOk) {
    return this->splitResultFlows(tested, whenTrue, whenFalse);
  }

  return this->splitResultFlows(tested, whenFalse, whenTrue);
}

const VarSymbol *LibraryResult::testedResult(const CallExpr *test) const {
  const Expr *receiver = cast<MemberExpr>(test->callee)->object;
  const auto *bare = ExprParts::withoutImplicitCasts(receiver);
  auto *name = dyn_cast<NameExpr>(bare);
  if (name == nullptr || name->var() == nullptr || name->var()->type == nullptr) {
    return nullptr;
  }
  if (!name->var()->type->nonOptional()->isResult()) {
    return nullptr;
  }

  return name->var();
}

void LibraryResult::splitResultFlows(const VarSymbol *tested, Flow &whenOk, Flow &whenErr) const {
  whenOk.okResults.insert(tested);
  whenOk.errResults.erase(tested);
  whenErr.errResults.insert(tested);
  whenErr.okResults.erase(tested);
}

const Type *LibraryResult::unifyHalf(const Type *first, const Type *second) {
  return this->state.types.relations.unifyAs(UnifyForm::HALF, first, second);
}

}
