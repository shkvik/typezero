#include "compiler/semantic/library/tz_modules/tz_modules_checker/tz_modules_checker.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "compiler/semantic/types/type_relations/type_relations.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr llvm::StringLiteral kSecureOption = "secure";
constexpr llvm::StringLiteral kTransportOption = "transport";

}

TzModulesChecker::TzModulesChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->libraryCalls = &all.libraryCalls;
}

const Expr *TzModulesChecker::uncast(const Expr *expr) const {
  return ExprParts::withoutCasts(expr);
}

bool TzModulesChecker::isPlainTransport(const Expr *transport) const {
  const auto *literal = dyn_cast<ObjectLitExpr>(this->uncast(transport));
  if (literal == nullptr) {
    return false;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      return false;
    }
    if (property.name == kSecureOption) {
      return this->isInsecureValue(property.value);
    }
  }

  return false;
}

bool TzModulesChecker::isInsecureValue(const Expr *value) const {
  const Expr *bare = value != nullptr ? this->uncast(value) : nullptr;
  const auto *secure = dyn_cast_or_null<BoolLitExpr>(bare);

  return secure != nullptr && !secure->value;
}

bool TzModulesChecker::isExternalReference(const Type *type) const {
  return type->isObject() || type->isArray() || type->isIterator() || type->isJson() ||
         type->isDict() || type->isPromise() || type->isUnknown();
}

bool TzModulesChecker::isExternalCallback(const Type *type) const {
  auto isPlain = std::bind_front(&TzModulesChecker::isPlainCallbackParam, this);

  return type->isFunction() && !type->hasRest() && type->ret()->isVoid() &&
         llvm::all_of(type->params(), isPlain);
}

bool TzModulesChecker::isPlainCallbackParam(const Type *param) const {
  return param->isInteger() || param->isFloat() || param->isBool();
}

bool TzModulesChecker::isExternalValue(const Type *type) const {
  if (type->isInteger() || type->isFloat() || type->isBool() || type->isString() ||
      this->isExternalCallback(type)) {
    return true;
  }
  if (type->isOptional()) {
    return this->isExternalReference(type->inner()) && !type->inner()->isArray();
  }

  return this->isExternalReference(type);
}

TzModulesChecker::ResultPlainSignature
TzModulesChecker::checkPlainSignature(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty() || functionDecl->sym->hasRest) {
    auto message = ErrorTextIntrinsic::plain();

    return err(TzModulesCheckerPlainSignatureError{
        .code = TzModulesCheckerPlainSignatureErrorCode::TYPE_PARAMS_OR_REST,
        .message = message,
    });
  }

  return ok(functionDecl);
}

TzModulesChecker::ResultExternalParams
TzModulesChecker::checkExternalParam(const Param &param, const Type *type) const {
  if (param.defaultValue() != nullptr || param.optional()) {
    auto message = ErrorTextIntrinsic::optional();

    return err(TzModulesCheckerExternalParamError{
        .code = TzModulesCheckerExternalParamErrorCode::OPTIONAL_PARAM,
        .message = message,
    });
  }
  if (!type->isError() && !this->isExternalValue(type)) {
    auto message = ErrorTextIntrinsic::takes(type->name());

    return err(TzModulesCheckerExternalParamError{
        .code = TzModulesCheckerExternalParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

TzModulesChecker::ResultExternalReturn
TzModulesChecker::checkExternalReturn(const Type *returnType, const Type *declared) const {
  if (!returnType->isVoid() && !returnType->isError() && !this->isExternalValue(returnType)) {
    auto message = ErrorTextIntrinsic::gives(declared->name());

    return err(TzModulesCheckerExternalReturnError{
        .code = TzModulesCheckerExternalReturnErrorCode::RETURN_TYPE,
        .message = message,
    });
  }

  return ok(returnType);
}

void TzModulesChecker::walkExternal(FunctionDecl *functionDecl) {
  FuncSymbol *function = functionDecl->sym;
  functionDecl->checked = true;
  auto plain = this->checkPlainSignature(functionDecl);
  if (plain.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), plain.error().message);
    return;
  }
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    auto param = this->checkExternalParam(functionDecl->params[index], function->params[index]);
    if (param.isErr()) {
      this->state.getDiagnostics().error(functionDecl->params[index].loc, param.error().message);
    }
  }
  const auto *returnType = function->getKnownReturnType();
  if (returnType == nullptr) {
    return;
  }
  if (returnType->isPromise()) {
    this->state.getInventoryStore().markUsesAsync();
    returnType = returnType->promised();
  }
  if (returnType->isResult()) {
    returnType = returnType->okType();
  }
  auto given = this->checkExternalReturn(returnType, function->getKnownReturnType());
  if (given.isErr()) {
    this->state.getDiagnostics().error(functionDecl->loc(), given.error().message);
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
  bool isConverting = false;
  for (size_t index = 0; index < given.size(); ++index) {
    if (mine[index] == given[index]) {
      continue;
    }
    if (!mine[index]->nonOptional()->isUnknown() || given[index]->isVoid()) {
      return nullptr;
    }
    isConverting = true;
  }
  bool isFewer = mine.size() > given.size();
  const Type *got = from->ret();
  const Type *wanted = to->ret();
  if ((got == wanted && !isFewer && !isConverting) || got->isVoid() || wanted->isVoid() ||
      got->isError() || wanted->isError() ||
      (got != wanted && !TypeRelations::isImplicitlyConvertible(got, wanted))) {
    return nullptr;
  }
  auto *castExpr = this->state.getAst().make<CastExpr>(expr->loc(), expr, std::nullopt);
  castExpr->type = to;
  castExpr->adaptsResult = true;

  return castExpr;
}

std::string TzModulesChecker::knownPrefix(const Expr *expr) const {
  const Expr *source = expr;
  const auto *inner = this->prefixInner(source);
  while (inner != nullptr) {
    source = inner;
    inner = this->prefixInner(source);
  }

  return this->ownPrefix(source);
}

const Expr *TzModulesChecker::prefixInner(const Expr *expr) const {
  auto *conversion = dyn_cast<CastExpr>(expr);
  if (conversion != nullptr) {
    return conversion->operand;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr) {
    bool isStartingWithValue = templateExpr->quasis[0].empty() && !templateExpr->exprs.empty();
    return isStartingWithValue ? templateExpr->exprs[0] : nullptr;
  }
  auto *binary = dyn_cast<BinaryExpr>(expr);
  if (binary != nullptr) {
    return binary->op == BinaryOp::Add ? binary->lhs : nullptr;
  }
  auto *made = dyn_cast<NewExpr>(expr);

  return made != nullptr ? this->addressArgument(made) : nullptr;
}

const Expr *TzModulesChecker::addressArgument(const NewExpr *made) const {
  const ClassDecl *classDecl = made->cls();
  bool isRequest = classDecl != nullptr && classDecl->library() && classDecl->name == "Request";
  bool isUrl = classDecl != nullptr && classDecl->library() && classDecl->name == "URL";
  if (!isRequest && !isUrl) {
    return nullptr;
  }
  const std::vector<Expr *> &args = made->ctor() != nullptr ? made->ctor()->args : made->args;
  bool isGiven = !args.empty() && (isRequest || args.size() == 1);

  return isGiven ? args[0] : nullptr;
}

std::string TzModulesChecker::ownPrefix(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return this->asciiPrefix(literal->value);
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr) {
    return this->asciiPrefix(templateExpr->quasis[0]);
  }
  auto *name = dyn_cast<NameExpr>(expr);
  if (name != nullptr && name->var() != nullptr) {
    return this->state.getConstantStore().findConstPrefix(name->var());
  }

  return std::string();
}

std::string TzModulesChecker::asciiPrefix(const std::u16string &text) const {
  auto out = std::string();
  for (char16_t unit : text) {
    if (unit >= 0x80) {
      break;
    }
    out += static_cast<char>(unit);
  }

  return out;
}

void TzModulesChecker::noteHttpsCall(CallExpr *call, const FunctionDecl *functionDecl) {
  if (functionDecl == nullptr || this->state.getInventoryStore().isUsingTls() ||
      call->args.empty() || this->libraryCalls->isInLibrary(call->loc())) {
    return;
  }
  if (functionDecl == this->state.getLibraryDeclarations().findLibraryFunction("fetch")) {
    std::string prefix = llvm::StringRef(this->knownPrefix(call->args[0])).lower();
    if (!llvm::StringRef(prefix).starts_with("http://") &&
        !llvm::StringRef(prefix).starts_with("data:")) {
      this->state.getInventoryStore().markUsesTls();
    }
  }
}

void TzModulesChecker::noteServer(const NewExpr *made) {
  const std::vector<Expr *> &args = made->ctor() != nullptr ? made->ctor()->args : made->args;
  if (this->state.getInventoryStore().isUsingTls() || args.empty() ||
      this->libraryCalls->isInLibrary(made->loc())) {
    return;
  }
  this->noteServeOptions(args[0]);
}

void TzModulesChecker::noteServeOptions(const Expr *options) {
  const auto *bare = this->uncast(options);
  const auto *literal = dyn_cast<ObjectLitExpr>(bare);
  if (literal != nullptr) {
    auto isSecured = std::bind_front(&TzModulesChecker::isSecuredProperty, this);
    if (llvm::any_of(literal->props, isSecured)) {
      this->state.getInventoryStore().markUsesTls();
    }
  } else {
    bool isSecured = bare->type == nullptr || !bare->type->isObject() ||
                     bare->type->field(kTransportOption) != nullptr;
    if (isSecured) {
      this->state.getInventoryStore().markUsesTls();
    }
  }
}

bool TzModulesChecker::isSecuredProperty(const ObjectLitExpr::Property &property) const {
  return property.spread() ||
         (property.name == kTransportOption && !this->isPlainTransport(property.value));
}

void TzModulesChecker::noteTlsMethod(const MemberExpr *member, const ClassDecl *classDecl) {
  if (this->state.getInventoryStore().isUsingTls() || member->name != "startTls" ||
      this->libraryCalls->isInLibrary(member->loc())) {
    return;
  }
  for (; classDecl != nullptr; classDecl = classDecl->parent) {
    if (classDecl->library() && classDecl->name == "Socket") {
      this->state.getInventoryStore().markUsesTls();
      return;
    }
  }
}

void TzModulesChecker::noteHttpsValue(const FunctionDecl *functionDecl, llvm::SMLoc loc) {
  if (functionDecl != nullptr && !this->libraryCalls->isInLibrary(loc) &&
      functionDecl == this->state.getLibraryDeclarations().findLibraryFunction("fetch")) {
    this->state.getInventoryStore().markUsesTls();
  }
}

}
