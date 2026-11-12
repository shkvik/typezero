#include "compiler/semantic/library/tz_modules/http_checker/http_checker.h"
#include "compiler/program/library/http/fetch_init_table/fetch_init_table.h"
#include "compiler/program/library/http/headers_method_table/headers_method_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kTokenSymbols = "!#$%&'*+-.^_`|~";
constexpr llvm::StringLiteral kGetMethod = "GET";
constexpr llvm::StringLiteral kHeadMethod = "HEAD";

}

HttpChecker::HttpChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->urlChecker = &all.urlChecker;
  this->libraryCalls = &all.libraryCalls;
  this->constantFolder = &all.constantFolder;
}

bool HttpChecker::isTokenChar(unsigned char character) const {
  return character < 0x80 &&
         (llvm::isAlnum(char(character)) || kTokenSymbols.contains(char(character)));
}

bool HttpChecker::isToken(llvm::StringRef text) const {
  auto isTokenUnit = std::bind_front(&HttpChecker::isTokenUnit, this);

  return !text.empty() && llvm::all_of(text, isTokenUnit);
}

bool HttpChecker::isTokenUnit(char character) const {
  return this->isTokenChar(static_cast<unsigned char>(character));
}

bool HttpChecker::isHttpSpace(char16_t unit) const {
  return unit == 0x20 || unit == 0x09 || unit == 0x0A || unit == 0x0D;
}

bool HttpChecker::isValidValue(const std::u16string &text) const {
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

const std::u16string *HttpChecker::stringLiteral(const Expr *expr) const {
  auto *literal = dyn_cast<StringLitExpr>(this->urlChecker->unconverted(expr));
  if (literal != nullptr) {
    return &literal->value;
  }
  auto *templateExpr = dyn_cast<TemplateExpr>(this->urlChecker->unconverted(expr));
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return &templateExpr->quasis[0];
  }

  return nullptr;
}

bool HttpChecker::isLibraryClass(const Type *type, LibraryClass libraryClass) const {
  const ClassDecl *classDecl = type != nullptr ? type->nonOptional()->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, libraryClass);
}

const ObjectLitExpr::Property *HttpChecker::property(const ObjectLitExpr *literal,
                                                     llvm::StringRef name) const {
  for (const ObjectLitExpr::Property &property : literal->props) {
    if (!property.spread() && !property.key() && property.value != nullptr &&
        property.name == name) {
      return &property;
    }
  }

  return nullptr;
}

bool HttpChecker::hasBody(const Expr *body) const {
  const Type *type = this->urlChecker->unconverted(body)->type;

  return !type->isError() && !type->isUndefined() && !type->isOptional();
}

bool HttpChecker::hasCredentials(llvm::StringRef url) const {
  size_t scheme = url.find("://");
  if (scheme == llvm::StringRef::npos) {
    return false;
  }
  llvm::StringRef rest = url.drop_front(scheme + 3);
  auto isEnd = std::bind_front(&HttpChecker::isAuthorityEnd, this);
  llvm::StringRef authority = rest.take_until(isEnd);

  return authority.contains('@');
}

bool HttpChecker::isAuthorityEnd(char character) const {
  return character == '/' || character == '\\' || character == '?' || character == '#';
}

HttpChecker::ResultHeaderName HttpChecker::checkHeaderNameText(llvm::StringRef name) const {
  if (!this->isToken(name)) {
    auto message = ErrorTextHeader::name(name);

    return err(HttpCheckerHeaderNameError{
        .code = HttpCheckerHeaderNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }

  return ok(name);
}

HttpChecker::ResultHeaderText HttpChecker::checkHeaderText(const std::u16string &text) const {
  if (!this->isValidValue(text)) {
    auto message = ErrorTextHeader::value();

    return err(HttpCheckerHeaderTextError{
        .code = HttpCheckerHeaderTextErrorCode::LINE_BREAK,
        .message = message,
    });
  }

  return ok(&text);
}

HttpChecker::ResultRequestUrl HttpChecker::checkRequestUrl(llvm::StringRef text) const {
  if (!this->urlChecker->isValidUrlText(text.str(), RelativeUrls::REJECTED)) {
    auto message = ErrorTextHttpRequest::badUrl(text);

    return err(HttpCheckerRequestUrlError{
        .code = HttpCheckerRequestUrlErrorCode::INVALID_URL,
        .message = message,
    });
  }
  if (this->hasCredentials(text)) {
    auto message = ErrorTextHttpRequest::userInUrl();

    return err(HttpCheckerRequestUrlError{
        .code = HttpCheckerRequestUrlErrorCode::CREDENTIALS,
        .message = message,
    });
  }

  return ok(text);
}

HttpChecker::ResultMethodName HttpChecker::checkMethodName(llvm::StringRef method) const {
  std::string upper = method.upper();
  if (!this->isToken(method)) {
    auto message = ErrorTextHttpRequest::badMethod(method);

    return err(HttpCheckerMethodNameError{
        .code = HttpCheckerMethodNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }
  if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK") {
    auto message = ErrorTextHttpRequest::forbidden(method);

    return err(HttpCheckerMethodNameError{
        .code = HttpCheckerMethodNameErrorCode::FORBIDDEN,
        .message = message,
    });
  }

  return ok(method);
}

bool HttpChecker::isValidReason(const std::u16string &text) const {
  auto isReason = std::bind_front(&HttpChecker::isReasonUnit, this);

  return llvm::all_of(text, isReason);
}

bool HttpChecker::isReasonUnit(char16_t unit) const {
  return unit == 0x09 || (unit >= 0x20 && unit <= 0x7E) || (unit >= 0x80 && unit <= 0xFF);
}

bool HttpChecker::isInProgram(const Expr *expr) const {
  return !this->libraryCalls->isInLibrary(expr->loc());
}

HttpChecker::ResultHeaderNameChecked HttpChecker::checkHeaderName(const Expr *name) {
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

HttpChecker::ResultHeaderValueChecked HttpChecker::checkHeaderValue(const Expr *value) {
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

void HttpChecker::walkHeadersInit(const Expr *init) {
  const auto *bare = this->urlChecker->unconverted(init);
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

void HttpChecker::walkHeaderProperty(const ObjectLitExpr::Property &property) {
  if (property.spread() || property.key() || property.value == nullptr) {
    return;
  }
  auto header = this->checkHeaderNameText(property.name);
  if (header.isErr()) {
    this->state.getDiagnostics().error(property.loc, header.error().message);
  }
  auto value = this->checkHeaderValue(property.value);
  if (value.isErr()) {
    this->state.getDiagnostics().error(property.value->loc(), value.error().message);
  }
}

void HttpChecker::walkHeaderPair(const ArrayLitExpr::Element &element) {
  if (element.spread || element.expr == nullptr) {
    return;
  }
  const auto *pair = dyn_cast<ArrayLitExpr>(this->urlChecker->unconverted(element.expr));
  bool isPair = pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
                !pair->elements[1].spread;
  if (!isPair) {
    return;
  }
  const Expr *nameExpr = pair->elements[0].expr;
  const Expr *valueExpr = pair->elements[1].expr;
  auto name = this->checkHeaderName(nameExpr);
  if (name.isErr()) {
    this->state.getDiagnostics().error(nameExpr->loc(), name.error().message);
  }
  auto value = this->checkHeaderValue(valueExpr);
  if (value.isErr()) {
    this->state.getDiagnostics().error(valueExpr->loc(), value.error().message);
  }
}

void HttpChecker::walkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (this->isInProgram(expr) && !args.empty()) {
    this->walkHeadersInit(args[0]);
  }
}

void HttpChecker::walkHeadersCall(const CallExpr *call, const MemberExpr *member) {
  bool isHeaders = this->isLibraryClass(member->object->type, LibraryClass::HEADERS);
  if (!isHeaders || !this->isInProgram(call) || call->args.empty()) {
    return;
  }
  auto method = HeadersMethodTable::find(member->name);
  if (method.has_value()) {
    auto header = this->checkHeaderName(call->args[0]);
    if (header.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), header.error().message);
    }
  }
  bool isWriting = method == HeadersMethod::APPEND_HEADER || method == HeadersMethod::SET_HEADER;
  if (isWriting && call->args.size() > 1) {
    auto value = this->checkHeaderValue(call->args[1]);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    }
  }
}

void HttpChecker::walkResponseNew(NewExpr *expr) {
  if (!this->isInProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (args.size() < 2) {
    return;
  }
  auto *init = dyn_cast<ObjectLitExpr>(this->urlChecker->unconverted(args[1]));
  if (init == nullptr) {
    return;
  }
  for (const ObjectLitExpr::Property &property : init->props) {
    this->walkResponseProperty(args, property);
  }
}

void HttpChecker::walkResponseProperty(llvm::ArrayRef<Expr *> args,
                                       const ObjectLitExpr::Property &property) {
  if (property.spread() || property.key() || property.value == nullptr) {
    return;
  }
  const auto *value = this->urlChecker->unconverted(property.value);
  auto field = FetchInitTable::find(property.name);
  if (field == FetchInit::STATUS) {
    this->walkResponseStatus(args, value);
  } else if (field == FetchInit::STATUS_TEXT) {
    this->walkStatusText(value);
  } else if (field == FetchInit::HEADERS) {
    this->walkHeadersInit(value);
  }
}

void HttpChecker::walkResponseStatus(llvm::ArrayRef<Expr *> args, const Expr *value) {
  auto status = this->statusConstant(value);
  bool isEmpty = status.has_value() && (*status == 204 || *status == 205 || *status == 304);
  if (isEmpty && this->hasBody(args[0])) {
    auto message = ErrorTextHttpResponse::noBody(*status);
    this->state.getDiagnostics().error(args[0]->loc(), message);
  }
}

std::optional<int64_t> HttpChecker::statusConstant(const Expr *value) const {
  bool isLiteral =
      value->type != nullptr && value->type->isLiteral() && value->type->literalBase()->isInt();
  if (isLiteral) {
    return int64_t(value->type->literalNumber());
  }

  return this->constantFolder->intConstant(value);
}

void HttpChecker::walkStatusText(const Expr *value) {
  const auto *text = this->stringLiteral(value);
  if (text != nullptr && !this->isValidReason(*text)) {
    auto message = ErrorTextHttpResponse::statusText();
    this->state.getDiagnostics().error(value->loc(), message);
  }
}

void HttpChecker::walkRequestNew(NewExpr *expr) {
  if (!this->isInProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  auto isFailed = std::bind_front(&HttpChecker::isFailedArg, this);
  if (args.empty() || llvm::any_of(args, isFailed)) {
    return;
  }
  const auto *input = this->urlChecker->unconverted(args[0]);
  bool isRequestInput = this->isLibraryClass(input->type, LibraryClass::REQUEST) ||
                        this->isLibraryClass(input->type, LibraryClass::URL);
  if (!isRequestInput) {
    this->walkRequestInput(input);
  }
  if (args.size() < 2) {
    return;
  }
  const auto *init = dyn_cast<ObjectLitExpr>(this->urlChecker->unconverted(args[1]));
  if (init == nullptr) {
    return;
  }
  auto method = this->requestMethod(init, input);
  auto bodyName = FetchInitTable::getName(FetchInit::BODY);
  const auto *body = this->property(init, bodyName);
  bool isBodiless = method.name == kGetMethod || method.name == kHeadMethod;
  if (body != nullptr && method.isKnown && isBodiless && this->hasBody(body->value)) {
    auto methodName = FetchInitTable::getName(FetchInit::METHOD);
    bool isGiven = this->property(init, methodName) != nullptr;
    auto message = ErrorTextHttpRequest::getBody(method.name, isGiven);
    this->state.getDiagnostics().error(body->value->loc(), message);
  }
  auto headersName = FetchInitTable::getName(FetchInit::HEADERS);
  const auto *headers = this->property(init, headersName);
  if (headers != nullptr) {
    this->walkHeadersInit(headers->value);
  }
}

bool HttpChecker::isFailedArg(const Expr *arg) const {
  return arg->type->isError();
}

void HttpChecker::walkRequestInput(const Expr *input) {
  auto text = this->urlChecker->literalText(input);
  if (text.has_value()) {
    auto url = this->checkRequestUrl(*text);
    if (url.isErr()) {
      this->state.getDiagnostics().error(input->loc(), url.error().message);
    }
    return;
  }
  if (!this->urlChecker->isKnownAbsolute(input)) {
    auto message = ErrorTextHttpRequest::unknown();
    this->state.getDiagnostics().error(input->loc(), message);
  }
}

HttpChecker::RequestMethod HttpChecker::requestMethod(const ObjectLitExpr *init,
                                                      const Expr *input) {
  bool isPlainInput = !this->isLibraryClass(input->type, LibraryClass::REQUEST);
  auto methodName = FetchInitTable::getName(FetchInit::METHOD);
  const auto *property = this->property(init, methodName);
  if (property == nullptr) {
    return RequestMethod{.name = kGetMethod.str(), .isKnown = isPlainInput};
  }
  const auto *text = this->stringLiteral(property->value);
  if (text == nullptr) {
    return RequestMethod{.name = kGetMethod.str(), .isKnown = false};
  }
  auto written = ToUtf8::toUtf8(*text);
  std::string upper = llvm::StringRef(written).upper();
  auto made = this->checkMethodName(written);
  if (made.isErr()) {
    this->state.getDiagnostics().error(property->value->loc(), made.error().message);
  }

  return RequestMethod{.name = upper, .isKnown = true};
}

}
