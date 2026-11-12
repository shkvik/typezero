#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

UrlChecker::UrlChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->libraryCalls = &all.libraryCalls;
}

bool UrlChecker::isLibraryClass(const Type *type, llvm::StringRef name) const {
  const ClassDecl *classDecl = type != nullptr ? type->nonOptional()->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() && classDecl->name == name;
}

const Expr *UrlChecker::unconverted(const Expr *expr) const {
  expr = ExprParts::withoutImplicitCasts(expr);

  return expr;
}

std::optional<std::string> UrlChecker::literalText(const Expr *expr) const {
  auto units = std::u16string();
  auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    units = literal->value;
  } else if (auto *templateExpr = dyn_cast<TemplateExpr>(expr);
             templateExpr != nullptr && templateExpr->exprs.empty()) {
    units = templateExpr->quasis[0];
  } else {
    return std::nullopt;
  }
  auto text = ToUtf8::toUtf8(units);

  return text;
}

bool UrlChecker::validHost(llvm::StringRef host) const {
  if (host.empty()) {
    return false;
  }
  if (host.front() == '[') {
    if (host.back() != ']' || host.size() < 3) {
      return false;
    }
    for (char c : host.drop_front().drop_back()) {
      if (!llvm::isHexDigit(c) && c != ':' && c != '.') {
        return false;
      }
    }
    return true;
  }
  for (unsigned char c : host) {
    if (c <= ' ' || c == 0x7F || llvm::StringRef("#%/:<>?@[\\]^|").contains(char(c))) {
      return false;
    }
  }

  return true;
}

bool UrlChecker::validUrlText(llvm::StringRef text, bool relative) const {
  text = text.trim(" \t\n\r\f\v");
  size_t colon = text.find(':');
  bool hasScheme = colon != llvm::StringRef::npos && colon > 0 && llvm::isAlpha(text[0]);
  for (size_t index = 0; hasScheme && index < colon; ++index) {
    char c = text[index];
    hasScheme = llvm::isAlnum(c) || c == '+' || c == '-' || c == '.';
  }
  if (!hasScheme) {
    return relative;
  }
  std::string scheme = text.take_front(colon).lower();
  static const llvm::StringLiteral special[] = {"http", "https", "ws", "wss", "ftp"};
  if (!llvm::is_contained(special, scheme)) {
    return true;
  }
  llvm::StringRef rest = text.drop_front(colon + 1).ltrim("/\\");
  llvm::StringRef authority =
      rest.take_until([](char c) { return c == '/' || c == '\\' || c == '?' || c == '#'; });
  size_t at = authority.rfind('@');
  if (at != llvm::StringRef::npos) {
    authority = authority.drop_front(at + 1);
  }
  llvm::StringRef host = authority;
  llvm::StringRef port;
  if (!authority.starts_with("[")) {
    std::tie(host, port) = authority.split(':');
  } else if (size_t close = authority.find(']'); close != llvm::StringRef::npos) {
    host = authority.take_front(close + 1);
    port = authority.drop_front(close + 1);
    if (!port.empty() && !port.consume_front(":")) {
      return false;
    }
  }
  if (!this->validHost(host)) {
    return false;
  }
  if (!llvm::all_of(port, llvm::isDigit)) {
    return false;
  }
  unsigned long long number = 0;

  return port.empty() || (!port.getAsInteger(10, number) && number <= 65535);
}

bool UrlChecker::knownAbsolute(const Expr *expr) const {
  expr = this->unconverted(expr);
  if (std::optional<std::string> text = this->literalText(expr)) {
    return this->validUrlText(*text, false);
  }
  if (this->isLibraryClass(expr->type, "URL")) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return member->name == "url" && this->isLibraryClass(member->object->type, "Request");
  }
  auto *call = dyn_cast<CallExpr>(expr);
  if (call != nullptr && call->args.empty()) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return member != nullptr && member->name == "href" &&
           this->isLibraryClass(member->object->type, "URL");
  }

  return false;
}

UrlChecker::ResultKnownBaseUrl UrlChecker::knownBase(const Expr *base) const {
  if (!this->knownAbsolute(base)) {
    auto message = ErrorTextUrlNew::base();

    return err(UrlCheckerKnownBaseError{
        .code = UrlCheckerKnownBaseErrorCode::UNKNOWN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

UrlChecker::ResultUrlTextValid UrlChecker::urlText(llvm::StringRef text, bool based) const {
  if (!this->validUrlText(text, based)) {
    auto message = ErrorTextUrlNew::invalid(text, based);

    return err(UrlCheckerUrlTextError{
        .code = UrlCheckerUrlTextErrorCode::INVALID_URL,
        .message = message,
    });
  }

  return ok(text);
}

UrlChecker::ResultInputIsKnown UrlChecker::knownInput(const Expr *input, bool based) const {
  if (!this->knownAbsolute(input)) {
    auto message = ErrorTextUrlNew::input(based);

    return err(UrlCheckerKnownInputError{
        .code = UrlCheckerKnownInputErrorCode::UNKNOWN_INPUT,
        .message = message,
    });
  }

  return ok(input);
}

void UrlChecker::walkUrlNew(NewExpr *expr) {
  if (this->libraryCalls->inLibrary(expr->loc())) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const auto *input = this->unconverted(args[0]);
  bool isBased = args.size() > 1;
  if (isBased) {
    auto base = this->knownBase(args[1]);
    if (base.isErr()) {
      this->state.getDiagnostics().error(args[1]->loc(), base.error().message);
      return;
    }
  }
  auto text = this->literalText(input);
  if (text) {
    auto valid = this->urlText(*text, isBased);
    if (valid.isErr()) {
      this->state.getDiagnostics().error(input->loc(), valid.error().message);
    }
    return;
  }
  auto known = this->knownInput(input, isBased);
  if (known.isErr()) {
    this->state.getDiagnostics().error(input->loc(), known.error().message);
  }
}

bool UrlChecker::checkUrlSetter(MemberExpr *member, const CallExpr *call) {
  if (call->args.size() != 1 || !this->isLibraryClass(member->object->type, "URL")) {
    return false;
  }
  llvm::StringRef name = member->name;
  if (name != "setHref" && name != "setProtocol" && name != "setHost" && name != "setHostname" &&
      name != "setPort") {
    return false;
  }
  const Expr *input = call->args[0];
  auto text = this->literalText(input);
  if (!text) {
    return false;
  }
  auto value = this->setterValue(name, *text);
  if (value.isErr()) {
    this->state.getDiagnostics().error(input->loc(), value.error().message);
    return false;
  }

  return true;
}

UrlChecker::ResultSettableText UrlChecker::setterValue(llvm::StringRef name,
                                                       llvm::StringRef value) const {
  if (name == "setHref" && !this->validUrlText(value, false)) {
    auto message = ErrorTextUrlSetter::href(value);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_URL,
        .message = message,
    });
  }
  if (name == "setProtocol") {
    llvm::StringRef scheme = value;
    scheme.consume_back(":");
    auto schemeChar = [](char c) { return llvm::isAlnum(c) || c == '+' || c == '-' || c == '.'; };
    bool isGood = !scheme.empty() && llvm::isAlpha(scheme[0]) && llvm::all_of(scheme, schemeChar);
    if (!isGood) {
      auto message = ErrorTextUrlSetter::protocol(value);

      return err(UrlCheckerSetterValueError{
          .code = UrlCheckerSetterValueErrorCode::NOT_A_PROTOCOL,
          .message = message,
      });
    }
  }
  if (name == "setPort") {
    unsigned long long number = 0;
    bool isWrong = !value.empty() && (!llvm::all_of(value, llvm::isDigit) ||
                                      value.getAsInteger(10, number) || number > 65535);
    if (isWrong) {
      auto message = ErrorTextUrlSetter::port(value);

      return err(UrlCheckerSetterValueError{
          .code = UrlCheckerSetterValueErrorCode::NOT_A_PORT,
          .message = message,
      });
    }
  }
  if (name == "setHost" || name == "setHostname") {
    llvm::StringRef host = value;
    llvm::StringRef port;
    if (name == "setHost" && !value.starts_with("[")) {
      std::tie(host, port) = value.split(':');
    } else if (name == "setHost") {
      size_t close = value.find(']');
      host = value.take_front(close == llvm::StringRef::npos ? value.size() : close + 1);
      port = value.drop_front(host.size());
      port.consume_front(":");
    }
    unsigned long long number = 0;
    bool isWrong = !this->validHost(host) || !llvm::all_of(port, llvm::isDigit) ||
                   (!port.empty() && (port.getAsInteger(10, number) || number > 65535));
    if (isWrong) {
      auto message = ErrorTextUrlSetter::host(value, name == "setHost");

      return err(UrlCheckerSetterValueError{
          .code = UrlCheckerSetterValueErrorCode::NOT_A_HOST,
          .message = message,
      });
    }
  }

  return ok(value);
}

}
