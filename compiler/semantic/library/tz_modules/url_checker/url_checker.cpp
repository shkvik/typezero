#include "compiler/semantic/library/tz_modules/url_checker/url_checker.h"
#include "compiler/program/library/url/url_scheme_table/url_scheme_table.h"
#include "compiler/program/library/url/web_member_table/web_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/library/tz_modules/library_calls/library_calls.h"
#include "compiler/semantic/semantic_parts/semantic_parts.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kUrlSpaces = " \t\n\r\f\v";
constexpr llvm::StringLiteral kHostForbidden = "#%/:<>?@[\\]^|";
constexpr unsigned long long kHighestPort = 65535;

}

UrlChecker::UrlChecker(SemanticState &owner, SemanticParts &all)
    : state(owner) {
  this->libraryCalls = &all.libraryCalls;
}

bool UrlChecker::isLibraryClass(const Type *type, LibraryClass libraryClass) const {
  const ClassDecl *classDecl = type != nullptr ? type->nonOptional()->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, libraryClass);
}

const Expr *UrlChecker::unconverted(const Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

std::optional<std::string> UrlChecker::literalText(const Expr *expr) const {
  auto units = this->literalUnits(expr);
  if (!units.has_value()) {
    return std::nullopt;
  }

  return ToUtf8::toUtf8(units.value());
}

std::optional<std::u16string> UrlChecker::literalUnits(const Expr *expr) const {
  const auto *literal = dyn_cast<StringLitExpr>(expr);
  if (literal != nullptr) {
    return literal->value;
  }
  const auto *templateExpr = dyn_cast<TemplateExpr>(expr);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return templateExpr->quasis[0];
  }

  return std::nullopt;
}

bool UrlChecker::isValidHost(llvm::StringRef host) const {
  if (host.empty()) {
    return false;
  }
  if (host.front() == '[') {
    return this->isValidBracketHost(host);
  }
  auto isAllowed = std::bind_front(&UrlChecker::isHostChar, this);

  return llvm::all_of(host, isAllowed);
}

bool UrlChecker::isValidBracketHost(llvm::StringRef host) const {
  if (host.back() != ']' || host.size() < 3) {
    return false;
  }
  auto isAddress = std::bind_front(&UrlChecker::isAddressChar, this);

  return llvm::all_of(host.drop_front().drop_back(), isAddress);
}

bool UrlChecker::isAddressChar(char character) const {
  return llvm::isHexDigit(character) || character == ':' || character == '.';
}

bool UrlChecker::isHostChar(char character) const {
  auto code = static_cast<unsigned char>(character);

  return code > ' ' && code != 0x7F && !kHostForbidden.contains(character);
}

bool UrlChecker::isSchemeChar(char character) const {
  return llvm::isAlnum(character) || character == '+' || character == '-' || character == '.';
}

bool UrlChecker::isAuthorityEnd(char character) const {
  return character == '/' || character == '\\' || character == '?' || character == '#';
}

bool UrlChecker::isValidPort(llvm::StringRef port) const {
  unsigned long long number = 0;

  return port.empty() || (!port.getAsInteger(10, number) && number <= kHighestPort);
}

bool UrlChecker::isValidUrlText(llvm::StringRef text, RelativeUrls relative) const {
  llvm::StringRef trimmed = text.trim(kUrlSpaces);
  size_t colon = trimmed.find(':');
  if (!this->hasScheme(trimmed, colon)) {
    return relative == RelativeUrls::ALLOWED;
  }
  std::string scheme = trimmed.take_front(colon).lower();
  if (!UrlSchemeTable::isSpecial(scheme)) {
    return true;
  }
  llvm::StringRef rest = trimmed.drop_front(colon + 1).ltrim("/\\");
  auto isEnd = std::bind_front(&UrlChecker::isAuthorityEnd, this);
  llvm::StringRef authority = rest.take_until(isEnd);
  size_t at = authority.rfind('@');
  llvm::StringRef hostPort = at != llvm::StringRef::npos ? authority.drop_front(at + 1) : authority;
  auto split = this->splitHostPort(hostPort);
  if (!split.has_value() || !this->isValidHost(split->host)) {
    return false;
  }
  if (!llvm::all_of(split->port, llvm::isDigit)) {
    return false;
  }

  return this->isValidPort(split->port);
}

bool UrlChecker::hasScheme(llvm::StringRef text, size_t colon) const {
  if (colon == llvm::StringRef::npos || colon == 0 || !llvm::isAlpha(text[0])) {
    return false;
  }
  auto isScheme = std::bind_front(&UrlChecker::isSchemeChar, this);

  return llvm::all_of(text.take_front(colon), isScheme);
}

std::optional<UrlChecker::HostPort> UrlChecker::splitHostPort(llvm::StringRef authority) const {
  if (!authority.starts_with("[")) {
    auto [host, port] = authority.split(':');
    return HostPort{.host = host, .port = port};
  }
  size_t close = authority.find(']');
  if (close == llvm::StringRef::npos) {
    return HostPort{.host = authority, .port = llvm::StringRef()};
  }
  llvm::StringRef host = authority.take_front(close + 1);
  llvm::StringRef port = authority.drop_front(close + 1);
  if (!port.empty() && !port.consume_front(":")) {
    return std::nullopt;
  }

  return HostPort{.host = host, .port = port};
}

bool UrlChecker::isKnownAbsolute(const Expr *expr) const {
  const auto *bare = this->unconverted(expr);
  auto text = this->literalText(bare);
  if (text.has_value()) {
    return this->isValidUrlText(*text, RelativeUrls::REJECTED);
  }
  if (this->isLibraryClass(bare->type, LibraryClass::URL)) {
    return true;
  }
  const auto *member = dyn_cast<MemberExpr>(bare);
  if (member != nullptr) {
    return WebMemberTable::find(member->name) == WebMember::URL_FIELD &&
           this->isLibraryClass(member->object->type, LibraryClass::REQUEST);
  }
  const auto *call = dyn_cast<CallExpr>(bare);
  if (call != nullptr && call->args.empty()) {
    const auto *callee = dyn_cast<MemberExpr>(call->callee);
    return callee != nullptr && WebMemberTable::find(callee->name) == WebMember::HREF &&
           this->isLibraryClass(callee->object->type, LibraryClass::URL);
  }

  return false;
}

UrlChecker::ResultKnownBaseUrl UrlChecker::checkKnownBase(const Expr *base) const {
  if (!this->isKnownAbsolute(base)) {
    auto message = ErrorTextUrlNew::base();

    return err(UrlCheckerKnownBaseError{
        .code = UrlCheckerKnownBaseErrorCode::UNKNOWN_BASE,
        .message = message,
    });
  }

  return ok(base);
}

UrlChecker::ResultUrlTextValid UrlChecker::checkUrlText(llvm::StringRef text, bool isBased) const {
  auto relative = isBased ? RelativeUrls::ALLOWED : RelativeUrls::REJECTED;
  if (!this->isValidUrlText(text, relative)) {
    auto message = ErrorTextUrlNew::invalid(text, isBased);

    return err(UrlCheckerUrlTextError{
        .code = UrlCheckerUrlTextErrorCode::INVALID_URL,
        .message = message,
    });
  }

  return ok(text);
}

UrlChecker::ResultInputIsKnown UrlChecker::checkKnownInput(const Expr *input, bool isBased) const {
  if (!this->isKnownAbsolute(input)) {
    auto message = ErrorTextUrlNew::input(isBased);

    return err(UrlCheckerKnownInputError{
        .code = UrlCheckerKnownInputErrorCode::UNKNOWN_INPUT,
        .message = message,
    });
  }

  return ok(input);
}

void UrlChecker::walkUrlNew(NewExpr *expr) {
  if (this->libraryCalls->isInLibrary(expr->loc())) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor() != nullptr ? expr->ctor()->args : expr->args;
  auto isFailed = std::bind_front(&UrlChecker::isFailedArg, this);
  if (args.empty() || llvm::any_of(args, isFailed)) {
    return;
  }
  const auto *input = this->unconverted(args[0]);
  bool isBased = args.size() > 1;
  if (isBased) {
    auto base = this->checkKnownBase(args[1]);
    if (base.isErr()) {
      this->state.getDiagnostics().error(args[1]->loc(), base.error().message);
      return;
    }
  }
  auto text = this->literalText(input);
  if (text.has_value()) {
    auto valid = this->checkUrlText(*text, isBased);
    if (valid.isErr()) {
      this->state.getDiagnostics().error(input->loc(), valid.error().message);
    }
    return;
  }
  auto known = this->checkKnownInput(input, isBased);
  if (known.isErr()) {
    this->state.getDiagnostics().error(input->loc(), known.error().message);
  }
}

bool UrlChecker::isFailedArg(const Expr *arg) const {
  return arg->type->isError();
}

bool UrlChecker::proveUrlSetter(MemberExpr *member, const CallExpr *call) {
  if (call->args.size() != 1 || !this->isLibraryClass(member->object->type, LibraryClass::URL)) {
    return false;
  }
  auto setter = UrlSetterTable::find(member->name);
  if (!setter.has_value()) {
    return false;
  }
  const Expr *input = call->args[0];
  auto text = this->literalText(input);
  if (!text.has_value()) {
    return false;
  }
  auto value = this->checkSetterValue(setter.value(), *text);
  if (value.isErr()) {
    this->state.getDiagnostics().error(input->loc(), value.error().message);
    return false;
  }

  return true;
}

UrlChecker::ResultSettableText UrlChecker::checkSetterValue(UrlSetter setter,
                                                            llvm::StringRef value) const {
  switch (setter) {
    case UrlSetter::HREF:
      return this->checkHrefValue(value);
    case UrlSetter::PROTOCOL:
      return this->checkProtocolValue(value);
    case UrlSetter::PORT:
      return this->checkPortValue(value);
    case UrlSetter::HOST:
    case UrlSetter::HOSTNAME:
      return this->checkHostValue(setter, value);
  }

  return ok(value);
}

UrlChecker::ResultSettableText UrlChecker::checkHrefValue(llvm::StringRef value) const {
  if (!this->isValidUrlText(value, RelativeUrls::REJECTED)) {
    auto message = ErrorTextUrlSetter::href(value);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_URL,
        .message = message,
    });
  }

  return ok(value);
}

UrlChecker::ResultSettableText UrlChecker::checkProtocolValue(llvm::StringRef value) const {
  llvm::StringRef scheme = value;
  scheme.consume_back(":");
  auto isScheme = std::bind_front(&UrlChecker::isSchemeChar, this);
  bool isGood = !scheme.empty() && llvm::isAlpha(scheme[0]) && llvm::all_of(scheme, isScheme);
  if (!isGood) {
    auto message = ErrorTextUrlSetter::protocol(value);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_PROTOCOL,
        .message = message,
    });
  }

  return ok(value);
}

UrlChecker::ResultSettableText UrlChecker::checkPortValue(llvm::StringRef value) const {
  bool isWrong =
      !value.empty() && (!llvm::all_of(value, llvm::isDigit) || !this->isValidPort(value));
  if (isWrong) {
    auto message = ErrorTextUrlSetter::port(value);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_PORT,
        .message = message,
    });
  }

  return ok(value);
}

UrlChecker::ResultSettableText UrlChecker::checkHostValue(UrlSetter setter,
                                                          llvm::StringRef value) const {
  bool isHost = setter == UrlSetter::HOST;
  auto split = isHost ? this->settableHostPort(value) : HostPort{.host = value, .port = {}};
  bool isWrong = !this->isValidHost(split.host) || !llvm::all_of(split.port, llvm::isDigit) ||
                 !this->isValidPort(split.port);
  if (isWrong) {
    auto message = ErrorTextUrlSetter::host(value, isHost);

    return err(UrlCheckerSetterValueError{
        .code = UrlCheckerSetterValueErrorCode::NOT_A_HOST,
        .message = message,
    });
  }

  return ok(value);
}

UrlChecker::HostPort UrlChecker::settableHostPort(llvm::StringRef value) const {
  if (!value.starts_with("[")) {
    auto [host, port] = value.split(':');
    return HostPort{.host = host, .port = port};
  }
  size_t close = value.find(']');
  llvm::StringRef host =
      value.take_front(close == llvm::StringRef::npos ? value.size() : close + 1);
  llvm::StringRef port = value.drop_front(host.size());
  port.consume_front(":");

  return HostPort{.host = host, .port = port};
}

}
