#include "compiler/check/classes/overload_resolver/overload_resolver.h"
#include "compiler/check/classes/class_check_helpers/class_check_helpers.h"
#include "compiler/check/expr/argument_checker/argument_checker.h"
#include "compiler/check/expr/expr_checker/expr_checker.h"
#include "compiler/check/flow/flow_checker/flow_checker.h"
#include "compiler/check/generics/generic_instantiator/generic_instantiator.h"
#include "compiler/check/generics/type_arg_inferrer/type_arg_inferrer.h"
#include "compiler/check/generics/type_params/type_params.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/decimal/decimal_checker/decimal_checker.h"
#include "compiler/whole_program/flows/flows.h"
#include "llvm/ADT/STLExtras.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::string variantList(llvm::ArrayRef<FunctionDecl *> variants) {
  std::string text;
  for (size_t i = 0; i < variants.size(); ++i) {
    text += (i == 0 ? "" : i + 1 == variants.size() ? " and " : ", ") + variantText(variants[i]);
  }

  return text;
}

namespace {

// How well a value of type `from` fits a parameter of type `to`: 0 for the
// same type, more for a conversion (int to long is better than int to
// double), -1 if it does not fit.
int conversionRank(const Type *from, const Type *to) {
  if (from == to || from->isError() || to->isError()) {
    return 0;
  }
  if (to->isOptional()) {
    if (from->isUndefined()) {
      return 1;
    }
    if (from->isOptional()) {
      int inner = conversionRank(from->inner(), to->inner());
      return inner < 0 ? -1 : inner;
    }
    int inner = conversionRank(from, to->inner());
    return inner < 0 ? -1 : inner + 1;
  }
  // A value that cannot exist: the half of a Result that is never there.
  if (from->isNever()) {
    return 1;
  }
  if (from->isInt() && to == Type::longTy()) {
    return 1;
  }
  // A double and a decimal fit an integer equally well, as in C#: neither
  // converts to the other, so with `f(k: double)` and `f(k: decimal)` a call
  // with an int or a long is ambiguous.
  if (from->isInt() && (to->isFloat() || to->isDecimal())) {
    return 2;
  }
  if (from == Type::longTy() && (to->isFloat() || to->isDecimal())) {
    return 1;
  }
  // A string, a number, a boolean and a Json[] are Json values: a fit worse
  // than any between typed values, so `f(s: string)` wins over `f(j: Json)`.
  if (to->isJson()) {
    return from->isString() || from->isNumeric() || from->isBool() ||
                   (from->isArray() && from->inner()->isJson())
               ? 3
               : -1;
  }
  // A value of `T extends C` is a C.
  if (from->isParam() && from->constraint()) {
    return conversionRank(from->constraint(), to);
  }
  // An iterator seen through a type that drops what it returns or what it
  // is sent.
  if (from->isIterator() && to->isIterator()) {
    return from->yielded() == to->yielded() &&
                   (from->returned() == to->returned() || to->returned()->isVoid()) &&
                   (from->sent() == to->sent() || to->sent()->isVoid())
               ? 1
               : -1;
  }
  // A Buffer is a Uint8Array.
  if (from->isBytes() || to->isBytes()) {
    return from->isBuffer() && to->bytes() == Type::Bytes::Uint8 ? 1 : -1;
  }
  // A match of a known regex is a match of some regex.
  if (from->isMatch() || to->isMatch() || from->isRegExp() || to->isRegExp() || from->isDate() ||
      to->isDate() || from->isIntl() || to->isIntl() || from->isCrypto() || to->isCrypto() ||
      from->isWeakRef() || to->isWeakRef()) {
    return from->isMatch() && to->isMatch() && to->matchGroups() < 0 && !from->field("indices")
               ? 1
               : -1;
  }
  // A type up the declared chain: the nearer, the better, as in C#.
  if (from->isObject() && to->isObject()) {
    int distance = subtypeDistance(from, to);
    if (distance > 0) {
      return distance;
    }
  }
  // An object seen through a type with read-only fields.
  if (from->isObject() && to->isObject() && !from->isClass() && !to->isClass() &&
      !from->isAnyObject() && !to->isAnyObject() && from->isTuple() == to->isTuple() &&
      from->fields().size() == to->fields().size() && from->overloads().empty() &&
      to->overloads().empty() && llvm::all_of(from->fields(), [&](const Field &f) {
        const Field *t = to->field(f.name);
        return t && t->type == f.type && (!f.readonly || t->readonly);
      })) {
    return 1;
  }
  // A Result converts half by half, as good as its worse half:
  // `Result<int, never>` fits `Result<int, string>` better than
  // `Result<double, string>`.
  if (from->isResult() && to->isResult()) {
    auto half = [](const Type *a, const Type *b) {
      if (a == b) {
        return 0;
      }
      if (a->isNever()) {
        return 1;
      }

      return a->isVoid() || b->isVoid() ? -1 : conversionRank(a, b);
    };
    int value = half(from->okType(), to->okType()), error = half(from->errType(), to->errType());
    return value < 0 || error < 0 ? -1 : std::max(value, error);
  }

  return -1;
}

// The type of the parameter that takes argument `i`.
const Type *parameterAt(const FuncSymbol *sym, size_t i) {
  if (sym->hasRest && i + 1 >= sym->params.size()) {
    const Type *rest = sym->params.back();
    return rest->isArray() ? rest->inner() : rest;
  }

  return i < sym->params.size() ? sym->params[i] : nullptr;
}

// Whether a tree of number literals has one written as a double: `1.5`,
// `2.0`, `1e-3`.
bool hasFloatLiteral(const Expr *expr) {
  if (isa<FloatLitExpr>(expr)) {
    return true;
  }
  if (auto *unary = dyn_cast<UnaryExpr>(expr)) {
    return hasFloatLiteral(unary->operand);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr)) {
    return hasFloatLiteral(bin->lhs) || hasFloatLiteral(bin->rhs);
  }

  return false;
}

} // namespace

OverloadResolver::OverloadResolver(Checker &checker) : checker_(checker) {}

FuncSymbol *OverloadResolver::chooseVariant(llvm::ArrayRef<FunctionDecl *> variants,
                                            std::vector<Expr *> &args,
                                            const std::vector<bool> &spread, llvm::SMLoc loc,
                                            bool &checked, std::vector<const Type *> *bound) {
  checked = false;
  auto checkLoosely = [&] {
    for (Expr *&arg : args) {
      if (!arg->type) {
        arg = checker_.exprChecker().checkExpr(arg);
      }
    }
  };
  if (variants.size() == 1) {
    return variants[0]->sym;
  }
  const FunctionDecl *first = variants[0];
  std::string what = first->isConstructor ? "the constructor of '" + first->cls->name.str() + "'"
                                          : "'" + first->name.str() + "'";
  for (FunctionDecl *v : variants) {
    checker_.returnInferrer().resolveParams(v->sym);
  }
  if (llvm::is_contained(spread, true)) {
    checker_.diags_.error(loc, "a spread argument cannot choose among the variants of " + what +
                                   "; pass the arguments one by one");
    checkLoosely();
    return nullptr;
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
    checker_.diags_.error(loc, "no variant of " + what + " takes " + std::to_string(count) +
                                   " argument" + (count == 1 ? "" : "s") + ": there are " +
                                   variantList(variants));
    checkLoosely();
    return nullptr;
  }
  if (fit.size() == 1) {
    return fit[0]->sym;
  }

  // The arguments, each with the type of its parameter if the variants agree
  // on it (and it is no type parameter of a generic variant).
  for (size_t i = 0; i < count; ++i) {
    const Type *expected = parameterAt(fit[0]->sym, i);
    for (FunctionDecl *v : fit) {
      if (parameterAt(v->sym, i) != expected ||
          (expected && typeMentions(expected, v->sym->typeParams))) {
        expected = nullptr;
      }
    }
    // A literal with a fraction is a double, or a decimal where a variant
    // takes one there and none takes a double: `m.times(1.5)` with
    // `times(k: int)` and `times(k: decimal)`.
    if (!expected && DecimalChecker::isLiteralTree(args[i]) && hasFloatLiteral(args[i])) {
      bool decimal = false, dbl = false;
      for (FunctionDecl *v : fit) {
        const Type *param = parameterAt(v->sym, i)->nonOptional();
        decimal |= param->isDecimal();
        dbl |= param->isFloat();
      }
      if (decimal && !dbl) {
        expected = Type::decimalTy();
      }
    }
    args[i] = checker_.exprChecker().checkExpr(args[i], expected && !expected->isError() ? expected
                                                                                         : nullptr);
  }
  checked = true;
  if (llvm::any_of(args, [](const Expr *arg) { return arg->type->isError(); })) {
    return nullptr;
  }

  struct Candidate {
    FunctionDecl *fn;
    std::vector<int> ranks;
    bool leavesOut;                     // takes defaults, optional parameters or a rest parameter
    std::vector<const Type *> typeArgs; // of a generic variant
  };
  std::vector<Candidate> applicable;
  std::string why;
  for (FunctionDecl *v : fit) {
    Candidate c{v, {}, v->sym->hasRest || count < v->sym->params.size(), {}};
    // A generic variant takes the types its arguments give its type
    // parameters.
    if (v->sym->isGeneric() &&
        !checker_.typeArgInferrer().inferFromArguments(v->sym, args, c.typeArgs)) {
      why += "; " + variantText(v) + " cannot take these arguments for its type parameters";
      continue;
    }
    std::string mismatch;
    for (size_t i = 0; i < count && mismatch.empty(); ++i) {
      const Type *param = parameterAt(v->sym, i);
      if (v->sym->isGeneric()) {
        param = checker_.typeParams().substitute(param, v->sym->typeParams, c.typeArgs);
      }
      int rank = conversionRank(args[i]->type, param);
      if (rank < 0) {
        mismatch = variantText(v) + " takes '" + param->name().str() + "' as argument " +
                   std::to_string(i + 1) + ", which is '" + args[i]->type->name().str() + "'";
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
    checker_.diags_.error(loc, "no variant of " + what + " fits these arguments" + why);
    return nullptr;
  }
  auto better = [&](const Candidate &a, const Candidate &b) {
    bool strictly = false;
    for (size_t i = 0; i < count; ++i) {
      if (a.ranks[i] > b.ranks[i]) {
        return false;
      }
      strictly |= a.ranks[i] < b.ranks[i];
    }
    bool generic = a.fn->sym->isGeneric(), otherGeneric = b.fn->sym->isGeneric();

    return strictly || (!a.leavesOut && b.leavesOut) || (!generic && otherGeneric);
  };
  for (const Candidate &c : applicable) {
    if (llvm::all_of(applicable,
                     [&](const Candidate &other) { return &other == &c || better(c, other); })) {
      if (c.fn->sym->isGeneric()) {
        if (!bound) {
          checker_.diags_.error(loc, "the call chooses " + variantText(c.fn) +
                                         ", a generic variant, which " + "cannot be chosen here");
          return nullptr;
        }
        *bound = c.typeArgs;
      }
      return c.fn->sym;
    }
  }
  std::vector<FunctionDecl *> tied;
  for (const Candidate &c : applicable) {
    tied.push_back(c.fn);
  }
  checker_.diags_.error(loc, "the call fits several variants of " + what + " equally well: " +
                                 variantList(tied) + "; convert an argument to choose one");

  return nullptr;
}

Expr *OverloadResolver::callGenericVariant(CallExpr *call, FuncSymbol *sym,
                                           llvm::ArrayRef<const Type *> bound, llvm::SMLoc loc,
                                           bool method) {
  llvm::ArrayRef<const Type *> typeParams = sym->typeParams;
  std::vector<const Type *> params;
  llvm::SMLoc outerLoc = std::exchange(checker_.instanceLoc_, loc);
  for (const Type *p : sym->params) {
    params.push_back(checker_.typeParams().substitute(p, typeParams, bound));
  }
  const Type *ret = checker_.returnInferrer().returnType(sym);
  call->type = ret ? checker_.typeParams().substitute(ret, typeParams, bound) : Type::error();
  checker_.instanceLoc_ = outerLoc;
  FuncSymbol *callee = sym;
  const ClassDecl *cls = sym->decl->cls;
  if (!checker_.trial_ && !sym->inferring && sym->ret &&
      (!cls || sym->decl->isStatic || cls->isEmitted())) {
    if (FuncSymbol *instance = checker_.genericInstantiator().instantiate(sym, bound, loc)) {
      callee = instance;
    }
  }
  (method ? call->method : call->func) = callee;
  if (auto *member = dyn_cast<MemberExpr>(call->callee); member && method) {
    member->type = checker_.returnInferrer().functionType(callee);
  }
  // The arguments chooseVariant checked are only converted.
  std::vector<bool> given(call->args.size(), true);

  return checker_.argumentChecker().checkArguments(call, params, sym->minArgs, sym->hasRest, loc,
                                                   &given);
}

FuncSymbol *OverloadResolver::variantForType(llvm::ArrayRef<FunctionDecl *> variants,
                                             const Type *expected, const MemberExpr *member) {
  if (variants.size() == 1) {
    return variants[0]->sym;
  }
  std::vector<FunctionDecl *> matching;
  const Type *wanted = expected ? expected->nonOptional() : nullptr;
  if (wanted && wanted->isFunction()) {
    for (FunctionDecl *v : variants) {
      if (checker_.returnInferrer().functionType(v->sym) == wanted) {
        matching.push_back(v);
      }
    }
  } else if (checker_.callback_) {
    // A callback, `[1, 2].map(v.add)`: the variant that takes what the
    // method passes.
    for (FunctionDecl *v : variants) {
      const FuncSymbol *sym = v->sym;
      bool fits = !sym->hasRest && sym->minArgs <= checker_.callback_->params.size() &&
                  sym->params.size() <= checker_.callback_->params.size();
      for (size_t i = 0; i < sym->params.size() && fits; ++i) {
        const Type *param = sym->params[i], *given = checker_.callback_->params[i];
        fits = param == given || (param->isOptional() && param->inner() == given);
      }
      if (fits) {
        matching.push_back(v);
      }
    }
  }
  if (matching.size() == 1) {
    return matching[0]->sym;
  }
  const FunctionDecl *last = variants.back();
  std::string type = "(";
  for (size_t i = 0; i < last->params.size() && i < last->sym->params.size(); ++i) {
    type += (i == 0 ? "" : ", ") + last->params[i].label().str() + ": " +
            last->sym->params[i]->name().str();
  }
  type += ") => " + checker_.returnInferrer().returnType(last->sym)->name().str();
  std::string object = "o";
  if (auto *name = dyn_cast<NameExpr>(member->object)) {
    object = name->name.str();
  }
  checker_.diags_.error(member->loc(),
                        "'" + member->name + "' has several variants, " + variantList(variants) +
                            ": say which one with the type of the place it goes, e.g. "
                            "'const f: " +
                            type + " = " + object + "." + member->name + "'");

  return nullptr;
}

std::vector<FunctionDecl *> OverloadResolver::signatureVariants(const Type *iface,
                                                                const Overload &overload) {
  std::vector<FunctionDecl *> &variants = signatureVariants_[{iface, overload.name}];
  if (!variants.empty()) {
    return variants;
  }
  for (const Type *signature : overload.signatures) {
    auto *fn =
        checker_.ctx_.make<FunctionDecl>(llvm::SMLoc(), checker_.ctx_.keepName(overload.name));
    FuncSymbol *sym = checker_.ctx_.newFunc();
    sym->name = fn->name;
    sym->decl = fn;
    sym->params.assign(signature->params().begin(), signature->params().end());
    sym->minArgs = signature->minArgs();
    sym->hasRest = signature->hasRest();
    sym->ret = signature->ret();
    for (size_t i = 0; i < signature->params().size(); ++i) {
      Param param;
      param.name = checker_.ctx_.keepName(std::string(1, char('a' + i % 26)));
      param.rest = sym->hasRest && i + 1 == signature->params().size();
      param.optional = i >= sym->minArgs && !param.rest;
      param.declaredType = signature->params()[i];
      fn->params.push_back(param);
    }
    fn->sym = sym;
    variants.push_back(fn);
  }

  return variants;
}

Expr *OverloadResolver::checkOverloadCall(CallExpr *call, MemberExpr *member) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  call->type = member->type = Type::error();
  std::vector<FunctionDecl *> variants = signatureVariants(iface, overload);
  bool checked = false;
  FuncSymbol *sym = chooseVariant(variants, call->args, call->spread, member->loc(), checked);
  if (!sym) {
    return call;
  }
  auto found = llvm::find(variants, sym->decl);
  call->signature = int(found - variants.begin());
  member->type = overload.signatures[size_t(call->signature)];
  call->type = sym->ret;
  checker_.flowChecker().calledFunction(); // a method runs
  std::vector<bool> given(checked ? call->args.size() : 0, true);

  return checker_.argumentChecker().checkArguments(call, sym->params, sym->minArgs, sym->hasRest,
                                                   member->loc(), &given);
}

Expr *OverloadResolver::checkOverloadMember(MemberExpr *member, const Type *expected) {
  const Type *iface = member->object->type;
  const Overload &overload = *iface->overload(member->name);
  member->type = Type::error();
  std::vector<FunctionDecl *> variants = signatureVariants(iface, overload);
  FuncSymbol *sym = variantForType(variants, expected, member);
  if (!sym) {
    return member;
  }
  member->signature = int(llvm::find(variants, sym->decl) - variants.begin());
  member->type = overload.signatures[size_t(member->signature)];

  return member;
}

} // namespace tz
