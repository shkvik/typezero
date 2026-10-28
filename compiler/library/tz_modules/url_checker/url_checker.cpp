#include "compiler/library/tz_modules/url_checker/url_checker.h"
#include "compiler/library/tz_modules/library_calls/library_calls.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"

using llvm::dyn_cast;

namespace tz {

namespace {

bool isLibraryClass(const Type *type, llvm::StringRef name) {
  const ClassDecl *cls = type ? type->nonOptional()->classDecl() : nullptr;

  return cls && cls->library && cls->name == name;
}

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
  std::string text;
  llvm::convertUTF16ToUTF8String(
      llvm::ArrayRef<llvm::UTF16>(reinterpret_cast<const llvm::UTF16 *>(units.data()),
                                  units.size()),
      text);

  return text;
}

namespace {

bool validHost(llvm::StringRef host) {
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

}

UrlChecker::UrlChecker(Checker &checker) : checker_(checker) {}

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
  llvm::StringRef host = authority, port;
  if (!authority.starts_with("[")) {
    std::tie(host, port) = authority.split(':');
  } else if (size_t close = authority.find(']'); close != llvm::StringRef::npos) {
    host = authority.take_front(close + 1);
    port = authority.drop_front(close + 1);
    if (!port.empty() && !port.consume_front(":")) {
      return false;
    }
  }
  if (!validHost(host)) {
    return false;
  }
  if (!llvm::all_of(port, llvm::isDigit)) {
    return false;
  }
  unsigned long long number = 0;

  return port.empty() || (!port.getAsInteger(10, number) && number <= 65535);
}

bool UrlChecker::knownAbsolute(const Expr *expr) const {
  expr = unconverted(expr);
  if (std::optional<std::string> text = literalText(expr)) {
    return validUrlText(*text, false);
  }
  if (isLibraryClass(expr->type, "URL")) {
    return true;
  }
  if (auto *member = dyn_cast<MemberExpr>(expr)) {
    const Type *object = member->object->type;
    return (member->name == "url" && isLibraryClass(object, "Request")) ||
           (member->name == "href" && isLibraryClass(object, "URL"));
  }

  return false;
}

void UrlChecker::checkUrlNew(NewExpr *expr) {
  if (checker_.libraryCalls().inLibrary(expr->loc())) {
    return;
  }
  const std::vector<Expr *> &args = expr->ctor ? expr->ctor->args : expr->args;
  if (args.empty() || llvm::any_of(args, [](const Expr *a) { return a->type->isError(); })) {
    return;
  }
  const Expr *input = unconverted(args[0]);
  if (args.size() > 1 && !knownAbsolute(args[1])) {
    checker_.diags_.error(args[1]->loc(),
                          "'new URL' with a base known only when the program runs may fail, and "
                          "'new' gives no Result; write 'URL.parse(input, base)', which gives a "
                          "Result<URL, UrlError>");
    return;
  }
  std::optional<std::string> text = literalText(input);
  if (text) {
    if (!validUrlText(*text, args.size() > 1)) {
      checker_.diags_.error(input->loc(), "'" + *text + "' is not a valid URL" +
                                              (args.size() > 1 ? "" : " without a base"));
    }
    return;
  }
  if (!knownAbsolute(input)) {
    checker_.diags_.error(input->loc(),
                          "'new URL' of a text known only when the program runs may fail, and "
                          "'new' gives no Result; write 'URL.parse(text" +
                              std::string(args.size() > 1 ? ", base" : "") +
                              ")', which gives a Result<URL, UrlError>");
  }
}

bool UrlChecker::checkUrlSetter(MemberExpr *member, const CallExpr *call) {
  if (call->args.size() != 1 || !isLibraryClass(member->object->type, "URL")) {
    return false;
  }
  llvm::StringRef name = member->name;
  if (name != "setHref" && name != "setProtocol" && name != "setHost" && name != "setHostname" &&
      name != "setPort") {
    return false;
  }
  const Expr *input = call->args[0];
  std::optional<std::string> text = literalText(input);
  if (!text) {
    return false;
  }
  llvm::StringRef value = *text;
  std::string problem;
  if (name == "setHref") {
    if (!validUrlText(value, false)) {
      problem = "'" + *text + "' is not a valid URL";
    }
  } else if (name == "setProtocol") {
    llvm::StringRef scheme = value;
    scheme.consume_back(":");
    bool good = !scheme.empty() && llvm::isAlpha(scheme[0]) && llvm::all_of(scheme, [](char c) {
      return llvm::isAlnum(c) || c == '+' || c == '-' || c == '.';
    });
    if (!good) {
      problem = "'" + *text +
                "' is not a protocol: a protocol is a letter, then letters, digits "
                "and +-., e.g. 'https:'";
    }
  } else if (name == "setPort") {
    unsigned long long number = 0;
    if (!value.empty() &&
        (!llvm::all_of(value, llvm::isDigit) || value.getAsInteger(10, number) || number > 65535)) {
      problem = "'" + *text + "' is not a port: a port is digits from 0 to 65535, or '' for none";
    }
  } else {
    llvm::StringRef host = value, port;
    if (name == "setHost" && !value.starts_with("[")) {
      std::tie(host, port) = value.split(':');
    } else if (name == "setHost") {
      size_t close = value.find(']');
      host = value.take_front(close == llvm::StringRef::npos ? value.size() : close + 1);
      port = value.drop_front(host.size());
      port.consume_front(":");
    }
    unsigned long long number = 0;
    if (!validHost(host) || !llvm::all_of(port, llvm::isDigit) ||
        (!port.empty() && (port.getAsInteger(10, number) || number > 65535))) {
      problem = "'" + *text + "' is not a host" +
                std::string(name == "setHost" ? ", with an optional port" : "");
    }
  }
  if (!problem.empty()) {
    checker_.diags_.error(input->loc(), problem);
    return false;
  }

  return true;
}

}
