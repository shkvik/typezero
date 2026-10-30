#include "compiler/library/tz_modules/http_checker/http_checker.h"
#include "compiler/declare/constant_folder/constant_folder.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "compiler/library/tz_modules/url_checker/url_checker.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

bool isTokenChar(unsigned char c) {
  return c < 0x80 &&
         (llvm::isAlnum(char(c)) || llvm::StringRef("!#$%&'*+-.^_`|~").contains(char(c)));
}

bool isToken(llvm::StringRef text) {
  return !text.empty() &&
         llvm::all_of(text, [](char c) { return isTokenChar(static_cast<unsigned char>(c)); });
}

bool isHttpSpace(char16_t c) { return c == 0x20 || c == 0x09 || c == 0x0A || c == 0x0D; }

bool validValue(const std::u16string &text) {
  size_t start = 0, end = text.size();
  while (start < end && isHttpSpace(text[start])) {
    ++start;
  }
  while (end > start && isHttpSpace(text[end - 1])) {
    --end;
  }
  for (size_t i = start; i < end; ++i) {
    if (text[i] == 0 || text[i] == 0x0A || text[i] == 0x0D) {
      return false;
    }
  }

  return true;
}

const std::u16string *stringLiteral(const Expr *expr) {
  if (auto *lit = dyn_cast<StringLitExpr>(UrlChecker::unconverted(expr))) {
    return &lit->value;
  }
  if (auto *tmpl = dyn_cast<TemplateExpr>(UrlChecker::unconverted(expr));
      tmpl && tmpl->exprs.empty()) {
    return &tmpl->quasis[0];
  }

  return nullptr;
}

std::string utf8(const std::u16string &units) {
  std::string text;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(units.data()),
                                  units.size()),
      text);

  return text;
}

bool isLibraryClass(const Type *type, llvm::StringRef name) {
  const ClassDecl *cls = type ? type->nonOptional()->classDecl() : nullptr;

  return cls && cls->library && cls->name == name;
}

const ObjectLitExpr::Property *property(const ObjectLitExpr *lit, llvm::StringRef name) {
  for (const ObjectLitExpr::Property &p : lit->props) {
    if (!p.spread && !p.key && p.value && p.name == name) {
      return &p;
    }
  }

  return nullptr;
}

bool hasBody(const Expr *body) {
  const Type *type = UrlChecker::unconverted(body)->type;

  return !type->isError() && !type->isUndefined() && !type->isOptional();
}

bool hasCredentials(llvm::StringRef url) {
  size_t scheme = url.find("://");
  if (scheme == llvm::StringRef::npos) {
    return false;
  }
  llvm::StringRef rest = url.drop_front(scheme + 3);
  llvm::StringRef authority =
      rest.take_until([](char c) { return c == '/' || c == '\\' || c == '?' || c == '#'; });

  return authority.contains('@');
}

}

HttpChecker::HttpChecker(Checker &checker) : checker_(checker) {}

bool HttpChecker::validReason(const std::u16string &text) {
  return llvm::all_of(text, [](char16_t c) {
    return c == 0x09 || (c >= 0x20 && c <= 0x7E) || (c >= 0x80 && c <= 0xFF);
  });
}

bool HttpChecker::inProgram(const Expr *expr) const {
  return !checker_.libraryCalls().inLibrary(expr->loc());
}

void HttpChecker::checkHeaderName(const Expr *name) {
  const std::u16string *text = stringLiteral(name);
  if (text && !isToken(utf8(*text))) {
    checker_.diags_.error(name->loc(), "'" + utf8(*text) +
                                           "' is not a name of a header: a name is letters, "
                                           "digits and !#$%&'*+-.^_`|~, without spaces");
  }
}

void HttpChecker::checkHeaderValue(const Expr *value) {
  const std::u16string *text = stringLiteral(value);
  if (text && !validValue(*text)) {
    checker_.diags_.error(value->loc(), "the value of a header cannot hold a line break or NUL");
  }
}

void HttpChecker::checkHeadersInit(const Expr *init) {
  init = UrlChecker::unconverted(init);
  if (auto *lit = dyn_cast<ObjectLitExpr>(init)) {
    for (const ObjectLitExpr::Property &p : lit->props) {
      if (p.spread || p.key || !p.value) {
        continue;
      }
      if (!isToken(p.name)) {
        checker_.diags_.error(p.loc, "'" + p.name.str() +
                                         "' is not a name of a header: a name is letters, "
                                         "digits and !#$%&'*+-.^_`|~, without spaces");
      }
      checkHeaderValue(p.value);
    }
    return;
  }
  if (auto *list = dyn_cast<ArrayLitExpr>(init)) {
    for (const ArrayLitExpr::Element &e : list->elements) {
      auto *pair =
          e.spread || !e.expr ? nullptr : dyn_cast<ArrayLitExpr>(UrlChecker::unconverted(e.expr));
      if (pair && pair->elements.size() == 2 && !pair->elements[0].spread &&
          !pair->elements[1].spread) {
        checkHeaderName(pair->elements[0].expr);
        checkHeaderValue(pair->elements[1].expr);
      }
    }
  }
}

void HttpChecker::checkHeadersNew(NewExpr *expr) {
  const std::vector<Expr *> &args = expr->ctor ? expr->ctor->args : expr->args;
  if (inProgram(expr) && !args.empty()) {
    checkHeadersInit(args[0]);
  }
}

void HttpChecker::checkHeadersCall(const CallExpr *call, const MemberExpr *member) {
  if (!isLibraryClass(member->object->type, "Headers") || !inProgram(call) || call->args.empty()) {
    return;
  }
  llvm::StringRef name = member->name;
  if (name == "append" || name == "set" || name == "get" || name == "has" || name == "delete") {
    checkHeaderName(call->args[0]);
  }
  if ((name == "append" || name == "set") && call->args.size() > 1) {
    checkHeaderValue(call->args[1]);
  }
}

void HttpChecker::checkResponseNew(NewExpr *expr) {
  if (!inProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor ? expr->ctor->args : expr->args;
  if (args.size() < 2) {
    return;
  }
  auto *init = dyn_cast<ObjectLitExpr>(UrlChecker::unconverted(args[1]));
  if (!init) {
    return;
  }
  for (const ObjectLitExpr::Property &p : init->props) {
    if (p.spread || p.key || !p.value) {
      continue;
    }
    const Expr *value = UrlChecker::unconverted(p.value);
    if (p.name == "status") {
      std::optional<int64_t> status =
          value->type && value->type->isLiteral() && value->type->literalBase()->isInt()
              ? std::optional<int64_t>(int64_t(value->type->literalNumber()))
              : checker_.constantFolder().intConstant(value);
      if (status && (*status == 204 || *status == 205 || *status == 304) && hasBody(args[0])) {
        std::string code = std::to_string(*status);
        checker_.diags_.error(args[0]->loc(), "a response with status " + code +
                                                  " has no body; write 'new Response(undefined, "
                                                  "{ status: " +
                                                  code + " })'");
      }
    } else if (p.name == "statusText") {
      if (const std::u16string *text = stringLiteral(value); text && !validReason(*text)) {
        checker_.diags_.error(value->loc(),
                              "a statusText holds only tabs, spaces and visible characters up "
                              "to U+00FF: no line breaks, no other control characters");
      }
    } else if (p.name == "headers") {
      checkHeadersInit(value);
    }
  }
}

void HttpChecker::checkRequestNew(NewExpr *expr) {
  if (!inProgram(expr)) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor ? expr->ctor->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const Expr *input = UrlChecker::unconverted(args[0]);
  if (!isLibraryClass(input->type, "Request") && !isLibraryClass(input->type, "URL")) {
    if (std::optional<std::string> text = UrlChecker::literalText(input)) {
      if (!UrlChecker::validUrlText(*text, false)) {
        checker_.diags_.error(input->loc(), "'" + *text + "' is not a valid URL");
      } else if (hasCredentials(*text)) {
        checker_.diags_.error(input->loc(), "a request cannot go to a URL with a user name or "
                                            "a password in it, as in node: send them in a "
                                            "header, e.g. 'authorization'");
      }
    } else if (!checker_.urlChecker().knownAbsolute(input)) {
      checker_.diags_.error(input->loc(),
                            "'new Request' of a text known only when the program runs may fail, "
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
  bool knownMethod = !isLibraryClass(input->type, "Request");
  if (const ObjectLitExpr::Property *p = property(init, "method")) {
    knownMethod = false;
    if (const std::u16string *text = stringLiteral(p->value)) {
      method = utf8(*text);
      std::string upper = llvm::StringRef(method).upper();
      knownMethod = true;
      if (!isToken(method)) {
        checker_.diags_.error(p->value->loc(), "'" + method + "' is not a valid HTTP method");
      } else if (upper == "CONNECT" || upper == "TRACE" || upper == "TRACK") {
        checker_.diags_.error(p->value->loc(),
                              "fetch does not make requests with method '" + method + "'");
      }
      method = upper;
    }
  }
  const ObjectLitExpr::Property *body = property(init, "body");
  if (body && knownMethod && (method == "GET" || method == "HEAD") && hasBody(body->value)) {
    checker_.diags_.error(
        body->value->loc(),
        "a " + method + " request has no body" +
            (property(init, "method") ? "" : " (a request without 'method' is a GET)") +
            "; give the method, e.g. 'method: \"POST\"'");
  }
  if (const ObjectLitExpr::Property *headers = property(init, "headers")) {
    checkHeadersInit(headers->value);
  }
}

}
