#include "compiler/semantic/bodies/classes/overload_resolver/overload_resolver.h"
#include "compiler/semantic/bodies/expr/argument_checker/argument_checker.h"
#include "compiler/semantic/bodies/expr/expr_checker/expr_checker.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/flow/flow_checker/flow_checker.h"
#include "compiler/semantic/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/semantic/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/semantic/generics/inference/inference.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/semantic/generics/type_params/type_params.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

OverloadResolver::OverloadResolver(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->typeParams = &all.getTypeParams();
  this->exprChecker = &all.getExprChecker();
  this->flowChecker = &all.getFlowChecker();
  this->typeRelations = &all.getTypeRelations();
  this->returnInferrer = &all.getReturnInferrer();
  this->argumentChecker = &all.getArgumentChecker();
  this->typeArgInferrer = &all.getTypeArgInferrer();
  this->genericInstantiator = &all.getGenericInstantiator();
}

int OverloadResolver::conversionRank(const Type *from, const Type *to) {
  RankRun run;
  run.steps.push_back(RankGoal{from, to});
  while (!run.steps.empty()) {
    RankStep step = std::move(run.steps.back());
    run.steps.pop_back();
    if (const RankGoal *goal = step.as<RankGoal>()) {
      OverloadResolver::enterRank(goal->from, goal->to, run);
    } else {
      OverloadResolver::joinRanks(*step.as<RankJoin>(), run);
    }
  }

  return run.answers.back();
}

void OverloadResolver::enterRank(const Type *from, const Type *to, RankRun &run) {
  std::pair<const Type *, const Type *> pair{from, to};
  if (llvm::is_contained(run.path, pair)) {
    run.answers.push_back(-1);
    return;
  }
  std::optional<int> rank = OverloadResolver::rankLeaf(from, to);
  if (rank.has_value()) {
    run.answers.push_back(*rank);
    return;
  }
  RankJoin join = OverloadResolver::rankJoin(from, to);
  std::vector<RankGoal> parts = join.parts;
  run.path.push_back(pair);
  run.steps.push_back(std::move(join));
  for (const RankGoal &part : llvm::reverse(parts)) {
    run.steps.push_back(part);
  }
}

// TODO: говнокод
std::optional<int> OverloadResolver::rankLeaf(const Type *from, const Type *to) {
  if (from == to || from->isError() || to->isError()) {
    return 0;
  }
  if (to->isOptional()) {
    return from->isUndefined() ? std::optional<int>(1) : std::nullopt;
  }
  if (from->isNever()) {
    return 1;
  }
  if (to->isUnion()) {
    return std::nullopt;
  }
  if (to->isJson()) {
    return from->isString() || from->isNumeric() || from->isBool() ||
                   (from->isArray() && from->inner()->isJson())
               ? 3
               : -1;
  }
  if (from->isParam() && from->constraint()) {
    return std::nullopt;
  }
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
                   (from->returned() == to->returned() || to->returned()->isVoid()) &&
                   (from->sent() == to->sent() || to->sent()->isVoid())
               ? 1
               : -1;
  }
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8 ? 1 : -1;
  }
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() || to->isCrypto() ||
      from->isWeakRef() || to->isWeakRef()) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices")
               ? 1
               : -1;
  }
  if (from->isObject() && to->isObject()) {
    int distance = TypeQueries::subtypeDistance(from, to);
    if (distance > 0) {
      return distance;
    }
  }
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      !from->isAnyObject() && !to->isAnyObject() && from->isTuple() == to->isTuple() &&
      from->fields().size() == to->fields().size() && from->overloads().empty() &&
      to->overloads().empty() && llvm::all_of(from->fields(), [&](const Field &f) {
        const Field *t = to->field(f.name);
        return t && t->type == f.type && (!f.readonly || t->readonly);
      })) {
    return 1;
  }
  if (OverloadResolver::sameShapeObjects(from, to)) {
    return std::nullopt;
  }
  if (from->isResult() && to->isResult()) {
    return std::nullopt;
  }

  return -1;
}

// TODO: говнокод
bool OverloadResolver::sameShapeObjects(const Type *from, const Type *to) {
  return from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
         !from->isAnyObject() && !to->isAnyObject() && !from->isTuple() && !to->isTuple() &&
         from->overloads().empty() && to->overloads().empty() &&
         llvm::all_of(from->fields(), [&](const Field &f) { return to->field(f.name); }) &&
         llvm::all_of(to->fields(), [&](const Field &t) {
           return from->field(t.name) || t.type->isOptional() || t.type->isUndefined();
         });
}

// TODO: говнокод
RankJoin OverloadResolver::rankJoin(const Type *from, const Type *to) {
  RankJoin join{from, to, RankForm::SAME, {}};
  if (to->isOptional()) {
    bool both = from->isOptional();
    join.form = both ? RankForm::OPTIONAL_SAME : RankForm::OPTIONAL_WRAP;
    join.parts = {RankGoal{both ? from->inner() : from, to->inner()}};
  } else if (to->isUnion() && from->isUnion()) {
    join.form = RankForm::WORST_MEMBER;
    for (const Type *member : from->members()) {
      join.parts.push_back(RankGoal{member, to});
    }
  } else if (to->isUnion()) {
    join.form = RankForm::BEST_TARGET;
    for (const Type *member : to->members()) {
      join.parts.push_back(RankGoal{from, member});
    }
  } else if (from->isParam() && from->constraint()) {
    join.parts = {RankGoal{from->constraint(), to}};
  } else if (from->isResult() && to->isResult()) {
    join.form = RankForm::RESULT;
    join.parts = {RankGoal{from->okType(), to->okType()}, RankGoal{from->errType(), to->errType()}};
  } else {
    join.form = RankForm::FIELDS;
    for (const Field &f : from->fields()) {
      join.parts.push_back(RankGoal{f.type, to->field(f.name)->type});
    }
  }

  return join;
}

void OverloadResolver::joinRanks(const RankJoin &join, RankRun &run) {
  run.path.pop_back();
  std::vector<int> ranks = ChildrenFirst::takeAnswers(run.answers, join.parts.size());
  bool failed = llvm::is_contained(ranks, -1);
  int rank = -1;
  switch (join.form) {
    case RankForm::SAME:
      rank = ranks[0];
      break;
    case RankForm::OPTIONAL_SAME:
      rank = ranks[0] < 0 ? -1 : ranks[0];
      break;
    case RankForm::OPTIONAL_WRAP:
      rank = ranks[0] < 0 ? -1 : ranks[0] + 1;
      break;
    case RankForm::WORST_MEMBER:
      rank = failed ? -1 : OverloadResolver::worstRank(ranks);
      break;
    case RankForm::BEST_TARGET:
      rank = OverloadResolver::bestRank(ranks);
      break;
    case RankForm::RESULT:
      rank = OverloadResolver::resultRank(join, ranks);
      break;
    case RankForm::FIELDS:
      rank = failed ? -1 : 2;
      break;
  }
  run.answers.push_back(rank);
}

int OverloadResolver::worstRank(llvm::ArrayRef<int> ranks) {
  int rank = 0;
  for (int inner : ranks) {
    rank = std::max(rank, inner);
  }

  return rank;
}

int OverloadResolver::bestRank(llvm::ArrayRef<int> ranks) {
  int rank = -1;
  for (int inner : ranks) {
    if (inner >= 0 && (rank < 0 || inner < rank)) {
      rank = inner;
    }
  }

  return rank < 0 ? -1 : rank + 1;
}

int OverloadResolver::resultRank(const RankJoin &join, llvm::ArrayRef<int> ranks) {
  std::pair<const Type *, const Type *> halves[2] = {{join.from->okType(), join.to->okType()},
                                                     {join.from->errType(), join.to->errType()}};
  int values[2] = {0, 0};
  for (size_t i = 0; i < 2; ++i) {
    auto [a, b] = halves[i];
    if (a == b) {
      values[i] = 0;
    } else if (a->isNever()) {
      values[i] = 1;
    } else if (a->isVoid() || b->isVoid()) {
      values[i] = -1;
    } else {
      values[i] = ranks[i];
    }
  }

  return values[0] < 0 || values[1] < 0 ? -1 : std::max(values[0], values[1]);
}

const Type *OverloadResolver::parameterAt(const FuncSymbol *sym, size_t i) {
  if (sym->hasRest && i + 1 >= sym->params.size()) {
    const Type *rest = sym->params.back();
    return rest->isArray() ? rest->inner() : rest;
  }

  return i < sym->params.size() ? sym->params[i] : nullptr;
}

bool OverloadResolver::hasFloatLiteral(const Expr *expr) {
  for (const Expr *node : ExprParts::operatorTree(expr)) {
    if (isa<FloatLitExpr>(node)) {
      return true;
    }
  }

  return false;
}

OverloadResolver::ResultVariantChosen
OverloadResolver::chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                                const std::vector<bool> &spread, bool &checked,
                                std::vector<const Type *> *bound) {
  checked = false;
  auto checkLoosely = [&] {
    for (Expr *&arg : args) {
      if (!arg->type) {
        arg = this->exprChecker->checkExpr(arg);
      }
    }
  };
  if (variants.size() == 1) {
    return ok(variants[0]->sym);
  }
  const FunctionDecl *first = variants[0];
  std::string what = first->isConstructor() ? ErrorTextVariant::ctorOf(first->cls()->name)
                                            : ErrorTextVariant::quoted(first->name);
  for (FunctionDecl *v : variants) {
    this->returnInferrer->resolveParams(v->sym);
  }
  if (llvm::is_contained(spread, true)) {
    checkLoosely();
    std::string message = ErrorTextVariant::spread(what);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::SPREAD_ARGUMENT,
        .message = message,
    });
  }
  size_t count = args.size();
  std::vector<FunctionDecl *> fit;
  for (FunctionDecl *v : variants) {
    const FuncSymbol *sym = v->sym;
    if (count >= sym->minArgs && (sym->hasRest || count <= sym->params.size())) {
      fit.push_back(v);
    }
  }
  if (fit.empty()) {
    std::string list = ErrorTextVariant::list(variants);
    checkLoosely();
    std::string message = ErrorTextVariant::count(what, count, list);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::NO_VARIANT_FOR_COUNT,
        .message = message,
    });
  }
  if (fit.size() == 1) {
    return ok(fit[0]->sym);
  }

  for (size_t i = 0; i < count; ++i) {
    const Type *expected = OverloadResolver::parameterAt(fit[0]->sym, i);
    for (FunctionDecl *v : fit) {
      if (OverloadResolver::parameterAt(v->sym, i) != expected ||
          (expected && GenericCheckHelpers::mentionsAnyOf(expected, v->sym->typeParams))) {
        expected = nullptr;
      }
    }
    if (!expected && isa<ObjectLitExpr>(args[i])) {
      int shapes = 0;
      for (FunctionDecl *v : fit) {
        const Type *param = OverloadResolver::parameterAt(v->sym, i);
        const Type *bare = param ? param->nonOptional() : nullptr;
        if (bare && bare->isObject() && !bare->isClass() && !bare->isAnyObject() &&
            !bare->isTuple() && param != expected) {
          expected = param;
          ++shapes;
        }
      }
      if (shapes != 1) {
        expected = nullptr;
      }
    }
    if (!expected && DecimalChecker::isLiteralTree(args[i]) &&
        OverloadResolver::hasFloatLiteral(args[i])) {
      bool decimal = false;
      bool dbl = false;
      for (FunctionDecl *v : fit) {
        const Type *param = OverloadResolver::parameterAt(v->sym, i)->nonOptional();
        decimal |= param->isDecimal();
        dbl |= param->isFloat();
      }
      if (decimal && !dbl) {
        expected = Type::decimalTy();
      }
    }
    args[i] = this->exprChecker->checkExpr(args[i],
                                           expected && !expected->isError() ? expected : nullptr);
  }
  checked = true;
  auto failed = [](const Expr *arg) { return arg->type->isError(); };
  if (llvm::any_of(args, failed)) {
    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }
  // TODO: говнокод
  struct Candidate {
    FunctionDecl *fn;
    std::vector<int> ranks;
    bool leavesOut;
    std::vector<const Type *> typeArgs;
    bool adapted = false;
  };
  std::vector<Candidate> applicable;
  std::string why, numberHint;
  for (FunctionDecl *v : fit) {
    Candidate c{v, {}, v->sym->hasRest || count < v->sym->params.size(), {}};
    if (v->sym->isGeneric() &&
        !this->typeArgInferrer->inferFromArguments(v->sym, args, c.typeArgs)) {
      why += ErrorTextVariant::typeParams(ErrorTextVariant::text(v));
      continue;
    }
    std::string mismatch;
    for (size_t i = 0; i < count && mismatch.empty(); ++i) {
      const Type *param = OverloadResolver::parameterAt(v->sym, i);
      if (v->sym->isGeneric()) {
        param = this->typeParams->substituteValue(param, v->sym->typeParams, c.typeArgs);
      }
      int rank = OverloadResolver::conversionRank(args[i]->type, param);
      if (rank < 0 && this->typeRelations->literalAdapts(args[i], param)) {
        rank = 1;
        c.adapted = true;
      }
      if (rank < 0) {
        mismatch = ErrorTextVariant::mismatch(ErrorTextVariant::text(v), param->name(), i + 1,
                                              args[i]->type->name());
        if (numberHint.empty()) {
          numberHint = TypeRelations::conversionHint(args[i], param);
        }
      }
      c.ranks.push_back(rank);
    }
    if (mismatch.empty()) {
      applicable.push_back(std::move(c));
    } else {
      why += "; " + mismatch;
    }
  }
  if (applicable.empty()) {
    std::string message = ErrorTextVariant::noFit(what, why, numberHint);

    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::NO_VARIANT_FITS,
        .message = message,
    });
  }
  auto better = [&](const Candidate &a, const Candidate &b) {
    bool strictly = false;
    for (size_t i = 0; i < count; ++i) {
      if (a.ranks[i] > b.ranks[i]) {
        return false;
      }
      strictly |= a.ranks[i] < b.ranks[i];
    }
    bool generic = a.fn->sym->isGeneric();
    bool otherGeneric = b.fn->sym->isGeneric();

    return strictly || (!a.leavesOut && b.leavesOut) || (!generic && otherGeneric);
  };
  for (const Candidate &c : applicable) {
    if (llvm::all_of(applicable,
                     [&](const Candidate &other) { return &other == &c || better(c, other); })) {
      if (c.fn->sym->isGeneric()) {
        if (!bound) {
          std::string message = ErrorTextVariant::generic(ErrorTextVariant::text(c.fn));

          return err(OverloadResolverChooseVariantError{
              .code = OverloadResolverChooseVariantErrorCode::GENERIC_VARIANT,
              .message = message,
          });
        }
        *bound = c.typeArgs;
      }
      return ok(c.fn->sym);
    }
  }
  std::vector<FunctionDecl *> tied;
  for (const Candidate &c : applicable) {
    tied.push_back(c.fn);
  }
  auto adapted = [](const Candidate &c) { return c.adapted; };
  bool literals = llvm::all_of(applicable, adapted);
  std::string list = ErrorTextVariant::list(tied);
  std::string message = ErrorTextVariant::tie(what, list, literals);

  return err(OverloadResolverChooseVariantError{
      .code = OverloadResolverChooseVariantErrorCode::SEVERAL_VARIANTS_FIT,
      .message = message,
  });
}

Expr *OverloadResolver::callGenericVariant(CallExpr *call, FuncSymbol *sym,
                                           llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc,
                                           bool method) {
  llvm::ArrayRef<const Type *> genericTypeParams = sym->typeParams;
  std::vector<const Type *> params;
  this->state.getGenericStore().enterInstanceLoc(loc);
  for (const Type *p : sym->params) {
    params.push_back(this->typeParams->substituteValue(p, genericTypeParams, bound));
  }
  const Type *ret = sym->getReturnType();
  call->type = ret ? this->typeParams->substitute(ret, genericTypeParams, bound) : Type::error();
  this->state.getGenericStore().leaveInstanceLoc();
  FuncSymbol *callee = sym;
  const ClassDecl *cls = sym->decl->cls();
  if (!this->state.getFunctionStack().isInsideTrial() && !sym->inferring && sym->hasReturnType() &&
      (!cls || sym->decl->isStatic() || cls->isEmitted())) {
    if (FuncSymbol *instance = this->genericInstantiator->instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
  if (call->superCall()) {
    call->target = SuperCall{.method = callee};
  } else if (method) {
    call->target = MethodCall{.method = callee};
  } else {
    call->target = FunctionCall{.func = callee};
  }
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && method) {
    member->type = this->returnInferrer->functionType(callee);
  }
  std::vector<bool> given(call->args.size(), true);

  return this->argumentChecker->checkArguments(call, params, sym->minArgs, sym->hasRest, loc,
                                               &given);
}

OverloadResolver::ResultVariantOfType
OverloadResolver::variantForType(llvm::ArrayRef<FunctionDecl *> variants, const Type *expected,
                                 const MemberExpr *member) {
  if (variants.size() == 1) {
    return ok(variants[0]->sym);
  }
  std::vector<FunctionDecl *> matching;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && wanted->isFunction()) {
    for (FunctionDecl *v : variants) {
      if (this->returnInferrer->functionType(v->sym) == wanted) {
        matching.push_back(v);
      }
    }
  } else if (this->state.getFunctionStack().getExpectedCallback()) {
    for (FunctionDecl *v : variants) {
      const FuncSymbol *sym = v->sym;
      bool fits =
          !sym->hasRest &&
          sym->minArgs <= this->state.getFunctionStack().getExpectedCallback()->params.size() &&
          sym->params.size() <= this->state.getFunctionStack().getExpectedCallback()->params.size();
      for (size_t i = 0; i < sym->params.size() && fits; ++i) {
        const Type *param = sym->params[i];
        const Type *given = this->state.getFunctionStack().getExpectedCallback()->params[i];
        fits = param == given || (param->isOptional() && param->inner() == given);
      }
      if (fits) {
        matching.push_back(v);
      }
    }
  }
  if (matching.size() == 1) {
    return ok(matching[0]->sym);
  }
  const FunctionDecl *last = variants.back();
  std::string type = "(";
  for (size_t i = 0; i < last->params.size() && i < last->sym->params.size(); ++i) {
    type += (i == 0 ? "" : ", ") + last->params[i].label().str() + ": " +
            last->sym->params[i]->name().str();
  }
  type += ") => " + last->sym->getReturnType()->name().str();
  std::string object = "o";
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    object = name->name.str();
  }
  std::string list = ErrorTextVariant::list(variants);
  std::string message = ErrorTextVariantType::several(member->name, list, type, object);

  return err(OverloadResolverVariantForTypeError{
      .code = OverloadResolverVariantForTypeErrorCode::SEVERAL_VARIANTS,
      .message = message,
  });
}

std::vector<FunctionDecl *> OverloadResolver::signatureVariants(const Type *iface,
                                                                const Overload &overload) {
  ClassStore &classes = this->state.getClassStore();
  std::vector<FunctionDecl *> known = classes.getSignatureVariants(iface, overload.name);
  if (!known.empty()) {
    return known;
  }
  std::vector<FunctionDecl *> variants;
  for (const Type *signature : overload.signatures) {
    auto *fn = this->state.getAst().make<FunctionDecl>(
        llvm::SMLoc(), this->state.getAst().keepName(overload.name));
    FuncSymbol *sym = this->state.getAst().newFunc();
    sym->setCompleter(this->state.getCompleter());
    sym->name = fn->name;
    sym->decl = fn;
    sym->params.assign(signature->params().begin(), signature->params().end());
    sym->minArgs = signature->minArgs();
    sym->hasRest = signature->hasRest();
    sym->setReturnType(signature->ret());
    for (size_t i = 0; i < signature->params().size(); ++i) {
      Param param;
      param.name = this->state.getAst().keepName(std::string(1, char('a' + i % 26)));
      bool rest = sym->hasRest && i + 1 == signature->params().size();
      if (rest) {
        param.form = RestParam{};
      } else if (i >= sym->minArgs) {
        param.form = OptionalParam{};
      }
      param.declaredType = signature->params()[i];
      fn->params.push_back(param);
    }
    fn->sym = sym;
    variants.push_back(fn);
  }
  classes.rememberSignatureVariants(iface, overload.name, variants);

  return variants;
}

Expr *OverloadResolver::checkOverloadCall(CallExpr *call, MemberExpr *member) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  call->type = member->type = Type::error();
  std::vector<FunctionDecl *> variants = this->signatureVariants(iface, overload);
  bool checked = false;
  ResultVariantChosen chosen = this->chooseVariant(variants, call->args, call->spread, checked);
  if (chosen.isErr() &&
      chosen.error().code != OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED) {
    this->state.getDiagnostics().error(member->loc(), chosen.error().message);
  }
  if (chosen.isErr()) {
    return call;
  }
  FuncSymbol *sym = chosen.value();
  auto found = llvm::find(variants, sym->decl);
  int signature = int(found - variants.begin());
  call->target = OverloadCall{.signature = signature};
  member->type = overload.signatures[size_t(signature)];
  call->type = sym->getKnownReturnType();
  this->flowChecker->calledFunction();
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return this->argumentChecker->checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                               member->loc(), &given);
}

Expr *OverloadResolver::checkOverloadMember(MemberExpr *member, const Type *expected) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  member->type = Type::error();
  std::vector<FunctionDecl *> variants = this->signatureVariants(iface, overload);
  ResultVariantOfType variant = this->variantForType(variants, expected, member);
  if (variant.isErr()) {
    this->state.getDiagnostics().error(member->loc(), variant.error().message);
    return member;
  }
  FuncSymbol *sym = variant.value();
  int signature = int(llvm::find(variants, sym->decl) - variants.begin());
  member->form = OverloadMember{.signature = signature};
  member->type = overload.signatures[size_t(signature)];

  return member;
}

}
