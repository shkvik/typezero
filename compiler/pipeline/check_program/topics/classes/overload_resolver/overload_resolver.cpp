#include "compiler/pipeline/check_program/topics/classes/overload_resolver/overload_resolver.h"
#include "compiler/pipeline/check_program/flow/flow_checker/flow_checker.h"
#include "compiler/pipeline/check_program/generics/generic_check_helpers/generic_check_helpers.h"
#include "compiler/pipeline/check_program/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/pipeline/check_program/generics/inference/inference.h"
#include "compiler/pipeline/check_program/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/pipeline/check_program/generics/type_params/type_params.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/topics/decimal/decimal_checker/decimal_checker.h"
#include "compiler/pipeline/check_program/topics/expr/argument_checker/argument_checker.h"
#include "compiler/pipeline/check_program/topics/expr/expr_checker/expr_checker.h"
#include "compiler/pipeline/check_program/types/type_relations/type_relations.h"
#include "compiler/pipeline/check_program/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"

using llvm::dyn_cast;
using llvm::isa;

namespace tz {

int OverloadResolver::conversionRank(const Type *from, const Type *to) {
  if (from == to || from->isError() || to->isError()) {
    return 0;
  }
  if (to->isOptional()) {
    if (from->isUndefined()) {
      return 1;
    }
    if (from->isOptional()) {
      int inner = OverloadResolver::conversionRank(from->inner(), to->inner());
      return inner < 0 ? -1 : inner;
    }
    int inner = OverloadResolver::conversionRank(from, to->inner());
    return inner < 0 ? -1 : inner + 1;
  }
  if (from->isNever()) {
    return 1;
  }
  if (to->isUnion()) {
    int rank = -1;
    if (from->isUnion()) {
      rank = 0;
      for (const Type *member : from->members()) {
        int inner = OverloadResolver::conversionRank(member, to);
        if (inner < 0) {
          return -1;
        }
        rank = std::max(rank, inner);
      }
      return rank;
    }
    for (const Type *member : to->members()) {
      int inner = OverloadResolver::conversionRank(from, member);
      if (inner >= 0 && (rank < 0 || inner < rank)) {
        rank = inner;
      }
    }
    return rank < 0 ? -1 : rank + 1;
  }
  if (to->isJson()) {
    return from->isString() || from->isNumeric() || from->isBool() ||
                   (from->isArray() && from->inner()->isJson())
               ? 3
               : -1;
  }
  if (from->isParam() && from->constraint()) {
    return OverloadResolver::conversionRank(from->constraint(), to);
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
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      !from->isAnyObject() && !to->isAnyObject() && !from->isTuple() && !to->isTuple() &&
      from->overloads().empty() && to->overloads().empty() &&
      llvm::all_of(from->fields(),
                   [&](const Field &f) {
                     const Field *t = to->field(f.name);
                     return t && OverloadResolver::conversionRank(f.type, t->type) >= 0;
                   }) &&
      llvm::all_of(to->fields(), [&](const Field &t) {
        return from->field(t.name) || t.type->isOptional() || t.type->isUndefined();
      })) {
    return 2;
  }
  if (from->isResult() && to->isResult()) {
    auto half = [](const Type *a, const Type *b) {
      if (a == b) {
        return 0;
      }
      if (a->isNever()) {
        return 1;
      }

      return a->isVoid() || b->isVoid() ? -1 : OverloadResolver::conversionRank(a, b);
    };
    int value = half(from->okType(), to->okType());
    int error = half(from->errType(), to->errType());
    return value < 0 || error < 0 ? -1 : std::max(value, error);
  }

  return -1;
}

const Type *OverloadResolver::parameterAt(const FuncSymbol *sym, size_t i) {
  if (sym->hasRest && i + 1 >= sym->params.size()) {
    const Type *rest = sym->params.back();
    return rest->isArray() ? rest->inner() : rest;
  }

  return i < sym->params.size() ? sym->params[i] : nullptr;
}

bool OverloadResolver::hasFloatLiteral(const Expr *expr) {
  if (isa<FloatLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return OverloadResolver::hasFloatLiteral(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return OverloadResolver::hasFloatLiteral(bin->lhs) ||
           OverloadResolver::hasFloatLiteral(bin->rhs);
  }

  return false;
}

OverloadResolver::OverloadResolver(CheckState &owner)
    : state(owner) {}

OverloadResolver::ResultVariantChosen
OverloadResolver::chooseVariant(llvm::ArrayRef<FunctionDecl *> variants, std::vector<Expr *> &args,
                                const std::vector<bool> &spread, bool &checked,
                                std::vector<const Type *> *bound) {
  checked = false;
  auto checkLoosely = [&] {
    for (Expr *&arg : args) {
      if (!arg->type) {
        arg = ExprChecker(this->state).checkExpr(arg);
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
    ReturnInferrer(this->state).resolveParams(v->sym);
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
    args[i] = ExprChecker(this->state)
                  .checkExpr(args[i], expected && !expected->isError() ? expected : nullptr);
  }
  checked = true;
  auto failed = [](const Expr *arg) { return arg->type->isError(); };
  if (llvm::any_of(args, failed)) {
    return err(OverloadResolverChooseVariantError{
        .code = OverloadResolverChooseVariantErrorCode::ALREADY_REPORTED,
        .message = "",
    });
  }

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
        !TypeArgInferrer(this->state).inferFromArguments(v->sym, args, c.typeArgs)) {
      why += ErrorTextVariant::typeParams(ErrorTextVariant::text(v));
      continue;
    }
    std::string mismatch;
    for (size_t i = 0; i < count && mismatch.empty(); ++i) {
      const Type *param = OverloadResolver::parameterAt(v->sym, i);
      if (v->sym->isGeneric()) {
        param = TypeParams(this->state).substituteValue(param, v->sym->typeParams, c.typeArgs);
      }
      int rank = OverloadResolver::conversionRank(args[i]->type, param);
      if (rank < 0 && TypeRelations(this->state).literalAdapts(args[i], param)) {
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
  llvm::ArrayRef<const Type *> typeParams = sym->typeParams;
  std::vector<const Type *> params;
  this->state.getGenericStore().enterInstanceLoc(loc);
  for (const Type *p : sym->params) {
    params.push_back(TypeParams(this->state).substituteValue(p, typeParams, bound));
  }
  const Type *ret = ReturnInferrer(this->state).returnType(sym);
  call->type = ret ? TypeParams(this->state).substitute(ret, typeParams, bound) : Type::error();
  this->state.getGenericStore().leaveInstanceLoc();
  FuncSymbol *callee = sym;
  const ClassDecl *cls = sym->decl->cls();
  if (!this->state.getFunctionStack().isInsideTrial() && !sym->inferring && sym->ret &&
      (!cls || sym->decl->isStatic() || cls->isEmitted())) {
    if (FuncSymbol *instance = GenericInstantiator(this->state).instantiate(sym, bound, loc)) {
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
    member->type = ReturnInferrer(this->state).functionType(callee);
  }
  std::vector<bool> given(call->args.size(), true);

  return ArgumentChecker(this->state)
      .checkArguments(call, params, sym->minArgs, sym->hasRest, loc, &given);
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
      if (ReturnInferrer(this->state).functionType(v->sym) == wanted) {
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
  type += ") => " + ReturnInferrer(this->state).returnType(last->sym)->name().str();
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
    sym->name = fn->name;
    sym->decl = fn;
    sym->params.assign(signature->params().begin(), signature->params().end());
    sym->minArgs = signature->minArgs();
    sym->hasRest = signature->hasRest();
    sym->ret = signature->ret();
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
  call->type = sym->ret;
  FlowChecker(this->state).calledFunction();
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return ArgumentChecker(this->state)
      .checkArguments(call, sym->params, sym->minArgs, sym->hasRest, member->loc(), &given);
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
