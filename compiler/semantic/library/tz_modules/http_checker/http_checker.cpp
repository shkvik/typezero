#include "compiler/semantic/library/tz_modules/http_checker/http_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/declare/constant_folder/constant_folder.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

HttpChecker::HttpChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->urlChecker = &all.urlChecker;
  this->libraryCalls = &all.libraryCalls;
  this->constantFolder = &all.constantFolder;
}

bool HttpChecker::isTokenChar(unsigned char c) const {
  return c < 0x80 &&
         (llvm::isAlnum(char(c)) || llvm::StringRef("!#$%&'*+-.^_`|~").contains(char(c)));
}

bool HttpChecker::isToken(llvm::StringRef text) const {
  return !text.empty() && llvm::all_of(text, [this](char c) {
    return this->isTokenChar(static_cast<unsigned char>(c));
  });
}

bool HttpChecker::isHttpSpace(char16_t c) const {
  return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D;
}

bool HttpChecker::validValue(const std::u16string &text) const {
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

bool HttpChecker::isLibraryClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *classDecl = type != nullptr ? type->nonOptional()->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() && classDecl->name == name;
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
  llvm::StringRef authority =
      rest.take_until([](char c) { return c == '/' || c == '\\' || c == '?' || c == '#'; });

  return authority.contains('@');
}

HttpChecker::ResultHeaderName HttpChecker::headerName(llvm::StringRef name) const {
  if (!this->isToken(name)) {
    auto message = ErrorTextHeader::name(name);

    return err(HttpCheckerHeaderNameError{
        .code = HttpCheckerHeaderNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }

  return ok(name);
}

HttpChecker::ResultHeaderText HttpChecker::headerText(const std::u16string &text) const {
  if (!this->validValue(text)) {
    auto message = ErrorTextHeader::value();

    return err(HttpCheckerHeaderTextError{
        .code = HttpCheckerHeaderTextErrorCode::LINE_BREAK,
        .message = message,
    });
  }

  return ok(&text);
}

HttpChecker::ResultRequestUrl HttpChecker::requestUrl(llvm::StringRef text) const {
  if (!this->urlChecker->validUrlText(text.str(), false)) {
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

HttpChecker::ResultMethodName HttpChecker::methodName(llvm::StringRef method) const {
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

bool HttpChecker::validReason(const std::u16string &text) const {
  return llvm::all_of(text, [](char16_t c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
  });
}

bool HttpChecker::inProgram(const Expr *expr) const {
  return !this->libraryCalls->inLibrary(expr->loc());
}

HttpChecker::ResultHeaderNameChecked HttpChecker::checkHeaderName(const Expr *name) {
  const auto *text = this->stringLiteral(name);
  if (text == nullptr) {
    return ok();
  }
  auto written = ToUtf8::toUtf8(*text);
  auto header = this->headerName(written);
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
  auto header = this->headerText(*text);
  if (header.isErr()) {
    return err(header.error());
  }

  return ok();
}

void HttpChecker::walkHeadersInit(const Expr *init) {
  init = this->urlChecker->unconverted(init);
  auto *literal = dyn_cast<ObjectLitExpr>(init);
  if (literal != nullptr) {
    for (const ObjectLitExpr::Property &property : literal->props) {
      if (property.spread() || property.key() || property.value == nullptr) {
        continue;
      }
      auto header = this->headerName(property.name);
      if (header.isErr()) {
        this->state.getDiagnostics().error(property.loc, header.error().message);
      }
      auto value = this->checkHeaderValue(property.value);
      if (value.isErr()) {
        this->state.getDiagnostics().error(property.value->loc(), value.error().message);
      }
    }
    return;
  }
  auto *list = dyn_cast<ArrayLitExpr>(init);
  if (list != nullptr) {
    for (const ArrayLitExpr::Element &element : list->elements) {
      auto *pair = element.spread || element.expr == nullptr
                       ? nullptr
                       : dyn_cast<ArrayLitExpr>(this->urlChecker->unconverted(element.expr));
      if (pair != nullptr && pair->elements.size() == 2 && !pair->elements[0].spread &&
          !pair->elements[1].spread) {
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
    }
  }
}

void HttpChecker::walkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (this->inProgram(expr) && !args.empty()) {
    this->walkHeadersInit(args[0]);
  }
}

void HttpChecker::walkHeadersCall(const CallExpr *call, const MemberExpr *member) {
  if (!this->isLibraryClass(member->object->type, "Headers") || !this->inProgram(call) ||
      call->args.empty()) {
    return;
  }
  llvm::StringRef name = member->name;
  if (name == "append" || name == "set" || name == "get" || name == "has" || name == "delete") {
    auto header = this->checkHeaderName(call->args[0]);
    if (header.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), header.error().message);
    }
  }
  if ((name == "append" || name == "set") && call->args.size() > 1) {
    auto value = this->checkHeaderValue(call->args[1]);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    }
  }
}

void HttpChecker::walkResponseNew(NewExpr *expr) {
  if (!this->inProgram(expr)) {
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
    if (property.spread() || property.key() || property.value == nullptr) {
      continue;
    }
    const auto *value = this->urlChecker->unconverted(property.value);
    if (property.name == "status") {
      std::optional<int64_t> status =
          value->type != nullptr && value->type->isLiteral() && value->type->literalBase()->isInt()
              ? std::optional<int64_t>(int64_t(value->type->literalNumber()))
              : this->constantFolder->intConstant(value);
      if (status && (*status == 204 || *status == 205 || *status == 304) &&
          this->hasBody(args[0])) {
        auto message = ErrorTextHttpResponse::noBody(*status);
        this->state.getDiagnostics().error(args[0]->loc(), message);
      }
    } else if (property.name == "statusText") {
      const auto *text = this->stringLiteral(value);
      if (text != nullptr && !this->validReason(*text)) {
        auto message = ErrorTextHttpResponse::statusText();
        this->state.getDiagnostics().error(value->loc(), message);
      }
    } else if (property.name == "headers") {
      this->walkHeadersInit(value);
    }
  }
}

void HttpChecker::walkRequestNew(NewExpr *expr) {
  if (!this->inProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const auto *input = this->urlChecker->unconverted(args[0]);
  if (!this->isLibraryClass(input->type, "Request") && !this->isLibraryClass(input->type, "URL")) {
    if (std::optional<std::string> text = this->urlChecker->literalText(input)) {
      auto url = this->requestUrl(*text);
      if (url.isErr()) {
        this->state.getDiagnostics().error(input->loc(), url.error().message);
      }
    } else if (!this->urlChecker->knownAbsolute(input)) {
      auto message = ErrorTextHttpRequest::unknown();
      this->state.getDiagnostics().error(input->loc(), message);
    }
  }
  if (args.size() < 2) {
    return;
  }
  auto *init = dyn_cast<ObjectLitExpr>(this->urlChecker->unconverted(args[1]));
  if (init == nullptr) {
    return;
  }
  std::string method = "GET";
  bool isKnownMethod = !this->isLibraryClass(input->type, "Request");
  if (const ObjectLitExpr::Property *property = this->property(init, "method")) {
    isKnownMethod = false;
    if (const std::u16string *text = this->stringLiteral(property->value)) {
      method = ToUtf8::toUtf8(*text);
      std::string upper = llvm::StringRef(method).upper();
      isKnownMethod = true;
      auto made = this->methodName(method);
      if (made.isErr()) {
        this->state.getDiagnostics().error(property->value->loc(), made.error().message);
      }
      method = upper;
    }
  }
  const auto *body = this->property(init, "body");
  if (body != nullptr && isKnownMethod && (method == "GET" || method == "HEAD") &&
      this->hasBody(body->value)) {
    bool isGiven = this->property(init, "method") != nullptr;
    auto message = ErrorTextHttpRequest::getBody(method, isGiven);
    this->state.getDiagnostics().error(body->value->loc(), message);
  }
  const auto *headers = this->property(init, "headers");
  if (headers != nullptr) {
    this->walkHeadersInit(headers->value);
  }
}

}
