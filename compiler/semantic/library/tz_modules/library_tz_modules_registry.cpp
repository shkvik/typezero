#include "compiler/semantic/library/tz_modules/library_tz_modules_registry.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/builtin_names/library_method_table/library_method_table.h"
#include "compiler/program/library/http/connect_option_table/connect_option_table.h"
#include "compiler/program/library/http/fetch_init_table/fetch_init_table.h"
#include "compiler/program/library/http/headers_method_table/headers_method_table.h"
#include "compiler/program/library/http/http_method_table/http_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>

using llvm::dyn_cast;
using llvm::dyn_cast_or_null;

namespace tz {

namespace {

constexpr llvm::StringLiteral kTokenSymbols = "!#$%&'*+-.^_`|~";

}

LibraryTzModulesRegistry::LibraryTzModulesRegistry(SemanticState &owner)
    : state(owner),
      foreign(owner) {}

void LibraryTzModulesRegistry::noteLibraryInstance(const NoteLibraryInstance &task) {
  NewExpr *made = task.made;
  llvm::StringRef name = task.classDecl->name;
  bool isServer = LibraryClassTable::isName(name, LibraryClass::SERVER) ||
                  LibraryClassTable::isName(name, LibraryClass::SOCKET);
  if (LibraryClassTable::isName(name, LibraryClass::URL)) {
    this->walkUrlNew(made);
  }
  if (isServer) {
    this->noteServer(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::RESPONSE)) {
    this->walkResponseNew(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::REQUEST)) {
    this->walkRequestNew(made);
  }
  if (LibraryClassTable::isName(name, LibraryClass::HEADERS)) {
    this->walkHeadersNew(made);
  }
}

const Expr *LibraryTzModulesRegistry::uncast(const Expr *expr) const {
  return ExprParts::withoutCasts(expr);
}

bool LibraryTzModulesRegistry::isPlainTransport(const Expr *transport) const {
  const auto *literal = dyn_cast<ObjectLitExpr>(this->uncast(transport));
  if (literal == nullptr) {
    return false;
  }
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (property.spread()) {
      return false;
    }
    if (ConnectOptionTable::find(property.name) == ConnectOption::SECURE) {
      return this->isInsecureValue(property.value);
    }
  }

  return false;
}

bool LibraryTzModulesRegistry::isInsecureValue(const Expr *value) const {
  const Expr *bare = value != nullptr ? this->uncast(value) : nullptr;
  const auto *secure = dyn_cast_or_null<BoolLitExpr>(bare);

  return secure != nullptr && !secure->value;
}

bool LibraryTzModulesRegistry::isExternalReference(const Type *type) const {
  return type->isObject() || type->isArray() || type->isIterator() || type->isJson() ||
         type->isDict() || type->isPromise() || type->isUnknown();
}

bool LibraryTzModulesRegistry::isExternalCallback(const Type *type) const {
  auto isPlain = std::bind_front(&LibraryTzModulesRegistry::isPlainCallbackParam, this);

  return type->isFunction() && !type->hasRest() && type->ret()->isVoid() &&
         llvm::all_of(type->params(), isPlain);
}

bool LibraryTzModulesRegistry::isPlainCallbackParam(const Type *param) const {
  return param->isInteger() || param->isFloat() || param->isBool();
}

bool LibraryTzModulesRegistry::isExternalValue(const Type *type) const {
  if (type->isInteger() || type->isFloat() || type->isBool() || type->isString() ||
      this->isExternalCallback(type)) {
    return true;
  }
  if (type->isOptional()) {
    return this->isExternalReference(type->inner()) && !type->inner()->isArray();
  }

  return this->isExternalReference(type);
}

LibraryTzModulesRegistry::ResultPlainSignature
LibraryTzModulesRegistry::checkPlainSignature(const FunctionDecl *functionDecl) const {
  if (!functionDecl->typeParams.empty() || functionDecl->sym->hasRest) {
    auto message = ErrorTextIntrinsic::plain();

    return err(LibraryTzModulesRegistryPlainSignatureError{
        .code = LibraryTzModulesRegistryPlainSignatureErrorCode::TYPE_PARAMS_OR_REST,
        .message = message,
    });
  }

  return ok(functionDecl);
}

LibraryTzModulesRegistry::ResultExternalParams
LibraryTzModulesRegistry::checkExternalParam(const Param &param, const Type *type) const {
  if (param.defaultValue() != nullptr || param.optional()) {
    auto message = ErrorTextIntrinsic::optional();

    return err(LibraryTzModulesRegistryExternalParamError{
        .code = LibraryTzModulesRegistryExternalParamErrorCode::OPTIONAL_PARAM,
        .message = message,
    });
  }
  if (!type->isError() && !this->isExternalValue(type)) {
    auto message = ErrorTextIntrinsic::takes(type->name());

    return err(LibraryTzModulesRegistryExternalParamError{
        .code = LibraryTzModulesRegistryExternalParamErrorCode::PARAM_TYPE,
        .message = message,
    });
  }

  return ok(type);
}

LibraryTzModulesRegistry::ResultExternalReturn
LibraryTzModulesRegistry::checkExternalReturn(const Type *returnType, const Type *declared) const {
  if (!returnType->isVoid() && !returnType->isError() && !this->isExternalValue(returnType)) {
    auto message = ErrorTextIntrinsic::gives(declared->name());

    return err(LibraryTzModulesRegistryExternalReturnError{
        .code = LibraryTzModulesRegistryExternalReturnErrorCode::RETURN_TYPE,
        .message = message,
    });
  }

  return ok(returnType);
}

void LibraryTzModulesRegistry::walkExternal(const WalkExternal &task) {
  FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl->foreign()) {
    return this->foreign.walkForeign(functionDecl);
  }
  FuncSymbol *function = functionDecl->sym;
  functionDecl->checked = true;
  auto plain = this->checkPlainSignature(functionDecl);
  if (plain.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), plain.error().message);
    return;
  }
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    auto param = this->checkExternalParam(functionDecl->params[index], function->params[index]);
    if (param.isErr()) {
      this->state.diagnostics.error(functionDecl->params[index].loc, param.error().message);
    }
  }
  const auto *returnType = function->getKnownReturnType();
  if (returnType == nullptr) {
    return;
  }
  if (returnType->isPromise()) {
    this->state.inventory.usesAsync = true;
    returnType = returnType->promised();
  }
  if (returnType->isResult()) {
    returnType = returnType->okType();
  }
  auto given = this->checkExternalReturn(returnType, function->getKnownReturnType());
  if (given.isErr()) {
    this->state.diagnostics.error(functionDecl->loc(), given.error().message);
  }
}

void LibraryTzModulesRegistry::noteHttpsCall(const NoteHttpsCall &task) {
  CallExpr *call = task.call;
  const FunctionDecl *functionDecl = task.functionDecl;
  if (functionDecl == nullptr || this->state.inventory.usesTls || call->args.empty() ||
      this->state.library.declarations.isInsideLibrary(call->loc())) {
    return;
  }
  if (functionDecl ==
      this->state.library.declarations.findLibraryFunction(LibraryFunction::FETCH)) {
    std::string known = this->state.library.tzModules.knownPrefix(call->args[0]);
    std::string prefix = llvm::StringRef(known).lower();
    if (!llvm::StringRef(prefix).starts_with("http://") &&
        !llvm::StringRef(prefix).starts_with("data:")) {
      this->state.inventory.usesTls = true;
    }
  }
}

void LibraryTzModulesRegistry::noteServer(const NewExpr *made) {
  const std::vector<Expr *> &args = made->ctor() != nullptr ? made->ctor()->args : made->args;
  if (this->state.inventory.usesTls || args.empty() ||
      this->state.library.declarations.isInsideLibrary(made->loc())) {
    return;
  }
  this->noteServeOptions(args[0]);
}

void LibraryTzModulesRegistry::noteServeOptions(const Expr *options) {
  const auto *bare = this->uncast(options);
  const auto *literal = dyn_cast<ObjectLitExpr>(bare);
  if (literal != nullptr) {
    auto isSecured = std::bind_front(&LibraryTzModulesRegistry::isSecuredProperty, this);
    if (llvm::any_of(literal->props, isSecured)) {
      this->state.inventory.usesTls = true;
    }
  } else {
    bool isSecured =
        bare->type == nullptr || !bare->type->isObject() ||
        bare->type->field(ConnectOptionTable::getName(ConnectOption::TRANSPORT)) != nullptr;
    if (isSecured) {
      this->state.inventory.usesTls = true;
    }
  }
}

bool LibraryTzModulesRegistry::isSecuredProperty(const ObjectLitExpr::Property &property) const {
  return property.spread() ||
         (ConnectOptionTable::find(property.name) == ConnectOption::TRANSPORT &&
          !this->isPlainTransport(property.value));
}

void LibraryTzModulesRegistry::noteTlsMethod(const NoteTlsMethod &task) {
  const MemberExpr *member = task.member;
  const ClassDecl *classDecl = task.classDecl;
  auto startTls = LibraryMethodTable::getName(LibraryMethod::START_TLS);
  if (this->state.inventory.usesTls || member->name != startTls ||
      this->state.library.declarations.isInsideLibrary(member->loc())) {
    return;
  }
  for (; classDecl != nullptr; classDecl = classDecl->parent) {
    if (classDecl->library() && LibraryClassTable::isName(classDecl->name, LibraryClass::SOCKET)) {
      this->state.inventory.usesTls = true;
      return;
    }
  }
}

void LibraryTzModulesRegistry::noteHttpsValue(const NoteHttpsValue &task) {
  const FunctionDecl *functionDecl = task.functionDecl;
  llvm::SMLoc loc = task.loc;
  if (functionDecl != nullptr && !this->state.library.declarations.isInsideLibrary(loc) &&
      functionDecl ==
          this->state.library.declarations.findLibraryFunction(LibraryFunction::FETCH)) {
    this->state.inventory.usesTls = true;
  }
}

bool LibraryTzModulesRegistry::isTokenChar(unsigned char character) const {
  return character < 0x80 &&
         (llvm::isAlnum(char(character)) || kTokenSymbols.contains(char(character)));
}

bool LibraryTzModulesRegistry::isToken(llvm::StringRef text) const {
  auto isTokenUnit = std::bind_front(&LibraryTzModulesRegistry::isTokenUnit, this);

  return !text.empty() && llvm::all_of(text, isTokenUnit);
}

bool LibraryTzModulesRegistry::isTokenUnit(char character) const {
  return this->isTokenChar(static_cast<unsigned char>(character));
}

bool LibraryTzModulesRegistry::isHttpSpace(char16_t unit) const {
  return unit == 0x20 || unit == 0x09 || unit == 0x0A || unit == 0x0D;
}

bool LibraryTzModulesRegistry::isValidValue(const std::u16string &text) const {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && this->isHttpSpace(text[start])) {
    ++start;
  }
  while (end > start && this->isHttpSpace(text[end - 1])) {
    --end;
  }
  for (size_t index = start; index < end; ++index) {
    if (text[index] == 0 || text[index] == 0x0A || text[index] == 0x0D) {
      return false;
    }
  }

  return true;
}

const std::u16string *LibraryTzModulesRegistry::stringLiteral(const Expr *expr) const {
  const Expr *bare = this->state.library.tzModules.unconverted(expr);
  auto *literal = dyn_cast<StringLitExpr>(bare);
  if (literal != nullptr) {
    return &literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(bare);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return &templateExpr->quasis[0];
  }

  return nullptr;
}

const ObjectLitExpr::Property *LibraryTzModulesRegistry::property(const ObjectLitExpr *literal,
                                                                  llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && !property.key() && property.value != nullptr &&
        property.name == name) {
      return &property;
    }
  }

  return nullptr;
}

bool LibraryTzModulesRegistry::hasBody(const Expr *body) const {
  const Expr *bare = this->state.library.tzModules.unconverted(body);
  const Type *type = bare->type;

  return !type->isError() && !type->isUndefined() && !type->isOptional();
}

bool LibraryTzModulesRegistry::hasCredentials(llvm::StringRef url) const {
  size_t scheme = url.find("://");
  if (scheme == llvm::StringRef::npos) {
    return false;
  }
  llvm::StringRef rest = url.drop_front(scheme + 3);
  auto &tzModules = this->state.library.tzModules;
  auto isEnd = std::bind_front(&LibraryTzModules::isAuthorityEnd, &tzModules);
  llvm::StringRef authority = rest.take_until(isEnd);

  return authority.contains('@');
}

LibraryTzModulesRegistry::ResultHeaderName
LibraryTzModulesRegistry::checkHeaderNameText(llvm::StringRef name) const {
  if (!this->isToken(name)) {
    auto message = ErrorTextHeader::name(name);

    return err(LibraryTzModulesRegistryHeaderNameError{
        .code = LibraryTzModulesRegistryHeaderNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }

  return ok(name);
}

LibraryTzModulesRegistry::ResultHeaderText
LibraryTzModulesRegistry::checkHeaderText(const std::u16string &text) const {
  if (!this->isValidValue(text)) {
    auto message = ErrorTextHeader::value();

    return err(LibraryTzModulesRegistryHeaderTextError{
        .code = LibraryTzModulesRegistryHeaderTextErrorCode::LINE_BREAK,
        .message = message,
    });
  }

  return ok(&text);
}

LibraryTzModulesRegistry::ResultRequestUrl
LibraryTzModulesRegistry::checkRequestUrl(llvm::StringRef text) const {
  if (!this->state.library.tzModules.isValidUrlText(text, RelativeUrls::REJECTED)) {
    auto message = ErrorTextHttpRequest::badUrl(text);

    return err(LibraryTzModulesRegistryRequestUrlError{
        .code = LibraryTzModulesRegistryRequestUrlErrorCode::INVALID_URL,
        .message = message,
    });
  }
  if (this->hasCredentials(text)) {
    auto message = ErrorTextHttpRequest::userInUrl();

    return err(LibraryTzModulesRegistryRequestUrlError{
        .code = LibraryTzModulesRegistryRequestUrlErrorCode::CREDENTIALS,
        .message = message,
    });
  }

  return ok(text);
}

LibraryTzModulesRegistry::ResultMethodName
LibraryTzModulesRegistry::checkMethodName(llvm::StringRef method) const {
  std::string upper = method.upper();
  if (!this->isToken(method)) {
    auto message = ErrorTextHttpRequest::badMethod(method);

    return err(LibraryTzModulesRegistryMethodNameError{
        .code = LibraryTzModulesRegistryMethodNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }
  const auto *known = HttpMethodTable::find(upper);
  if (known != nullptr && known->isForbidden) {
    auto message = ErrorTextHttpRequest::forbidden(method);

    return err(LibraryTzModulesRegistryMethodNameError{
        .code = LibraryTzModulesRegistryMethodNameErrorCode::FORBIDDEN,
        .message = message,
    });
  }

  return ok(method);
}

bool LibraryTzModulesRegistry::isValidReason(const std::u16string &text) const {
  auto isReason = std::bind_front(&LibraryTzModulesRegistry::isReasonUnit, this);

  return llvm::all_of(text, isReason);
}

bool LibraryTzModulesRegistry::isReasonUnit(char16_t unit) const {
  return unit == 0x09 || (unit >= 0x20 && unit <= 0x7E) || (unit >= 0x80 && unit <= 0xFF);
}

bool LibraryTzModulesRegistry::isInProgram(const Expr *expr) const {
  return !this->state.library.declarations.isInsideLibrary(expr->loc());
}

LibraryTzModulesRegistry::ResultHeaderNameChecked
LibraryTzModulesRegistry::checkHeaderName(const Expr *name) {
  const auto *text = this->stringLiteral(name);
  if (text == nullptr) {
    return ok();
  }
  auto written = ToUtf8::toUtf8(*text);
  auto header = this->checkHeaderNameText(written);
  if (header.isErr()) {
    return err(header.error());
  }

  return ok();
}

LibraryTzModulesRegistry::ResultHeaderValueChecked
LibraryTzModulesRegistry::checkHeaderValue(const Expr *value) {
  const auto *text = this->stringLiteral(value);
  if (text == nullptr) {
    return ok();
  }
  auto header = this->checkHeaderText(*text);
  if (header.isErr()) {
    return err(header.error());
  }

  return ok();
}

void LibraryTzModulesRegistry::walkHeadersInit(const Expr *init) {
  const auto *bare = this->state.library.tzModules.unconverted(init);
  const auto *literal = dyn_cast<ObjectLitExpr>(bare);
  if (literal != nullptr) {
    for (const ObjectLitExpr::Property &property : literal->props) {
      this->walkHeaderProperty(property);
    }
    return;
  }
  const auto *list = dyn_cast<ArrayLitExpr>(bare);
  if (list != nullptr) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      this->walkHeaderPair(element);
    }
  }
}

void LibraryTzModulesRegistry::walkHeaderProperty(const ObjectLitExpr::Property &property) {
  if (property.spread() || property.key() || property.value == nullptr) {
    return;
  }
  auto header = this->checkHeaderNameText(property.name);
  if (header.isErr()) {
    this->state.diagnostics.error(property.loc, header.error().message);
  }
  auto value = this->checkHeaderValue(property.value);
  if (value.isErr()) {
    this->state.diagnostics.error(property.value->loc(), value.error().message);
  }
}

void LibraryTzModulesRegistry::walkHeaderPair(const ArrayLitExpr::Element &element) {
  if (element.spread || element.expr == nullptr) {
    return;
  }
  const Expr *bare = this->state.library.tzModules.unconverted(element.expr);
  const auto *pair = dyn_cast<ArrayLitExpr>(bare);
  bool isPair = pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
                !pair->elements[1].spread;
  if (!isPair) {
    return;
  }
  const Expr *nameExpr = pair->elements[0].expr;
  const Expr *valueExpr = pair->elements[1].expr;
  auto name = this->checkHeaderName(nameExpr);
  if (name.isErr()) {
    this->state.diagnostics.error(nameExpr->loc(), name.error().message);
  }
  auto value = this->checkHeaderValue(valueExpr);
  if (value.isErr()) {
    this->state.diagnostics.error(valueExpr->loc(), value.error().message);
  }
}

void LibraryTzModulesRegistry::walkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (this->isInProgram(expr) && !args.empty()) {
    this->walkHeadersInit(args[0]);
  }
}

void LibraryTzModulesRegistry::walkHeadersCall(const WalkHeadersCall &task) {
  const CallExpr *call = task.call;
  const MemberExpr *member = task.member;
  bool isHeaders =
      this->state.library.tzModules.isLibraryClass(member->object->type, LibraryClass::HEADERS);
  if (!isHeaders || !this->isInProgram(call) || call->args.empty()) {
    return;
  }
  auto method = HeadersMethodTable::find(member->name);
  if (method.has_value()) {
    auto header = this->checkHeaderName(call->args[0]);
    if (header.isErr()) {
      this->state.diagnostics.error(call->args[0]->loc(), header.error().message);
    }
  }
  bool isWriting = method == HeadersMethod::APPEND_HEADER || method == HeadersMethod::SET_HEADER;
  if (isWriting && call->args.size() > 1) {
    auto value = this->checkHeaderValue(call->args[1]);
    if (value.isErr()) {
      this->state.diagnostics.error(call->args[1]->loc(), value.error().message);
    }
  }
}

void LibraryTzModulesRegistry::walkResponseNew(NewExpr *expr) {
  if (!this->isInProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (args.size() < 2) {
    return;
  }
  const Expr *bare = this->state.library.tzModules.unconverted(args[1]);
  auto *init = dyn_cast<ObjectLitExpr>(bare);
  if (init == nullptr) {
    return;
  }
  for (const ObjectLitExpr::Property &property : init->props) {
    this->walkResponseProperty(args, property);
  }
}

void LibraryTzModulesRegistry::walkResponseProperty(llvm::ArrayRef<Expr *> args,
                                                    const ObjectLitExpr::Property &property) {
  if (property.spread() || property.key() || property.value == nullptr) {
    return;
  }
  const auto *value = this->state.library.tzModules.unconverted(property.value);
  auto field = FetchInitTable::find(property.name);
  if (field == FetchInit::STATUS) {
    this->walkResponseStatus(args, value);
  } else if (field == FetchInit::STATUS_TEXT) {
    this->walkStatusText(value);
  } else if (field == FetchInit::HEADERS) {
    this->walkHeadersInit(value);
  }
}

void LibraryTzModulesRegistry::walkResponseStatus(llvm::ArrayRef<Expr *> args, const Expr *value) {
  auto status = this->statusConstant(value);
  bool isEmpty = status.has_value() && (*status == 204 || *status == 205 || *status == 304);
  if (isEmpty && this->hasBody(args[0])) {
    auto message = ErrorTextHttpResponse::noBody(*status);
    this->state.diagnostics.error(args[0]->loc(), message);
  }
}

std::optional<int64_t> LibraryTzModulesRegistry::statusConstant(const Expr *value) const {
  bool isLiteral =
      value->type != nullptr && value->type->isLiteral() && value->type->literalBase()->isInt();
  if (isLiteral) {
    return int64_t(value->type->literalNumber());
  }

  return this->state.code.constants.intConstant(value);
}

void LibraryTzModulesRegistry::walkStatusText(const Expr *value) {
  const auto *text = this->stringLiteral(value);
  if (text != nullptr && !this->isValidReason(*text)) {
    auto message = ErrorTextHttpResponse::statusText();
    this->state.diagnostics.error(value->loc(), message);
  }
}

void LibraryTzModulesRegistry::walkRequestNew(NewExpr *expr) {
  if (!this->isInProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  auto isFailed = std::bind_front(&LibraryTzModulesRegistry::isFailedArg, this);
  if (args.empty() || llvm::any_of(args, isFailed)) {
    return;
  }
  const auto *input = this->state.library.tzModules.unconverted(args[0]);
  bool isRequestInput =
      this->state.library.tzModules.isLibraryClass(input->type, LibraryClass::REQUEST) ||
      this->state.library.tzModules.isLibraryClass(input->type, LibraryClass::URL);
  if (!isRequestInput) {
    this->walkRequestInput(input);
  }
  if (args.size() < 2) {
    return;
  }
  const Expr *bareInit = this->state.library.tzModules.unconverted(args[1]);
  const auto *init = dyn_cast<ObjectLitExpr>(bareInit);
  if (init == nullptr) {
    return;
  }
  auto method = this->requestMethod(init, input);
  auto bodyName = FetchInitTable::getName(FetchInit::BODY);
  const auto *body = this->property(init, bodyName);
  const auto *known = HttpMethodTable::find(method.name);
  bool isBodiless = known != nullptr && known->isBodiless;
  if (body != nullptr && method.isKnown && isBodiless && this->hasBody(body->value)) {
    auto methodName = FetchInitTable::getName(FetchInit::METHOD);
    bool isGiven = this->property(init, methodName) != nullptr;
    auto message = ErrorTextHttpRequest::getBody(method.name, isGiven);
    this->state.diagnostics.error(body->value->loc(), message);
  }
  auto headersName = FetchInitTable::getName(FetchInit::HEADERS);
  const auto *headers = this->property(init, headersName);
  if (headers != nullptr) {
    this->walkHeadersInit(headers->value);
  }
}

bool LibraryTzModulesRegistry::isFailedArg(const Expr *arg) const {
  return arg->type->isError();
}

void LibraryTzModulesRegistry::walkRequestInput(const Expr *input) {
  auto text = this->state.library.tzModules.literalText(input);
  if (text.has_value()) {
    auto url = this->checkRequestUrl(*text);
    if (url.isErr()) {
      this->state.diagnostics.error(input->loc(), url.error().message);
    }
    return;
  }
  if (!this->state.library.tzModules.isKnownAbsolute(input)) {
    auto message = ErrorTextHttpRequest::unknown();
    this->state.diagnostics.error(input->loc(), message);
  }
}

LibraryTzModulesRegistry::RequestMethod
LibraryTzModulesRegistry::requestMethod(const ObjectLitExpr *init, const Expr *input) {
  bool isPlainInput =
      !this->state.library.tzModules.isLibraryClass(input->type, LibraryClass::REQUEST);
  auto methodName = FetchInitTable::getName(FetchInit::METHOD);
  const auto *property = this->property(init, methodName);
  if (property == nullptr) {
    auto get = HttpMethodTable::getName(HttpMethod::GET);
    return RequestMethod{.name = get.str(), .isKnown = isPlainInput};
  }
  const auto *text = this->stringLiteral(property->value);
  if (text == nullptr) {
    auto get = HttpMethodTable::getName(HttpMethod::GET);
    return RequestMethod{.name = get.str(), .isKnown = false};
  }
  auto written = ToUtf8::toUtf8(*text);
  std::string upper = llvm::StringRef(written).upper();
  auto made = this->checkMethodName(written);
  if (made.isErr()) {
    this->state.diagnostics.error(property->value->loc(), made.error().message);
  }

  return RequestMethod{.name = upper, .isKnown = true};
}

LibraryTzModulesRegistry::ResultKnownBaseUrl
LibraryTzModulesRegistry::checkKnownBase(const Expr *base) const {
  if (!this->state.library.tzModules.isKnownAbsolute(base)) {
    auto message = ErrorTextUrlNew::base();

    return err(LibraryTzModulesRegistryKnownBaseError{
        .code = LibraryTzModulesRegistryKnownBaseErrorCode::UNKNOWN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

LibraryTzModulesRegistry::ResultUrlTextValid
LibraryTzModulesRegistry::checkUrlText(llvm::StringRef text, bool isBased) const {
  auto relative = isBased ? RelativeUrls::ALLOWED : RelativeUrls::REJECTED;
  if (!this->state.library.tzModules.isValidUrlText(text, relative)) {
    auto message = ErrorTextUrlNew::invalid(text, isBased);

    return err(LibraryTzModulesRegistryUrlTextError{
        .code = LibraryTzModulesRegistryUrlTextErrorCode::INVALID_URL,
        .message = message,
    });
  }

  return ok(text);
}

LibraryTzModulesRegistry::ResultInputIsKnown
LibraryTzModulesRegistry::checkKnownInput(const Expr *input, bool isBased) const {
  if (!this->state.library.tzModules.isKnownAbsolute(input)) {
    auto message = ErrorTextUrlNew::input(isBased);

    return err(LibraryTzModulesRegistryKnownInputError{
        .code = LibraryTzModulesRegistryKnownInputErrorCode::UNKNOWN_INPUT,
        .message = message,
    });
  }

  return ok(input);
}

void LibraryTzModulesRegistry::walkUrlNew(NewExpr *expr) {
  if (this->state.library.declarations.isInsideLibrary(expr->loc())) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  auto isFailed = std::bind_front(&LibraryTzModulesRegistry::isFailedArg, this);
  if (args.empty() || llvm::any_of(args, isFailed)) {
    return;
  }
  const auto *input = this->state.library.tzModules.unconverted(args[0]);
  bool isBased = args.size() > 1;
  if (isBased) {
    auto base = this->checkKnownBase(args[1]);
    if (base.isErr()) {
      this->state.diagnostics.error(args[1]->loc(), base.error().message);
      return;
    }
  }
  auto text = this->state.library.tzModules.literalText(input);
  if (text.has_value()) {
    auto valid = this->checkUrlText(*text, isBased);
    if (valid.isErr()) {
      this->state.diagnostics.error(input->loc(), valid.error().message);
    }
    return;
  }
  auto known = this->checkKnownInput(input, isBased);
  if (known.isErr()) {
    this->state.diagnostics.error(input->loc(), known.error().message);
  }
}

}
