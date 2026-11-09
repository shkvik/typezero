#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

UrlChecker::UrlChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->libraryCalls = &all.getLibraryCalls();
}

bool UrlChecker::isLibraryClass(const Type *type, llvm::StringRef name) {
  const ClassDecl *cls = type ? type->nonOptional()->classDecl() : nullptr;

  return cls && cls->library() && cls->name == name;
}

const Expr *UrlChecker::unconverted(const Expr *expr) {
  while (auto *conv = dyn_cast<CastExpr>(expr)) {
    if (!conv->isImplicit()) {
      break;
    }
    expr = conv->operand;
  }

  return expr;
}

std::optional<std::string> UrlChecker::literalText(const Expr *expr) {
  std::u16string units;
  if (auto *lit = dyn_cast<StringLitExpr>(expr)) {
    units = lit->value;
  } else if (auto *tmpl = dyn_cast<TemplateExpr>(expr); tmpl && tmpl->exprs.empty()) {
    units = tmpl->quasis[0];
  } else {
    return std::nullopt;
  }
  std::string text = ToUtf8::toUtf8(units);

  return text;
}

bool UrlChecker::validHost(llvm::StringRef host) {
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

bool UrlChecker::validUrlText(llvm::StringRef text, bool relative) {
  text = text.trim(" \t\n\r\f\v");
  size_t colon = text.find(':');
  bool hasScheme = colon != llvm::StringRef::npos && colon > 0 && llvm::isAlpha(text[0]);
  for (size_t i = 0; hasScheme && i < colon; ++i) {
    char c = text[i];
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
  if (size_t at = authority.rfind('@'); at != llvm::StringRef::npos) {
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
  if (!UrlChecker::validHost(host)) {
    return false;
  }
  if (!llvm::all_of(port, llvm::isDigit)) {
    return false;
  }
  unsigned long long number = 0;

  return port.empty() || (!port.getAsInteger(10, number) && number <= 65535);
}

bool UrlChecker::knownAbsolute(const Expr *expr) const {
  expr = UrlChecker::unconverted(expr);
  if (std::optional<std::string> text = UrlChecker::literalText(expr)) {
    return UrlChecker::validUrlText(*text, false);
  }
  if (UrlChecker::isLibraryClass(expr->type, "URL")) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    return member->name == "url" && UrlChecker::isLibraryClass(member->object->type, "Request");
  }
  if (auto *call = dyn_cast<CallExpr>(expr); call && call->args.empty()) {
    auto *member = dyn_cast<MemberExpr>(call->callee);
    return member && member->name == "href" &&
           UrlChecker::isLibraryClass(member->object->type, "URL");
  }

  return false;
}

UrlChecker::ResultKnownBaseUrl UrlChecker::knownBase(const Expr *base) const {
  if (!this->knownAbsolute(base)) {
    std::string message = ErrorTextUrlNew::base();

    return err(UrlCheckerKnownBaseError{
        .code = UrlCheckerKnownBaseErrorCode::UNKNOWN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

UrlChecker::ResultUrlTextValid UrlChecker::urlText(llvm::StringRef text, bool based) {
  if (!UrlChecker::validUrlText(text, based)) {
    std::string message = ErrorTextUrlNew::invalid(text, based);

    return err(UrlCheckerUrlTextError{
        .code = UrlCheckerUrlTextErrorCode::INVALID_URL,
        .message = message,
    });
  }

  return ok(text);
}

UrlChecker::ResultInputIsKnown UrlChecker::knownInput(const Expr *input, bool based) const {
  if (!this->knownAbsolute(input)) {
    std::string message = ErrorTextUrlNew::input(based);

    return err(UrlCheckerKnownInputError{
        .code = UrlCheckerKnownInputErrorCode::UNKNOWN_INPUT,
        .message = message,
    });
  }

  return ok(input);
}

void UrlChecker::checkUrlNew(NewExpr *expr) {
  if (this->libraryCalls->inLibrary(expr->loc())) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() ? expr->ctor()->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const Expr *input = UrlChecker::unconverted(args[0]);
  bool based = args.size() > 1;
  if (based) {
    ResultKnownBaseUrl base = this->knownBase(args[1]);
    if (base.isErr()) {
      this->state.getDiagnostics().error(args[1]->loc(), base.error().message);
      return;
    }
  }
  std::optional<std::string> text = UrlChecker::literalText(input);
  if (text) {
    ResultUrlTextValid valid = UrlChecker::urlText(*text, based);
    if (valid.isErr()) {
      this->state.getDiagnostics().error(input->loc(), valid.error().message);
    }
    return;
  }
  ResultInputIsKnown known = this->knownInput(input, based);
  if (known.isErr()) {
    this->state.getDiagnostics().error(input->loc(), known.error().message);
  }
}

bool UrlChecker::checkUrlSetter(MemberExpr *member, const CallExpr *call) {
  if (call->args.size() != 1 || !UrlChecker::isLibraryClass(member->object->type, "URL")) {
    return false;
  }
  llvm::StringRef name = member->name;
  if (name != "setHref" && name != "setProtocol" && name != "setHost" && name != "setHostname" &&
      name != "setPort") {
    return false;
  }
  const Expr *input = call->args[0];
  std::optional<std::string> text = UrlChecker::literalText(input);
  if (!text) {
    return false;
  }
  ResultSettableText value = UrlChecker::setterValue(name, *text);
  if (value.isErr()) {
    this->state.getDiagnostics().error(input->loc(), value.error().message);
    return false;
  }

  return true;
}

UrlChecker::ResultSettableText UrlChecker::setterValue(llvm::StringRef name,
                                                       llvm::StringRef value) {
  if (name == "setHref" && !UrlChecker::validUrlText(value, false)) {
    std::string message = ErrorTextUrlSetter::href(value);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_URL,
        .message = message,
    });
  }
  if (name == "setProtocol") {
    llvm::StringRef scheme = value;
    scheme.consume_back(":");
    auto schemeChar = [](char c) { return llvm::isAlnum(c) || c == '+' || c == '-' || c == '.'; };
    bool good = !scheme.empty() && llvm::isAlpha(scheme[0]) && llvm::all_of(scheme, schemeChar);
    if (!good) {
      std::string message = ErrorTextUrlSetter::protocol(value);

      return err(UrlCheckerSetterValueError{
          .code = UrlCheckerSetterValueErrorCode::NOT_A_PROTOCOL,
          .message = message,
      });
    }
  }
  if (name == "setPort") {
    unsigned long long number = 0;
    bool wrong = !value.empty() && (!llvm::all_of(value, llvm::isDigit) ||
                                    value.getAsInteger(10, number) || number > 65535);
    if (wrong) {
      std::string message = ErrorTextUrlSetter::port(value);

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
    bool wrong = !UrlChecker::validHost(host) || !llvm::all_of(port, llvm::isDigit) ||
                 (!port.empty() && (port.getAsInteger(10, number) || number > 65535));
    if (wrong) {
      std::string message = ErrorTextUrlSetter::host(value, name == "setHost");

      return err(UrlCheckerSetterValueError{
          .code = UrlCheckerSetterValueErrorCode::NOT_A_HOST,
          .message = message,
      });
    }
  }

  return ok(value);
}

}
