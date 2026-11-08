#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/infer/return_inferrer/return_inferrer.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

const Expr *TzModulesChecker::uncast(const Expr *expr) {
  while (auto *cast = dyn_cast<CastExpr>(expr)) {
    expr = cast->operand;
  }

  return expr;
}

bool TzModulesChecker::plainTransport(const Expr *transport) {
  auto *lit = dyn_cast<ObjectLitExpr>(TzModulesChecker::uncast(transport));
  if (!lit) {
    return false;
  }
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (p.spread()) {
      return false;
    }
    if (p.name == "secure") {
      auto *secure =
          dyn_cast_or_null<BoolLitExpr>(p.value ? TzModulesChecker::uncast(p.value) : nullptr);
      return secure && !secure->value;
    }
  }

  return false;
}

bool TzModulesChecker::isExternalReference(const Type *type) {
  return type->isObject() || type->isArray() || type->isIterator() || type->isJson() ||
         type->isDict() || type->isPromise() || type->isUnknown();
}

bool TzModulesChecker::isExternalCallback(const Type *type) {
  return type->isFunction() && !type->hasRest() && type->ret()->isVoid() &&
         llvm::all_of(type->params(), [](const Type *param) {
           return param->isInteger() || param->isFloat() || param->isBool();
         });
}

bool TzModulesChecker::isExternalValue(const Type *type) {
  if (type->isInteger() || type->isFloat() || type->isBool() || type->isString() ||
      TzModulesChecker::isExternalCallback(type)) {
    return true;
  }
  if (type->isOptional()) {
    return TzModulesChecker::isExternalReference(type->inner()) && !type->inner()->isArray();
  }

  return TzModulesChecker::isExternalReference(type);
}

TzModulesChecker::TzModulesChecker(SemanticState &owner)
    : state(owner) {}

TzModulesChecker::ResultPlainSignature TzModulesChecker::plainSignature(const FunctionDecl *fn) {
  if (!fn->typeParams.empty() || fn->sym->hasRest) {
    std::string message = ErrorTextIntrinsic::plain();

    return err(TzModulesCheckerPlainSignatureError{
        .code = TzModulesCheckerPlainSignatureErrorCode::TYPE_PARAMS_OR_REST,
        .message = message,
    });
  }

  return ok(fn);
}

TzModulesChecker::ResultExternalParams TzModulesChecker::externalParam(const Param &param,
                                                                       const Type *type) {
  if (param.defaultValue() || param.optional()) {
    std::string message = ErrorTextIntrinsic::optional();

    return err(TzModulesCheckerExternalParamError{
        .code = TzModulesCheckerExternalParamErrorCode::OPTIONAL_PARAM,
        .message = message,
    });
  }
  if (!type->isError() && !TzModulesChecker::isExternalValue(type)) {
    std::string message = ErrorTextIntrinsic::takes(type->name());

    return err(TzModulesCheckerExternalParamError{
        .code = TzModulesCheckerExternalParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

TzModulesChecker::ResultExternalReturn TzModulesChecker::externalReturn(const Type *ret,
                                                                        const Type *declared) {
  if (!ret->isVoid() && !ret->isError() && !TzModulesChecker::isExternalValue(ret)) {
    std::string message = ErrorTextIntrinsic::gives(declared->name());

    return err(TzModulesCheckerExternalReturnError{
        .code = TzModulesCheckerExternalReturnErrorCode::RETURN_TYPE,
        .message = message,
    });
  }

  return ok(ret);
}

void TzModulesChecker::checkExternal(FunctionDecl *fn) {
  FuncSymbol *sym = fn->sym;
  ReturnInferrer(this->state).resolveParams(sym);
  fn->checked = true;
  ResultPlainSignature plain = TzModulesChecker::plainSignature(fn);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), plain.error().message);
    return;
  }
  for (size_t i = 0; i < fn->params.size(); ++i) {
    ResultExternalParams param = TzModulesChecker::externalParam(fn->params[i], sym->params[i]);
    if (param.isErr()) {
      this->state.getDiagnostics().error(fn->params[i].loc, param.error().message);
    }
  }
  const Type *ret = sym->getKnownReturnType();
  if (!ret) {
    return;
  }
  if (ret->isPromise()) {
    this->state.getInventoryStore().markUsesAsync();
    ret = ret->promised();
  }
  if (ret->isResult()) {
    ret = ret->okType();
  }
  ResultExternalReturn given = TzModulesChecker::externalReturn(ret, sym->getKnownReturnType());
  if (given.isErr()) {
    this->state.getDiagnostics().error(fn->loc(), given.error().message);
  }
}

Expr *TzModulesChecker::adaptResult(Expr *expr, const Type *to) {
  const Type *from = expr->type;
  llvm::ArrayRef<const Type *> mine = from->params();
  llvm::ArrayRef<const Type *> given = to->params();
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
  const Type *got = from->ret();
  const Type *wanted = to->ret();
  if ((got == wanted && !fewer && !converts) || got->isVoid() || wanted->isVoid() ||
      got->isError() || wanted->isError() ||
      (got != wanted && !TypeRelations::isImplicitlyConvertible(got, wanted))) {
    return nullptr;
  }
  auto *cast = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
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
      return this->knownPrefix(tmpl->exprs[0]);
    }
    return ascii(tmpl->quasis[0]);
  }
  if (auto *bin = dyn_cast<BinaryExpr>(expr); bin && bin->op == BinaryOp::Add) {
    return this->knownPrefix(bin->lhs);
  }
  if (auto *name = dyn_cast<NameExpr>(expr); name && name->var()) {
    return this->state.getConstantStore().findConstPrefix(name->var());
  }
  if (auto *made = dyn_cast<NewExpr>(expr);
      made && made->cls() && made->cls()->library() &&
      (made->cls()->name == "URL" || made->cls()->name == "Request")) {
    const std::vector<Expr *> &args = made->ctor() ? made->ctor()->args : made->args;
    if (!args.empty() && (made->cls()->name == "Request" || args.size() == 1)) {
      return this->knownPrefix(args[0]);
    }
  }

  return std::string();
}

void TzModulesChecker::noteHttpsCall(CallExpr *call, const FunctionDecl *fn) {
  if (!fn || this->state.getInventoryStore().isUsingTls() || call->args.empty() ||
      LibraryCalls(this->state).inLibrary(call->loc())) {
    return;
  }
  if (fn == this->state.getLibraryDeclarations().findLibraryFunction("fetch")) {
    std::string prefix = llvm::StringRef(this->knownPrefix(call->args[0])).lower();
    if (!llvm::StringRef(prefix).starts_with("http://") &&
        !llvm::StringRef(prefix).starts_with("data:")) {
      this->state.getInventoryStore().markUsesTls();
    }
  }
}

void TzModulesChecker::noteServer(const NewExpr *made) {
  const std::vector<Expr *> &args = made->ctor() ? made->ctor()->args : made->args;
  if (this->state.getInventoryStore().isUsingTls() || args.empty() ||
      LibraryCalls(this->state).inLibrary(made->loc())) {
    return;
  }
  this->noteServeOptions(args[0]);
}

void TzModulesChecker::noteServeOptions(const Expr *options) {
  options = TzModulesChecker::uncast(options);
  if (auto *lit = dyn_cast<ObjectLitExpr>(options)) {
    auto secured = [](const ObjectLitExpr::Property &p) {
      return p.spread() || (p.name == "transport" && !TzModulesChecker::plainTransport(p.value));
    };
    if (llvm::any_of(lit->props, secured)) {
      this->state.getInventoryStore().markUsesTls();
    }
  } else {
    bool secured = !options->type || !options->type->isObject() ||
                   options->type->field("transport") != nullptr;
    if (secured) {
      this->state.getInventoryStore().markUsesTls();
    }
  }
}

void TzModulesChecker::noteTlsMethod(const MemberExpr *member, const ClassDecl *cls) {
  if (this->state.getInventoryStore().isUsingTls() || member->name != "startTls" ||
      LibraryCalls(this->state).inLibrary(member->loc())) {
    return;
  }
  for (; cls; cls = cls->parent) {
    if (cls->library() && cls->name == "Socket") {
      this->state.getInventoryStore().markUsesTls();
      return;
    }
  }
}

void TzModulesChecker::noteHttpsValue(const FunctionDecl *fn, llvm::SMLoc loc) {
  if (fn && !LibraryCalls(this->state).inLibrary(loc) &&
      fn == this->state.getLibraryDeclarations().findLibraryFunction("fetch")) {
    this->state.getInventoryStore().markUsesTls();
  }
}

}
