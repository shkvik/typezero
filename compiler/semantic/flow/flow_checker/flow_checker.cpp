#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/program/ast_walker/ast_walker.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/index_proofs/index_proofs.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/library/bytes/byte_proofs/byte_proofs.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"
#include <limits>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

constexpr int64_t kIntMin = std::numeric_limits<int64_t>::min();

constexpr int64_t kIntMax = std::numeric_limits<int64_t>::max();

}

FlowChecker::FlowChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->byteProofs = &all.getByteProofs();
  this->indexProofs = &all.getIndexProofs();
}

void FlowChecker::addLess(Facts &facts, const Linear &x, const Linear &y, bool orEqual) {
  facts.add(x.term, y.term, y.offset - x.offset + (orEqual ? 1 : 0));
}

bool FlowChecker::isArrayLength(const Term &t) {
  return t.length && t.var->type->nonOptional()->isArray();
}

bool FlowChecker::changedByCalls(const VarSymbol *var) const {
  if (var->isConst) {
    return false;
  }
  if (var->isGlobal()) {
    return var->usedInFunction();
  }
  if (var->captured()) {
    return true;
  }

  return this->state.getFunctionStack().isAssignedInGroup(var->name);
}

Flow FlowChecker::join(const Flow &a, const Flow &b) {
  if (a.unreachable) {
    return b;
  }
  if (b.unreachable) {
    return a;
  }
  Flow result;
  result.facts = Facts::meet(a.facts, b.facts);
  for (const VarSymbol *var : a.defined) {
    if (b.defined.contains(var)) {
      result.defined.insert(var);
    }
  }
  for (const FieldPath &path : a.definedFields) {
    if (llvm::is_contained(b.definedFields, path)) {
      result.definedFields.push_back(path);
    }
  }
  for (const auto &narrowed : a.narrowedFields) {
    if (llvm::is_contained(b.narrowedFields, narrowed)) {
      result.narrowedFields.push_back(narrowed);
    }
  }
  for (const auto &narrowed : a.narrowed) {
    if (llvm::is_contained(b.narrowed, narrowed)) {
      result.narrowed.push_back(narrowed);
      continue;
    }
    auto [var, type] = narrowed;
    auto other = llvm::find_if(b.narrowed, [&](const auto &n) { return n.first == var; });
    if (other == b.narrowed.end() || var->type->nonOptional()->isParam() || type->isParam() ||
        other->second->isParam()) {
      continue;
    }
    const Type *either = Type::unionOf({type, other->second});
    if (either->isError() || !UnionSlots::unionProblem(either).empty() || either == var->type) {
      continue;
    }
    if (var->type->isOptional() && either == var->type->inner()) {
      continue;
    }
    result.narrowed.push_back({var, either});
  }
  auto widenWithUndefined = [&](const Flow &empty, const Flow &other) {
    for (const VarSymbol *var : empty.undefinedVars) {
      auto narrowed = llvm::find_if(other.narrowed, [&](const auto &n) { return n.first == var; });
      if (narrowed == other.narrowed.end() || var->type->nonOptional()->isParam() ||
          var->type->nonOptional()->isJson() || narrowed->second->isParam() ||
          llvm::any_of(result.narrowed, [&](const auto &n) { return n.first == var; })) {
        continue;
      }
      const Type *either = Type::unionOf({narrowed->second, Type::undefinedTy()});
      if (either->isError() || !UnionSlots::unionProblem(either).empty() || either == var->type) {
        continue;
      }
      result.narrowed.push_back({var, either});
    }
  };
  widenWithUndefined(a, b);
  widenWithUndefined(b, a);
  for (const VarSymbol *var : a.undefinedVars) {
    if (b.undefinedVars.contains(var)) {
      result.undefinedVars.insert(var);
    }
  }
  for (const VarSymbol *var : a.okResults) {
    if (b.okResults.contains(var)) {
      result.okResults.insert(var);
    }
  }
  for (const VarSymbol *var : a.errResults) {
    if (b.errResults.contains(var)) {
      result.errResults.insert(var);
    }
  }
  for (llvm::StringRef field : a.setFields) {
    if (llvm::is_contained(b.setFields, field)) {
      result.setFields.push_back(field);
    }
  }
  for (const EmitterKey &key : a.errorListened) {
    if (llvm::is_contained(b.errorListened, key)) {
      result.errorListened.push_back(key);
    }
  }
  result.errorPending = a.errorPending;
  for (const auto &pending : b.errorPending) {
    if (llvm::none_of(result.errorPending,
                      [&](const auto &p) { return p.first == pending.first; })) {
      result.errorPending.push_back(pending);
    }
  }
  result.beforeSuper = a.beforeSuper || b.beforeSuper;

  return result;
}

void FlowChecker::setDefined(const VarSymbol *var, bool defined) {
  if (defined) {
    this->state.getFlowState().markDefined(var);
  } else {
    this->state.getFlowState().forgetDefined(var);
  }
}

void FlowChecker::assigned(const VarSymbol *var, const Expr *value) {
  const Type *type = var->type;
  if (type->isError()) {
    return;
  }
  Flow flow = this->state.getFlowState().takeFlow();
  FieldFacts::forgetFieldsOf(flow, var);
  flow.undefinedVars.erase(var);
  llvm::erase_if(flow.narrowed, [&](const auto &narrowed) { return narrowed.first == var; });
  if (const Type *base = type->nonOptional();
      base->isUnion() && value->type && !value->type->isError() && value->type != type &&
      TypeRelations::isImplicitlyConvertible(value->type, base)) {
    UnionNarrowing::narrowVariable(flow, var, value->type);
  }
  this->state.getFlowState().replaceFlow(std::move(flow));
  if (type->isOptional()) {
    this->setDefined(var, !value->type->isOptional() && !value->type->isUndefined());
  }
  if (type->isResult()) {
    const Type *given = value->type;
    this->state.getFlowState().forgetResultState(var);
    if (given->isResult() && given->errType()->isNever()) {
      this->state.getFlowState().markOkResult(var);
    } else if (given->isResult() && given->okType()->isNever()) {
      this->state.getFlowState().markErrResult(var);
    }
    return;
  }

  if (type->isInteger() && !type->isWrapping()) {
    std::optional<Linear> v = this->indexProofs->linear(value);
    if (v && v->term == Term::of(var)) {
      this->incremented(var, v->offset);
      return;
    }
    IntegerRange range = v ? IntegerRange{} : this->byteProofs->valueRange(value);
    this->state.getFlowState().getFacts().forget(var);
    if (v) {
      this->state.getFlowState().getFacts().addEqual(Term::of(var), v->term, v->offset);
    }
    if (range.lo) {
      this->state.getFlowState().getFacts().add(Term::zero(), Term::of(var),
                                                Facts::saturatingSub(1, *range.lo));
    }
    if (range.hi) {
      this->state.getFlowState().getFacts().add(Term::of(var), Term::zero(),
                                                Facts::saturatingAdd(*range.hi, 1));
    }
    if (auto *call = dyn_cast<CallExpr>(value);
        call && (call->builtin() == Builtin::MathMin || call->builtin() == Builtin::MathMax) &&
        !call->hasSpread()) {
      for (const Expr *arg : call->args) {
        if (std::optional<Linear> bound = this->indexProofs->linear(arg)) {
          if (call->builtin() == Builtin::MathMin) {
            FlowChecker::addLess(this->state.getFlowState().getFacts(), Linear{Term::of(var), 0},
                                 *bound, true);
          } else {
            FlowChecker::addLess(this->state.getFlowState().getFacts(), *bound,
                                 Linear{Term::of(var), 0}, true);
          }
        }
      }
    }
    return;
  }

  this->state.getFlowState().getFacts().forget(var);
  const Type *base = type->nonOptional();
  if (!base->isString() && !base->isArray() && !base->isByteView() &&
      base->bytes() != Type::Bytes::ArrayBuffer) {
    return;
  }
  if (value->type->isOptional() || value->type->isUndefined()) {
    return;
  }
  if (std::optional<Linear> length = this->indexProofs->knownLength(value)) {
    if (length->term != Term::lengthOf(var)) {
      this->state.getFlowState().getFacts().addEqual(Term::lengthOf(var), length->term,
                                                     length->offset);
    }
    return;
  }
  IntegerRange range = this->indexProofs->lengthRange(value);
  if (range.lo) {
    this->state.getFlowState().getFacts().add(Term::zero(), Term::lengthOf(var),
                                              Facts::saturatingSub(1, *range.lo));
  }
  if (range.hi) {
    this->state.getFlowState().getFacts().add(Term::lengthOf(var), Term::zero(),
                                              Facts::saturatingAdd(*range.hi, 1));
  }
}

void FlowChecker::incremented(const VarSymbol *var, int64_t delta) {
  if (!var->type->isInt()) {
    return;
  }
  Term t = Term::of(var);
  bool safe =
      delta == 0 || (delta > 0 ? this->state.getFlowState().getFacts().proves(t, Term::zero(),
                                                                              kIntMax - (delta - 1))
                               : this->state.getFlowState().getFacts().proves(
                                     Term::zero(), t, Facts::saturatingSub(delta + 1, kIntMin)));
  if (safe) {
    this->state.getFlowState().getFacts().shift(t, delta);
  } else {
    this->state.getFlowState().getFacts().forget(var);
  }
}

bool FlowChecker::staysLocal(const FunctionDecl *fn, llvm::StringRef name) {
  if (!fn || !fn->body) {
    return false;
  }
  auto named = [&](const Expr *e) {
    auto *n = dyn_cast_or_null<NameExpr>(e);

    return n && n->name == name;
  };
  size_t all = 0;
  size_t safe = 0;
  AstWalker::visitExprs(fn->body, [&](const Expr *e) { all += named(e) ? 1 : 0; });
  AstWalker::visitExprs(
      fn->body,
      [&](const Expr *e) {
        if (auto *index = dyn_cast<IndexExpr>(e)) {
          safe += named(index->object) ? 1 : 0;
        } else if (auto *member = dyn_cast<MemberExpr>(e)) {
          safe += named(member->object) &&
                          BuiltinPropertyTable::isName(member->name, BuiltinProperty::LENGTH)
                      ? 1
                      : 0;
        } else if (auto *list = dyn_cast<ArrayLitExpr>(e)) {
          for (const ArrayLitExpr::Element &el : list->elements) {
            safe += el.spread && named(el.expr) ? 1 : 0;
          }
        } else if (auto *call = dyn_cast<CallExpr>(e)) {
          for (size_t i = 0; i < call->args.size(); ++i) {
            safe += call->spreads(i) && named(call->args[i]) ? 1 : 0;
          }
          auto *callee = dyn_cast<MemberExpr>(call->callee);
          if (!callee || !named(callee->object)) {
            return;
          }
          const ArrayMethodInfo *method = ArrayMethodTable::find(callee->name);
          if (method == nullptr) {
            return;
          }
          const CallbackArrayMethod *callback = method->form.as<CallbackArrayMethod>();
          bool reduce = callback != nullptr && callback->callback == ArrayCallback::REDUCE;
          bool calls = callback != nullptr || method->form.is<SortingArrayMethod>();
          if (calls && !llvm::all_of(call->args, [&](const Expr *arg) {
                auto *fe = dyn_cast<FunctionExpr>(arg);
                return !fe ? !reduce || arg != call->args.front()
                           : fe->fn->params.size() < (reduce ? 4u : 3u);
              })) {
            return;
          }
          if (calls && !reduce && !call->args.empty() && !isa<FunctionExpr>(call->args.front())) {
            return;
          }
          ++safe;
        }
      },
      false);
  std::function<void(const Stmt *)> loops = [&](const Stmt *s) {
    if (!s) {
      return;
    }
    switch (s->kind()) {
    case Stmt::Kind::Block:
      for (const Stmt *inner : cast<BlockStmt>(s)->body) {
        loops(inner);
      }
      break;
    case Stmt::Kind::If:
      loops(cast<IfStmt>(s)->thenStmt);
      loops(cast<IfStmt>(s)->elseStmt);
      break;
    case Stmt::Kind::While:
      loops(cast<WhileStmt>(s)->body);
      break;
    case Stmt::Kind::DoWhile:
      loops(cast<DoWhileStmt>(s)->body);
      break;
    case Stmt::Kind::For:
      loops(cast<ForStmt>(s)->body);
      break;
    case Stmt::Kind::ForOf:
      safe += named(cast<ForOfStmt>(s)->iterable) ? 1 : 0;
      loops(cast<ForOfStmt>(s)->body);
      break;
    case Stmt::Kind::Switch:
      for (const SwitchCase &c : cast<SwitchStmt>(s)->cases) {
        for (const Stmt *inner : c.body) {
          loops(inner);
        }
      }
      break;
    case Stmt::Kind::Labeled:
      loops(cast<LabeledStmt>(s)->body);
      break;
    case Stmt::Kind::Return: {
      const Expr *value = cast<ReturnStmt>(s)->value;
      auto *wrap = dyn_cast_or_null<CallExpr>(value);
      auto *callee = wrap ? dyn_cast<NameExpr>(wrap->callee) : nullptr;
      std::optional<GlobalName> wrapper = std::nullopt;
      if (callee != nullptr) {
        wrapper = GlobalNameTable::find(callee->name);
      }
      bool wraps = wrapper == GlobalName::OK || wrapper == GlobalName::ERR;
      if (wraps && wrap->args.size() == 1) {
        value = wrap->args[0];
      }
      safe += named(value) ? 1 : 0;
      break;
    }
    default:
      break;
    }
  };
  loops(fn->body);

  return safe == all;
}

void FlowChecker::forgetCallEffects(Flow &flow) const {
  auto part = [&](const Term &t) {
    return t.var &&
           ((FlowChecker::isArrayLength(t) && !t.var->unshared()) || this->changedByCalls(t.var));
  };
  auto changed = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  flow.facts.forgetIf([&](const Facts::Fact &f) { return changed(f.lhs) || changed(f.rhs); });
  for (const VarSymbol *var : llvm::to_vector(flow.defined)) {
    if (this->changedByCalls(var)) {
      flow.defined.erase(var);
    }
  }
  for (const VarSymbol *var : llvm::to_vector(flow.undefinedVars)) {
    if (this->changedByCalls(var)) {
      flow.undefinedVars.erase(var);
    }
  }
  llvm::erase_if(flow.narrowed,
                 [&](const auto &narrowed) { return this->changedByCalls(narrowed.first); });
  for (auto *results : {&flow.okResults, &flow.errResults}) {
    for (const VarSymbol *var : llvm::to_vector(*results)) {
      if (this->changedByCalls(var)) {
        results->erase(var);
      }
    }
  }
  auto fieldChanged = [&](const FieldPath &path) {
    return this->changedByCalls(path.var) || llvm::any_of(path.names, [&](llvm::StringRef field) {
             return this->state.getFlowState().isFieldAssigned(field);
           });
  };
  llvm::erase_if(flow.definedFields, fieldChanged);
  llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return fieldChanged(entry.first); });
}

void FlowChecker::calledFunction() {
  Flow flow = this->state.getFlowState().takeFlow();
  this->forgetCallEffects(flow);
  this->state.getFlowState().replaceFlow(std::move(flow));
}

void FlowChecker::arrayResized(const Type *type, bool grew, bool shrank, const VarSymbol *except) {
  type = type->nonOptional();
  auto part = [&](const Term &t) {
    return t.length && t.var != except && t.var->type->nonOptional() == type;
  };
  auto affected = [&](const Term &t) { return part(t.first()) || part(t.second()); };
  this->state.getFlowState().getFacts().forgetIf([&](const Facts::Fact &f) {
    return (grew && affected(f.lhs)) || (shrank && affected(f.rhs));
  });
}

}
