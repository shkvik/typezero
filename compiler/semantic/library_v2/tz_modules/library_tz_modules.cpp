#include "compiler/semantic/library_v2/tz_modules/library_tz_modules.h"
#include "compiler/program/library/builtin_names/library_class_table/library_class_table.h"
#include "compiler/program/library/url/url_scheme_table/url_scheme_table.h"
#include "compiler/program/library/url/web_member_table/web_member_table.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include <functional>

using llvm::dyn_cast;

namespace tz {

namespace {

constexpr llvm::StringLiteral kUrlSpaces = " \t\n\r\f\v";
constexpr llvm::StringLiteral kHostForbidden = "#%/:<>?@[\\]^|";
constexpr unsigned long long kHighestPort = 65535;

}

LibraryTzModules::LibraryTzModules(SemanticState &owner)
    : state(owner) {}

std::string LibraryTzModules::knownPrefix(const Expr *expr) const {
  const Expr *source = expr;
  const auto *inner = this->prefixInner(source);
  while (inner != nullptr) {
    source = inner;
    inner = this->prefixInner(source);
  }

  return this->ownPrefix(source);
}

const Expr *LibraryTzModules::prefixInner(const Expr *expr) const {
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

const Expr *LibraryTzModules::addressArgument(const NewExpr *made) const {
  const ClassDecl *classDecl = made->cls();
  bool isLibrary = classDecl != nullptr && classDecl->library();
  bool isRequest = isLibrary && LibraryClassTable::isName(classDecl->name, LibraryClass::REQUEST);
  bool isUrl = isLibrary && LibraryClassTable::isName(classDecl->name, LibraryClass::URL);
  if (!isRequest && !isUrl) {
    return nullptr;
  }
  const std::vector<Expr *> &args = made->ctor() != nullptr ? made->ctor()->args : made->args;
  bool isGiven = !args.empty() && (isRequest || args.size() == 1);

  return isGiven ? args[0] : nullptr;
}

std::string LibraryTzModules::ownPrefix(const Expr *expr) const {
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
    return this->state.code.constants.findConstPrefix(name->var());
  }

  return std::string();
}

std::string LibraryTzModules::asciiPrefix(const std::u16string &text) const {
  auto out = std::string();
  for (char16_t unit : text) {
    if (unit >= 0x80) {
      break;
    }
    out += static_cast<char>(unit);
  }

  return out;
}

bool LibraryTzModules::proveUrlSetter(MemberExpr *member, const CallExpr *call) {
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
    this->state.diagnostics.error(input->loc(), value.error().message);
    return false;
  }

  return true;
}

LibraryTzModules::ResultSettableText
LibraryTzModules::checkSetterValue(UrlSetter setter, llvm::StringRef value) const {
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

LibraryTzModules::ResultSettableText LibraryTzModules::checkHrefValue(llvm::StringRef value) const {
  if (!this->isValidUrlText(value, RelativeUrls::REJECTED)) {
    auto message = ErrorTextUrlSetter::href(value);

    return err(LibraryTzModulesSetterValueError{
        .code = LibraryTzModulesSetterValueErrorCode::NOT_A_URL,
        .message = message,
    });
  }

  return ok(value);
}

LibraryTzModules::ResultSettableText
LibraryTzModules::checkProtocolValue(llvm::StringRef value) const {
  llvm::StringRef scheme = value;
  scheme.consume_back(":");
  auto isScheme = std::bind_front(&LibraryTzModules::isSchemeChar, this);
  bool isGood = !scheme.empty() && llvm::isAlpha(scheme[0]) && llvm::all_of(scheme, isScheme);
  if (!isGood) {
    auto message = ErrorTextUrlSetter::protocol(value);

    return err(LibraryTzModulesSetterValueError{
        .code = LibraryTzModulesSetterValueErrorCode::NOT_A_PROTOCOL,
        .message = message,
    });
  }

  return ok(value);
}

LibraryTzModules::ResultSettableText LibraryTzModules::checkPortValue(llvm::StringRef value) const {
  bool isWrong =
      !value.empty() && (!llvm::all_of(value, llvm::isDigit) || !this->isValidPort(value));
  if (isWrong) {
    auto message = ErrorTextUrlSetter::port(value);

    return err(LibraryTzModulesSetterValueError{
        .code = LibraryTzModulesSetterValueErrorCode::NOT_A_PORT,
        .message = message,
    });
  }

  return ok(value);
}

LibraryTzModules::ResultSettableText LibraryTzModules::checkHostValue(UrlSetter setter,
                                                                      llvm::StringRef value) const {
  bool isHost = setter == UrlSetter::HOST;
  auto split = isHost ? this->settableHostPort(value) : HostPort{.host = value, .port = {}};
  bool isWrong = !this->isValidHost(split.host) || !llvm::all_of(split.port, llvm::isDigit) ||
                 !this->isValidPort(split.port);
  if (isWrong) {
    auto message = ErrorTextUrlSetter::host(value, isHost);

    return err(LibraryTzModulesSetterValueError{
        .code = LibraryTzModulesSetterValueErrorCode::NOT_A_HOST,
        .message = message,
    });
  }

  return ok(value);
}

LibraryTzModules::HostPort LibraryTzModules::settableHostPort(llvm::StringRef value) const {
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

bool LibraryTzModules::isValidUrlText(llvm::StringRef text, RelativeUrls relative) const {
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
  auto isEnd = std::bind_front(&LibraryTzModules::isAuthorityEnd, this);
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

std::optional<std::string> LibraryTzModules::literalText(const Expr *expr) const {
  auto units = this->literalUnits(expr);
  if (!units.has_value()) {
    return std::nullopt;
  }

  return ToUtf8::toUtf8(units.value());
}

std::optional<std::u16string> LibraryTzModules::literalUnits(const Expr *expr) const {
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

bool LibraryTzModules::isKnownAbsolute(const Expr *expr) const {
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

const Expr *LibraryTzModules::unconverted(const Expr *expr) const {
  return ExprParts::withoutImplicitCasts(expr);
}

bool LibraryTzModules::isLibraryClass(const Type *type, LibraryClass libraryClass) const {
  const ClassDecl *classDecl = type != nullptr ? type->nonOptional()->classDecl() : nullptr;

  return classDecl != nullptr && classDecl->library() &&
         LibraryClassTable::isName(classDecl->name, libraryClass);
}

bool LibraryTzModules::isValidHost(llvm::StringRef host) const {
  if (host.empty()) {
    return false;
  }
  if (host.front() == '[') {
    return this->isValidBracketHost(host);
  }
  auto isAllowed = std::bind_front(&LibraryTzModules::isHostChar, this);

  return llvm::all_of(host, isAllowed);
}

bool LibraryTzModules::isValidBracketHost(llvm::StringRef host) const {
  if (host.back() != ']' || host.size() < 3) {
    return false;
  }
  auto isAddress = std::bind_front(&LibraryTzModules::isAddressChar, this);

  return llvm::all_of(host.drop_front().drop_back(), isAddress);
}

bool LibraryTzModules::isAddressChar(char character) const {
  return llvm::isHexDigit(character) || character == ':' || character == '.';
}

bool LibraryTzModules::isHostChar(char character) const {
  auto code = static_cast<unsigned char>(character);

  return code > ' ' && code != 0x7F && !kHostForbidden.contains(character);
}

bool LibraryTzModules::isSchemeChar(char character) const {
  return llvm::isAlnum(character) || character == '+' || character == '-' || character == '.';
}

bool LibraryTzModules::isAuthorityEnd(char character) const {
  return character == '/' || character == '\\' || character == '?' || character == '#';
}

bool LibraryTzModules::isValidPort(llvm::StringRef port) const {
  unsigned long long number = 0;

  return port.empty() || (!port.getAsInteger(10, number) && number <= kHighestPort);
}

bool LibraryTzModules::hasScheme(llvm::StringRef text, size_t colon) const {
  if (colon == llvm::StringRef::npos || colon == 0 || !llvm::isAlpha(text[0])) {
    return false;
  }
  auto isScheme = std::bind_front(&LibraryTzModules::isSchemeChar, this);

  return llvm::all_of(text.take_front(colon), isScheme);
}

std::optional<LibraryTzModules::HostPort>
LibraryTzModules::splitHostPort(llvm::StringRef authority) const {
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

}
