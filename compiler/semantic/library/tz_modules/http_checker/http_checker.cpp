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

bool HttpChecker::isTokenChar(unsigned char c) {
  return c < 0x80 &&
         (llvm::isAlnum(char(c)) || llvm::StringRef("!#$%&'*+-.^_`|~").contains(char(c)));
}

bool HttpChecker::isToken(llvm::StringRef text) {
  return !text.empty() && llvm::all_of(text, [](char c) {
    return HttpChecker::isTokenChar(static_cast<unsigned char>(c));
  });
}

bool HttpChecker::isHttpSpace(char16_t c) {
  return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D;
}

bool HttpChecker::validValue(const std::u16string &text) {
  size_t start = 0;
  size_t end = text.size();
  while (start < end && HttpChecker::isHttpSpace(text[start])) {
    ++start;
  }
  while (end > start && HttpChecker::isHttpSpace(text[end - 1])) {
    --end;
  }
  for (size_t i = start; i < end; ++i) {
    if (text[i] == 0 || text[i] == 0x0A || text[i] == 0x0D) {
      return false;
    }
  }

  return true;
}

const std::u16string *HttpChecker::stringLiteral(const Expr *expr) {
  if (auto *lit = dyn_cast<StringLitExpr>(UrlChecker::unconverted(expr))) {
    return &lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(UrlChecker::unconverted(expr));
      tmpl && tmpl->exprs.empty()) {
    return &tmpl->quasis[0];
  }

  return nullptr;
}

bool HttpChecker::isLibraryClass(const Type *type, llvm::StringRef name) {
  const ClassDecl *cls = type ? type->nonOptional()->classDecl() : nullptr;

  return cls && cls->library() && cls->name == name;
}

const ObjectLitExpr::Property *HttpChecker::property(const ObjectLitExpr *lit,
                                                     llvm::StringRef name) {
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (!p.spread() && !p.key() && p.value && p.name == name) {
      return &p;
    }
  }

  return nullptr;
}

bool HttpChecker::hasBody(const Expr *body) {
  const Type *type = UrlChecker::unconverted(body)->type;

  return !type->isError() && !type->isUndefined() && !type->isOptional();
}

bool HttpChecker::hasCredentials(llvm::StringRef url) {
  size_t scheme = url.find("://");
  if (scheme == llvm::StringRef::npos) {
    return false;
  }
  llvm::StringRef rest = url.drop_front(scheme + 3);
  llvm::StringRef authority =
      rest.take_until([](char c) { return c == '/' || c == '\\' || c == '?' || c == '#'; });

  return authority.contains('@');
}

HttpChecker::ResultHeaderName HttpChecker::headerName(llvm::StringRef name) {
  if (!HttpChecker::isToken(name)) {
    std::string message = ErrorTextHeader::name(name);

    return err(HttpCheckerHeaderNameError{
        .code = HttpCheckerHeaderNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }

  return ok(name);
}

HttpChecker::ResultHeaderText HttpChecker::headerText(const std::u16string &text) {
  if (!HttpChecker::validValue(text)) {
    std::string message = ErrorTextHeader::value();

    return err(HttpCheckerHeaderTextError{
        .code = HttpCheckerHeaderTextErrorCode::LINE_BREAK,
        .message = message,
    });
  }

  return ok(&text);
}

HttpChecker::ResultRequestUrl HttpChecker::requestUrl(llvm::StringRef text) {
  if (!UrlChecker::validUrlText(text.str(), false)) {
    std::string message = ErrorTextHttpRequest::badUrl(text);

    return err(HttpCheckerRequestUrlError{
        .code = HttpCheckerRequestUrlErrorCode::INVALID_URL,
        .message = message,
    });
  }
  if (HttpChecker::hasCredentials(text)) {
    std::string message = ErrorTextHttpRequest::userInUrl();

    return err(HttpCheckerRequestUrlError{
        .code = HttpCheckerRequestUrlErrorCode::CREDENTIALS,
        .message = message,
    });
  }

  return ok(text);
}

HttpChecker::ResultMethodName HttpChecker::methodName(llvm::StringRef method) {
  std::string upper = method.upper();
  if (!HttpChecker::isToken(method)) {
    std::string message = ErrorTextHttpRequest::badMethod(method);

    return err(HttpCheckerMethodNameError{
        .code = HttpCheckerMethodNameErrorCode::NOT_A_TOKEN,
        .message = message,
    });
  }
  if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK") {
    std::string message = ErrorTextHttpRequest::forbidden(method);

    return err(HttpCheckerMethodNameError{
        .code = HttpCheckerMethodNameErrorCode::FORBIDDEN,
        .message = message,
    });
  }

  return ok(method);
}

bool HttpChecker::validReason(const std::u16string &text) {
  return llvm::all_of(text, [](char16_t c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
  });
}

bool HttpChecker::inProgram(const Expr *expr) const {
  return !this->libraryCalls->inLibrary(expr->loc());
}

HttpChecker::ResultHeaderNameChecked HttpChecker::checkHeaderName(const Expr *name) {
  const std::u16string *text = HttpChecker::stringLiteral(name);
  if (!text) {
    return ok();
  }
  std::string written = ToUtf8::toUtf8(*text);
  ResultHeaderName header = HttpChecker::headerName(written);
  if (header.isErr()) {
    return err(header.error());
  }

  return ok();
}

HttpChecker::ResultHeaderValueChecked HttpChecker::checkHeaderValue(const Expr *value) {
  const std::u16string *text = HttpChecker::stringLiteral(value);
  if (!text) {
    return ok();
  }
  ResultHeaderText header = HttpChecker::headerText(*text);
  if (header.isErr()) {
    return err(header.error());
  }

  return ok();
}

void HttpChecker::walkHeadersInit(const Expr *init) {
  init = UrlChecker::unconverted(init);
  if (auto *lit = dyn_cast<ObjectLitExpr>(init)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      if (p.spread() || p.key() || !p.value) {
        continue;
      }
      ResultHeaderName header = HttpChecker::headerName(p.name);
      if (header.isErr()) {
        this->state.getDiagnostics().error(p.loc, header.error().message);
      }
      ResultHeaderValueChecked value = this->checkHeaderValue(p.value);
      if (value.isErr()) {
        this->state.getDiagnostics().error(p.value->loc(), value.error().message);
      }
    }
    return;
  }
  if (auto *list = dyn_cast<ArrayLitExpr>(init)) {
    for (const ArrayLitExpr::Element &e : list->elements) {
      auto *pair =
          e.spread || !e.expr ? nullptr : dyn_cast<ArrayLitExpr>(UrlChecker::unconverted(e.expr));
      if (pair && pair->elements.size() == 2 && !pair->elements[0].spread &&
          !pair->elements[1].spread) {
        const Expr *nameExpr = pair->elements[0].expr;
        const Expr *valueExpr = pair->elements[1].expr;
        ResultHeaderNameChecked name = this->checkHeaderName(nameExpr);
        if (name.isErr()) {
          this->state.getDiagnostics().error(nameExpr->loc(), name.error().message);
        }
        ResultHeaderValueChecked value = this->checkHeaderValue(valueExpr);
        if (value.isErr()) {
          this->state.getDiagnostics().error(valueExpr->loc(), value.error().message);
        }
      }
    }
  }
}

void HttpChecker::walkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor() ? expr->ctor()->args : expr->args;
  if (this->inProgram(expr) && !args.empty()) {
    this->walkHeadersInit(args[0]);
  }
}

void HttpChecker::walkHeadersCall(const CallExpr *call, const MemberExpr *member) {
  if (!HttpChecker::isLibraryClass(member->object->type, "Headers") || !this->inProgram(call) ||
      call->args.empty()) {
    return;
  }
  llvm::StringRef name = member->name;
  if (name == "append" || name == "set" || name == "get" || name == "has" || name == "delete") {
    ResultHeaderNameChecked header = this->checkHeaderName(call->args[0]);
    if (header.isErr()) {
      this->state.getDiagnostics().error(call->args[0]->loc(), header.error().message);
    }
  }
  if ((name == "append" || name == "set") && call->args.size() > 1) {
    ResultHeaderValueChecked value = this->checkHeaderValue(call->args[1]);
    if (value.isErr()) {
      this->state.getDiagnostics().error(call->args[1]->loc(), value.error().message);
    }
  }
}

void HttpChecker::walkResponseNew(NewExpr *expr) {
  if (!this->inProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() ? expr->ctor()->args : expr->args;
  if (args.size() < 2) {
    return;
  }
  auto *init = dyn_cast<ObjectLitExpr>(UrlChecker::unconverted(args[1]));
  if (!init) {
    return;
  }
  for (const ObjectLitExpr::Property &p : init->props) {
    if (p.spread() || p.key() || !p.value) {
      continue;
    }
    const Expr *value = UrlChecker::unconverted(p.value);
    if (p.name == "status") {
      std::optional<int64_t> status =
          value->type && value->type->isLiteral() && value->type->literalBase()->isInt()
              ? std::optional<int64_t>(int64_t(value->type->literalNumber()))
              : this->constantFolder->intConstant(value);
      if (status && (*status == 204 || *status == 205 || *status == 304) &&
          HttpChecker::hasBody(args[0])) {
        std::string message = ErrorTextHttpResponse::noBody(*status);
        this->state.getDiagnostics().error(args[0]->loc(), message);
      }
    } else if (p.name == "statusText") {
      if (const std::u16string *text = HttpChecker::stringLiteral(value);
          text && !HttpChecker::validReason(*text)) {
        std::string message = ErrorTextHttpResponse::statusText();
        this->state.getDiagnostics().error(value->loc(), message);
      }
    } else if (p.name == "headers") {
      this->walkHeadersInit(value);
    }
  }
}

void HttpChecker::walkRequestNew(NewExpr *expr) {
  if (!this->inProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() ? expr->ctor()->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const Expr *input = UrlChecker::unconverted(args[0]);
  if (!HttpChecker::isLibraryClass(input->type, "Request") &&
      !HttpChecker::isLibraryClass(input->type, "URL")) {
    if (std::optional<std::string> text = UrlChecker::literalText(input)) {
      ResultRequestUrl url = HttpChecker::requestUrl(*text);
      if (url.isErr()) {
        this->state.getDiagnostics().error(input->loc(), url.error().message);
      }
    } else if (!this->urlChecker->knownAbsolute(input)) {
      std::string message = ErrorTextHttpRequest::unknown();
      this->state.getDiagnostics().error(input->loc(), message);
    }
  }
  if (args.size() < 2) {
    return;
  }
  auto *init = dyn_cast<ObjectLitExpr>(UrlChecker::unconverted(args[1]));
  if (!init) {
    return;
  }
  std::string method = "GET";
  bool isKnownMethod = !HttpChecker::isLibraryClass(input->type, "Request");
  if (const ObjectLitExpr::Property *p = HttpChecker::property(init, "method")) {
    isKnownMethod = false;
    if (const std::u16string *text = HttpChecker::stringLiteral(p->value)) {
      method = ToUtf8::toUtf8(*text);
      std::string upper = llvm::StringRef(method).upper();
      isKnownMethod = true;
      ResultMethodName made = HttpChecker::methodName(method);
      if (made.isErr()) {
        this->state.getDiagnostics().error(p->value->loc(), made.error().message);
      }
      method = upper;
    }
  }
  const ObjectLitExpr::Property *body = HttpChecker::property(init, "body");
  if (body && isKnownMethod && (method == "GET" || method == "HEAD") &&
      HttpChecker::hasBody(body->value)) {
    bool isGiven = HttpChecker::property(init, "method") != nullptr;
    std::string message = ErrorTextHttpRequest::getBody(method, isGiven);
    this->state.getDiagnostics().error(body->value->loc(), message);
  }
  if (const ObjectLitExpr::Property *headers = HttpChecker::property(init, "headers")) {
    this->walkHeadersInit(headers->value);
  }
}

}
