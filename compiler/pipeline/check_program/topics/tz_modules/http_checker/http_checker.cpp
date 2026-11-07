#include "compiler/pipeline/check_program/topics/tz_modules/http_checker/http_checker.h"
#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder.h"
#include "compiler/pipeline/check_program/topics/tz_modules/library_calls/library_calls.h"
#include "compiler/pipeline/check_program/topics/tz_modules/url_checker/url_checker.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

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

std::string HttpChecker::utf8(const std::u16string &units) {
  std::string text;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(units.data()),
                                  units.size()),
      text);

  return text;
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

HttpChecker::HttpChecker(CheckState &owner)
    : state(owner) {}

bool HttpChecker::validReason(const std::u16string &text) {
  return llvm::all_of(text, [](char16_t c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
  });
}

bool HttpChecker::inProgram(const Expr *expr) const {
  return !LibraryCalls(this->state).inLibrary(expr->loc());
}

void HttpChecker::checkHeaderName(const Expr *name) {
  const std::u16string *text = HttpChecker::stringLiteral(name);
  if (text && !HttpChecker::isToken(HttpChecker::utf8(*text))) {
    this->state.getDiagnostics().error(name->loc(),
                                       "'" + HttpChecker::utf8(*text) +
                                           "' is not a name of a header: a name is letters, "
                                           "digits and !#$%&'*+-.^_`|~, without spaces");
  }
}

void HttpChecker::checkHeaderValue(const Expr *value) {
  const std::u16string *text = HttpChecker::stringLiteral(value);
  if (text && !HttpChecker::validValue(*text)) {
    this->state.getDiagnostics().error(value->loc(),
                                       "the value of a header cannot hold a line break or NUL");
  }
}

void HttpChecker::checkHeadersInit(const Expr *init) {
  init = UrlChecker::unconverted(init);
  if (auto *lit = dyn_cast<ObjectLitExpr>(init)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      if (p.spread() || p.key() || !p.value) {
        continue;
      }
      if (!HttpChecker::isToken(p.name)) {
        this->state.getDiagnostics().error(p.loc,
                                           "'" + p.name.str() +
                                               "' is not a name of a header: a name is letters, "
                                               "digits and !#$%&'*+-.^_`|~, without spaces");
      }
      this->checkHeaderValue(p.value);
    }
    return;
  }
  if (auto *list = dyn_cast<ArrayLitExpr>(init)) {
    for (const ArrayLitExpr::Element &e : list->elements) {
      auto *pair =
          e.spread || !e.expr ? nullptr : dyn_cast<ArrayLitExpr>(UrlChecker::unconverted(e.expr));
      if (pair && pair->elements.size() == 2 && !pair->elements[0].spread &&
          !pair->elements[1].spread) {
        this->checkHeaderName(pair->elements[0].expr);
        this->checkHeaderValue(pair->elements[1].expr);
      }
    }
  }
}

void HttpChecker::checkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor() ? expr->ctor()->args : expr->args;
  if (this->inProgram(expr) && !args.empty()) {
    this->checkHeadersInit(args[0]);
  }
}

void HttpChecker::checkHeadersCall(const CallExpr *call, const MemberExpr *member) {
  if (!HttpChecker::isLibraryClass(member->object->type, "Headers") || !this->inProgram(call) ||
      call->args.empty()) {
    return;
  }
  llvm::StringRef name = member->name;
  if (name == "append" || name == "set" || name == "get" || name == "has" || name == "delete") {
    this->checkHeaderName(call->args[0]);
  }
  if ((name == "append" || name == "set") && call->args.size() > 1) {
    this->checkHeaderValue(call->args[1]);
  }
}

void HttpChecker::checkResponseNew(NewExpr *expr) {
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
              : ConstantFolder(this->state).intConstant(value);
      if (status && (*status == 204 || *status == 205 || *status == 304) &&
          HttpChecker::hasBody(args[0])) {
        std::string code = std::to_string(*status);
        this->state.getDiagnostics().error(args[0]->loc(),
                                           "a response with status " + code +
                                               " has no body; write 'new Response(undefined, "
                                               "{ status: " +
                                               code + " })'");
      }
    } else if (p.name == "statusText") {
      if (const std::u16string *text = HttpChecker::stringLiteral(value);
          text && !HttpChecker::validReason(*text)) {
        this->state.getDiagnostics().error(
            value->loc(), "a statusText holds only tabs, spaces and visible characters up "
                          "to U+00FF: no line breaks, no other control characters");
      }
    } else if (p.name == "headers") {
      this->checkHeadersInit(value);
    }
  }
}

void HttpChecker::checkRequestNew(NewExpr *expr) {
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
      if (!UrlChecker::validUrlText(*text, false)) {
        this->state.getDiagnostics().error(input->loc(), "'" + *text + "' is not a valid URL");
      } else if (HttpChecker::hasCredentials(*text)) {
        this->state.getDiagnostics().error(input->loc(),
                                           "a request cannot go to a URL with a user name or "
                                           "a password in it, as in node: send them in a "
                                           "header, e.g. 'authorization'");
      }
    } else if (!UrlChecker(this->state).knownAbsolute(input)) {
      this->state.getDiagnostics().error(
          input->loc(), "'new Request' of a text known only when the program runs may fail, "
                        "and 'new' gives no Result; parse the text with 'URL.parse(text)' "
                        "and pass the URL, or give the text to fetch, which gives a Result");
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
  bool knownMethod = !HttpChecker::isLibraryClass(input->type, "Request");
  if (const ObjectLitExpr::Property *p = HttpChecker::property(init, "method")) {
    knownMethod = false;
    if (const std::u16string *text = HttpChecker::stringLiteral(p->value)) {
      method = HttpChecker::utf8(*text);
      std::string upper = llvm::StringRef(method).upper();
      knownMethod = true;
      if (!HttpChecker::isToken(method)) {
        this->state.getDiagnostics().error(p->value->loc(),
                                           "'" + method + "' is not a valid HTTP method");
      } else if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK") {
        this->state.getDiagnostics().error(
            p->value->loc(), "fetch does not make requests with method '" + method + "'");
      }
      method = upper;
    }
  }
  const ObjectLitExpr::Property *body = HttpChecker::property(init, "body");
  if (body && knownMethod && (method == "GET" || method == "HEAD") &&
      HttpChecker::hasBody(body->value)) {
    this->state.getDiagnostics().error(body->value->loc(),
                                       "a " + method + " request has no body" +
                                           (HttpChecker::property(init, "method")
                                                ? ""
                                                : " (a request without 'method' is a GET)") +
                                           "; give the method, e.g. 'method: \"POST\"'");
  }
  if (const ObjectLitExpr::Property *headers = HttpChecker::property(init, "headers")) {
    this->checkHeadersInit(headers->value);
  }
}

}
