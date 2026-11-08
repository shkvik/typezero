#include "compiler/semantic/flow/predicate_checker/predicate_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker.h"
#include "compiler/semantic/bodies/functions/function_checker/function_checker_errors.h"
#include "compiler/semantic/flow/field_facts/field_facts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/flow/union_narrowing/union_narrowing.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/types/literal_types/literal_types.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "compiler/semantic/types/type_resolver/type_resolver.h"
#include "llvm/ADT/StringExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

const VarSymbol *PredicateChecker::argumentVariable(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit() || conv->shortCircuit) {
      return nullptr;
    }
    expr = conv->operand;
  }
  auto *name = dyn_cast<NameExpr>(expr);

  return name ? name->var() : nullptr;
}

std::optional<Linear> PredicateChecker::argumentTerm(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      return std::nullopt;
    }
    expr = conv->operand;
  }
  bool negated = false;
  if (auto *unary = dyn_cast<UnaryExpr>(expr); unary && unary->op == UnaryOp::Neg) {
    negated = true;
    expr = unary->operand;
  }
  if (auto *lit = dyn_cast<IntLitExpr>(expr); lit && lit->value < (uint64_t(1) << 31)) {
    return Linear{Term::zero(), negated ? -int64_t(lit->value) : int64_t(lit->value)};
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var() && !negated) {
    return Linear{Term::of(name->var()), 0};
  }

  return std::nullopt;
}

PredicateChecker::PredicateChecker(SemanticState &owner)
    : state(owner) {}

PredicateChecker::ResultNotAsserting PredicateChecker::notAsserting(const FunctionDecl *fn,
                                                                    const TypeRef &ref) {
  if (ref.asserts()) {
    const Type *type =
        ref.args.empty() ? nullptr : TypeResolver(this->state).resolveType(ref.args[0], false);
    std::string args;
    for (const Param &p : fn->params) {
      args += (args.empty() ? "" : ", ") + p.name.str();
    }
    std::string call = fn->name.str() + "(" + args + ")";
    if (fn->takesThis()) {
      std::string object = fn->cls()->name.str();
      object[0] = llvm::toLower(object[0]);
      call = object + "." + call;
    }
    std::string shown = ref.name.str() + (type ? " is " + type->name().str() : "");
    std::string message = ErrorTextAsserts::notAllowed(shown, type != nullptr, call);

    return err(PredicateCheckerNotAssertingError{
        .code = PredicateCheckerNotAssertingErrorCode::ASSERTS_PREDICATE,
        .message = message,
    });
  }

  return ok(&ref);
}

PredicateChecker::ResultPredicateArg PredicateChecker::predicateArg(const FunctionDecl *fn,
                                                                    const TypeRef &ref) {
  bool method = fn->cls() && !fn->isStatic() && !fn->isConstructor();
  if (ref.name == "this" && !method) {
    std::string message = ErrorTextPredicateArg::thisOutside();

    return err(PredicateCheckerPredicateArgError{
        .code = PredicateCheckerPredicateArgErrorCode::THIS_OUTSIDE_METHOD,
        .message = message,
    });
  }
  if (ref.name == "this") {
    return ok(-1);
  }

  int param = -2;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    if (fn->params[i].name == ref.name) {
      param = int(i);
    }
  }
  if (param == -2) {
    std::string message = ErrorTextPredicateArg::notParam(ref.name);

    return err(PredicateCheckerPredicateArgError{
        .code = PredicateCheckerPredicateArgErrorCode::NOT_A_PARAMETER,
        .message = message,
    });
  }

  return ok(param);
}

const Type *PredicateChecker::resolveReturnType(FunctionDecl *fn, FuncSymbol *sym) {
  const TypeRef &ref = *fn->returnType;
  if (ref.kind() != TypeRefKind::PREDICATE_REF) {
    return TypeResolver(this->state).resolveType(ref, true);
  }
  if (sym) {
    ResultNotAsserting plain = this->notAsserting(fn, ref);
    if (plain.isErr()) {
      this->state.getDiagnostics().error(ref.loc, plain.error().message);
    }
  }
  if (ref.asserts()) {
    return Type::voidTy();
  }
  ResultPredicateArg named = PredicateChecker::predicateArg(fn, ref);
  if (named.isErr() && sym) {
    this->state.getDiagnostics().error(ref.loc, named.error().message);
  }
  int param = named.isOk() ? named.value() : -2;
  const Type *type =
      ref.args.empty() ? nullptr : TypeResolver(this->state).resolveType(ref.args[0], false);
  if (sym && param != -2 && type && !type->isError()) {
    sym->predicate = TypePredicate{.param = param, .type = type};
  }

  return Type::boolTy();
}

bool PredicateChecker::looksBoolean(const Expr *expr) {
  if (isa<BoolLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return unary->op == UnaryOp::Not;
  }
  auto *bin = dyn_cast<BinaryExpr>(expr);
  if (!bin) {
    return false;
  }
  switch (bin->op) {
  case BinaryOp::Eq:
  case BinaryOp::Ne:
  case BinaryOp::Lt:
  case BinaryOp::Le:
  case BinaryOp::Gt:
  case BinaryOp::Ge:
  case BinaryOp::InstanceOf:
  case BinaryOp::In:
    return true;
  case BinaryOp::LogAnd:
  case BinaryOp::LogOr:
    return PredicateChecker::looksBoolean(bin->lhs) && PredicateChecker::looksBoolean(bin->rhs);
  default:
    return false;
  }
}

void PredicateChecker::beginPredicate(FunctionDecl *fn) {
  const FuncSymbol *sym = fn->sym;
  bool boolean = !sym->hasReturnType() || sym->getKnownReturnType()->isBool();
  if (!boolean || fn->isGenerator || fn->isConstructor()) {
    this->state.getPredicateStore().enterPredicateBuild({nullptr, fn, Flow(), Flow()});
    return;
  }
  PredicateBuild build{sym, fn, Flow(), Flow()};
  build.whenTrue.unreachable = true;
  build.whenFalse.unreachable = true;
  this->state.getPredicateStore().enterPredicateBuild(std::move(build));
}

PredicateChecker::ResultExitNarrowed PredicateChecker::notePredicateExit(const Flow &whenTrue,
                                                                         const Flow &whenFalse) {
  PredicateStore &predicates = this->state.getPredicateStore();
  if (!predicates.isBuildingPredicate()) {
    return ok(&whenTrue);
  }
  const PredicateBuild &build = predicates.getPredicateBuild();
  const FuncSymbol *sym = build.fn;
  const FunctionDecl *decl = build.decl;
  Flow joinedTrue = FlowChecker::join(build.whenTrue, whenTrue);
  Flow joinedFalse = FlowChecker::join(build.whenFalse, whenFalse);
  predicates.updatePredicateBuild(std::move(joinedTrue), std::move(joinedFalse));
  if (!sym->predicate || whenTrue.unreachable) {
    return ok(&whenTrue);
  }
  const TypePredicate &predicate = *sym->predicate;
  const VarSymbol *var = predicate.param == -1 ? decl->thisVar()
                         : size_t(predicate.param) < decl->params.size()
                             ? decl->params[size_t(predicate.param)].sym
                             : nullptr;
  if (!var) {
    return ok(&whenTrue);
  }
  const Type *shown = LiteralTypes(this->state).typeIn(whenTrue, var);
  bool narrowed = shown->isError() || shown->isNever() ||
                  TypeRelations::isImplicitlyConvertible(shown, predicate.type);
  if (!narrowed) {
    std::string function = ErrorTextFunctionName::label(sym->name);
    std::string message =
        ErrorTextPredicateExit::mayLie(function, var->name, predicate.type->name(), shown->name());

    return err(PredicateCheckerNotePredicateExitError{
        .code = PredicateCheckerNotePredicateExitErrorCode::NOT_NARROWED,
        .message = message,
    });
  }

  return ok(&whenTrue);
}

void PredicateChecker::endPredicate(FunctionDecl *fn) {
  PredicateBuild build = this->state.getPredicateStore().leavePredicateBuild();
  if (!build.fn) {
    return;
  }
  Predicate summary;
  auto position = [&](const VarSymbol *var) -> int {
    if (!var || var->reassigned) {
      return -2;
    }
    if (var == fn->thisVar()) {
      return -1;
    }
    for (size_t i = 0; i < fn->params.size(); ++i) {
      if (fn->params[i].sym == var) {
        return int(i);
      }
    }

    return -2;
  };
  auto project = [&](const Flow &flow, PredicateSide &side) {
    side.unreachable = flow.unreachable;
    if (flow.unreachable) {
      return;
    }
    for (const auto &[var, type] : flow.narrowed) {
      if (int i = position(var); i != -2) {
        side.narrowed.push_back({i, type});
      }
    }
    for (const VarSymbol *var : flow.defined) {
      if (int i = position(var); i != -2) {
        side.defined.push_back(i);
      }
    }
    for (const FieldPath &path : flow.definedFields) {
      if (int i = position(path.var); i != -2) {
        side.fields.push_back({i, path.names});
      }
    }
    for (const auto &[path, type] : flow.narrowedFields) {
      if (int i = position(path.var); i != -2) {
        side.narrowedFields.push_back({i, path.names, type});
      }
    }
    for (const Facts::Fact &fact : flow.facts.all()) {
      auto param = [&](const Term &t) {
        return t.isZero() || (!t.isSum() && position(t.var) != -2);
      };
      if (param(fact.lhs) && param(fact.rhs)) {
        side.facts.push_back(fact);
      }
    }
  };
  project(build.whenTrue, summary.whenTrue);
  project(build.whenFalse, summary.whenFalse);
  for (const Param &p : fn->params) {
    summary.params.push_back(p.sym);
  }
  summary.self = fn->thisVar();
  this->state.getPredicateStore().rememberPredicate(build.fn, std::move(summary));
}

const Predicate *PredicateChecker::predicateOf(const FuncSymbol *fn) const {
  PredicateStore &predicates = this->state.getPredicateStore();
  if (const Predicate *known = predicates.findPredicate(fn)) {
    return known;
  }
  FunctionDecl *decl = fn->decl;
  if (!decl || decl->checked || !decl->body || decl->external() || decl->isGenerator ||
      decl->isInstance || (fn->hasReturnType() && !fn->getKnownReturnType()->isBool()) ||
      predicates.isSummarizing(fn)) {
    return nullptr;
  }
  predicates.markSummarizing(fn);
  BodyState saved = ReturnInferrer(this->state).enterBody(decl);
  bool muted = this->state.getDiagnostics().setMuted(true);
  FunctionDecl *copy = ReturnInferrer(this->state).freshCopy(decl);
  FunctionChecker(this->state).checkFunctionBody(copy);
  this->state.getDiagnostics().setMuted(muted);
  ReturnInferrer(this->state).leaveBody(saved);
  return predicates.findPredicate(fn);
}

void PredicateChecker::applyPredicate(const CallExpr *call, Flow &whenTrue, Flow &whenFalse) const {
  const FuncSymbol *fn = call->func() ? call->func() : call->method();
  if (!fn || call->virtualCall() || !call->unionMethods().empty()) {
    return;
  }
  auto *member = dyn_cast<MemberExpr>(call->callee);
  auto argument = [&](int i) -> const Expr * {
    if (i == -1) {
      return call->method() && member ? member->object : nullptr;
    }

    return size_t(i) < call->args.size() ? call->args[size_t(i)] : nullptr;
  };
  auto variable = [&](int i) -> const VarSymbol * {
    const Expr *arg = argument(i);
    const VarSymbol *var = arg ? PredicateChecker::argumentVariable(arg) : nullptr;

    return var && !FlowChecker(this->state).changedByCalls(var) ? var : nullptr;
  };
  auto narrowTo = [&](Flow &flow, const VarSymbol *var, const Type *type) {
    const Type *now = LiteralTypes(this->state).typeIn(flow, var);
    if (TypeRelations::isImplicitlyConvertible(now, type)) {
      return;
    }
    if (TypeRelations::isImplicitlyConvertible(type, now)) {
      UnionNarrowing::narrowVariable(flow, var, type);
      return;
    }
    if (const Type *value = now->nonOptional(); value->isUnion()) {
      std::vector<const Type *> kept;
      for (const Type *m : value->members()) {
        if (TypeRelations::isImplicitlyConvertible(m, type)) {
          kept.push_back(m);
        }
      }
      UnionNarrowing::narrowVariable(flow, var, kept.empty() ? nullptr : Type::unionOf(kept));
    }
  };

  const Predicate *summary = this->predicateOf(fn);
  auto apply = [&](const PredicateSide &side, Flow &flow) {
    if (side.unreachable) {
      flow.unreachable = true;
      return;
    }
    for (auto [i, type] : side.narrowed) {
      if (const VarSymbol *var = variable(i)) {
        narrowTo(flow, var, type);
      }
    }
    for (int i : side.defined) {
      if (const VarSymbol *var = variable(i); var && var->type->isOptional()) {
        flow.defined.insert(var);
      }
    }
    for (const auto &[i, names] : side.fields) {
      if (const VarSymbol *var = variable(i)) {
        FieldFacts::defineField(flow, FieldPath{var, names});
      }
    }
    for (const PredicateSide::NarrowedField &field : side.narrowedFields) {
      if (const VarSymbol *var = variable(field.param)) {
        FieldPath path{var, field.names};
        llvm::erase_if(flow.narrowedFields, [&](const auto &entry) { return entry.first == path; });
        flow.narrowedFields.push_back({path, field.type});
      }
    }
    auto term = [&](const Term &t) -> std::optional<Linear> {
      if (t.isZero()) {
        return Linear{Term::zero(), 0};
      }
      int i = -2;
      for (size_t k = 0; k < summary->params.size(); ++k) {
        if (summary->params[k] == t.var) {
          i = int(k);
        }
      }
      if (i == -2) {
        return std::nullopt;
      }
      const Expr *arg = argument(i);
      if (!arg) {
        return std::nullopt;
      }
      if (t.length) {
        const VarSymbol *var = variable(i);
        if (!var) {
          return std::nullopt;
        }
        return Linear{Term::lengthOf(var), 0};
      }
      std::optional<Linear> value = PredicateChecker::argumentTerm(arg);
      if (value && value->term.var && FlowChecker(this->state).changedByCalls(value->term.var)) {
        return std::nullopt;
      }

      return value;
    };
    for (const Facts::Fact &fact : side.facts) {
      std::optional<Linear> l = term(fact.lhs);
      std::optional<Linear> r = term(fact.rhs);
      if (l && r) {
        flow.facts.add(l->term, r->term, fact.offset + r->offset - l->offset);
      }
    }
  };
  if (summary) {
    apply(summary->whenTrue, whenTrue);
    apply(summary->whenFalse, whenFalse);
  }
  if (fn->predicate) {
    if (const VarSymbol *var = variable(fn->predicate->param)) {
      narrowTo(whenTrue, var, fn->predicate->type);
    }
  }
}

const Type *PredicateChecker::filteredType(const Expr *callback, const Type *element) const {
  const FuncSymbol *fn = nullptr;
  if (auto *fe = dyn_cast<FunctionExpr>(callback)) {
    fn = fe->fn->sym;
  } else if (auto *name = dyn_cast<NameExpr>(callback)) {
    fn = name->func()                             ? name->func()
         : name->var() && name->var()->function() ? name->var()->function()->sym
                                                  : nullptr;
  }
  if (!fn) {
    return nullptr;
  }
  const Type *kept = nullptr;
  if (fn->predicate && fn->predicate->param == 0) {
    kept = fn->predicate->type;
  }
  if (const Predicate *summary = this->predicateOf(fn); summary && !kept) {
    const PredicateSide &side = summary->whenTrue;
    if (side.unreachable) {
      return nullptr;
    }
    for (auto [i, type] : side.narrowed) {
      if (i == 0) {
        kept = type;
      }
    }
    if (!kept && element->isOptional() && llvm::is_contained(side.defined, 0)) {
      kept = element->inner();
    }
  }
  if (!kept || kept == element || !TypeRelations::isImplicitlyConvertible(kept, element)) {
    return nullptr;
  }

  return kept;
}

}
