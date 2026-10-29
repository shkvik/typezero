#include "compiler/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/check/types/type_relations/type_relations.h"
#include "compiler/infer/return_inferrer/return_inferrer.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

namespace {

bool isExternalReference(const Type *type) {
  return type->isObject() || type->isArray() || type->isIterator() || type->isJson() ||
         type->isDict() || type->isPromise() || type->isUnknown();
}

bool isExternalCallback(const Type *type) {
  return type->isFunction() && !type->hasRest() && type->ret()->isVoid() &&
         llvm::all_of(type->params(), [](const Type *param) {
           return param->isInteger() || param->isFloat() || param->isBool();
         });
}

bool isExternalValue(const Type *type) {
  if (type->isInteger() || type->isFloat() || type->isBool() || type->isString() ||
      isExternalCallback(type)) {
    return true;
  }
  if (type->isOptional()) {
    return isExternalReference(type->inner()) && !type->inner()->isArray();
  }

  return isExternalReference(type);
}

}

TzModulesChecker::TzModulesChecker(Checker &checker) : checker_(checker) {}

void TzModulesChecker::checkExternal(FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  checker_.returnInferrer().resolveParams(sym);
  fn->checked = true;
  if (!fn->typeParams.empty() || sym->hasRest) {
    checker_.diags_.error(fn->loc(),
                          "an intrinsic takes plain parameters: no type parameters, no rest");
    return;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    const Type *type = sym->params[i];
    if (fn->params[i].defaultValue || fn->params[i].optional) {
      checker_.diags_.error(fn->params[i].loc, "an intrinsic has no optional parameters");
    } else if (!type->isError() && !isExternalValue(type)) {
      checker_.diags_.error(fn->params[i].loc,
                            "an intrinsic cannot take a value of type '" + type->name() + "'");
    }
  }
  const Type *ret = sym->ret;
  if (!ret) {
    return;
  }
  if (ret->isPromise()) {
    checker_.file_->usesAsync = true;
    ret = ret->promised();
  }
  if (ret->isResult()) {
    ret = ret->okType();
  }
  if (!ret->isVoid() && !ret->isError() && !isExternalValue(ret)) {
    checker_.diags_.error(fn->loc(),
                          "an intrinsic cannot give a value of type '" + sym->ret->name() + "'");
  }
}

Expr *TzModulesChecker::adaptResult(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  llvm::ArrayRef<const Type *> mine = from->params(), given = to->params();
  if (mine.size() < given.size() || from->minArgs() > given.size() || from->hasRest() ||
      to->hasRest() || from->hasThis() != to->hasThis() || (given.size() < to->minArgs())) {
    return nullptr;
  }
  bool converts = false;
  for (size_t i = 0; i < given.size(); ++i) {
    if (mine[i] == given[i]) {
      continue;
    }
    if (!mine[i]->nonOptional()->isUnknown() || given[i]->isVoid()) {
      return nullptr;
    }
    converts = true;
  }
  bool fewer = mine.size() > given.size();
  const Type *got = from->ret(), *wanted = to->ret();
  if ((got == wanted && !fewer && !converts) || got->isVoid() || wanted->isVoid() ||
      got->isError() || wanted->isError() ||
      (got != wanted && !TypeRelations::isImplicitlyConvertible(got, wanted))) {
    return nullptr;
  }
  auto *cast = checker_.ctx_.make<CastExpr>(expr->loc(), expr, std::nullopt);
  cast->type = to;
  cast->adaptsResult = true;

  return cast;
}

std::string TzModulesChecker::knownPrefix(const Expr *expr) const {
  while (auto *cast = dyn_cast<CastExpr>(expr)) {
    expr = cast->operand;
  }
  auto ascii = [](const std::u16string &text) {
    std::string out;
    for (char16_t c : text) {
      if (c >= 0x80) {
        break;
      }
      out += static_cast<char>(c);
    }

    return out;
  };
  if (auto *lit = dyn_cast<StringLitExpr>(expr)) {
    return ascii(lit->value);
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(expr)) {
    if (tmpl->quasis[0].empty() && !tmpl->exprs.empty()) {
      return knownPrefix(tmpl->exprs[0]);
    }
    return ascii(tmpl->quasis[0]);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr); bin && bin->op == BinaryOp::Add) {
    return knownPrefix(bin->lhs);
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var) {
    auto it = checker_.constPrefixes_.find(name->var);
    return it == checker_.constPrefixes_.end() ? std::string() : it->second;
  }
  if (auto *made = dyn_cast<NewExpr>(expr);
      made && made->cls && made->cls->library &&
      (made->cls->name == "URL" || made->cls->name == "Request")) {
    const std::vector<Expr *> &args = made->ctor ? made->ctor->args : made->args;
    if (!args.empty() && (made->cls->name == "Request" || args.size() == 1)) {
      return knownPrefix(args[0]);
    }
  }

  return std::string();
}

void TzModulesChecker::noteHttpsCall(CallExpr *call, const FunctionDecl *fn) {
  if (!fn || checker_.file_->usesTls || call->args.empty() ||
      checker_.libraryCalls().inLibrary(call->loc())) {
    return;
  }
  if (fn == checker_.libFunctions_.lookup("fetch")) {
    std::string prefix = llvm::StringRef(knownPrefix(call->args[0])).lower();
    if (!llvm::StringRef(prefix).starts_with("http://") &&
        !llvm::StringRef(prefix).starts_with("data:")) {
      checker_.file_->usesTls = true;
    }
  }
}

void TzModulesChecker::noteServer(const NewExpr *made) {
  const std::vector<Expr *> &args = made->ctor ? made->ctor->args : made->args;
  if (checker_.file_->usesTls || args.empty() || checker_.libraryCalls().inLibrary(made->loc())) {
    return;
  }
  noteServeOptions(args[0]);
}

static const Expr *uncast(const Expr *expr) {
  while (auto *cast = dyn_cast<CastExpr>(expr)) {
    expr = cast->operand;
  }

  return expr;
}

static bool plainTransport(const Expr *transport) {
  auto *lit = dyn_cast<ObjectLitExpr>(uncast(transport));
  if (!lit) {
    return false;
  }
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (p.spread) {
      return false;
    }
    if (p.name == "secure") {
      auto *secure = dyn_cast_or_null<BoolLitExpr>(p.value ? uncast(p.value) : nullptr);
      return secure && !secure->value;
    }
  }

  return false;
}

void TzModulesChecker::noteServeOptions(const Expr *options) {
  options = uncast(options);
  if (auto *lit = dyn_cast<ObjectLitExpr>(options)) {
    checker_.file_->usesTls = llvm::any_of(lit->props, [](const ObjectLitExpr::Property &p) {
      return p.spread || (p.name == "transport" && !plainTransport(p.value));
    });
  } else {
    checker_.file_->usesTls = !options->type || !options->type->isObject() ||
                              options->type->field("transport") != nullptr;
  }
}

void TzModulesChecker::noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc) {
  if (fn && !checker_.libraryCalls().inLibrary(loc) &&
      fn == checker_.libFunctions_.lookup("fetch")) {
    checker_.file_->usesTls = true;
  }
}

}
