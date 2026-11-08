#include "compiler/semantic/library/tz_modules/url_checker/url_checker_errors.h"

namespace tz {

std::string ErrorTextUrlNew::base() {
  return "'new URL' with a base known only when the program runs may fail, and "
         "'new' gives no Result; write 'URL.parse(input, base)', which gives a "
         "Result<URL, UrlError>";
}

std::string ErrorTextUrlNew::invalid(llvm::StringRef text, bool based) {
  return "'" + text.str() + "' is not a valid URL" + (based ? "" : " without a base");
}

std::string ErrorTextUrlNew::input(bool based) {
  return "'new URL' of a text known only when the program runs may fail, and "
         "'new' gives no Result; write 'URL.parse(text" +
         std::string(based ? ", base" : "") + ")', which gives a Result<URL, UrlError>";
}

std::string ErrorTextUrlSetter::href(llvm::StringRef text) {
  return "'" + text.str() + "' is not a valid URL";
}

std::string ErrorTextUrlSetter::protocol(llvm::StringRef text) {
  return "'" + text.str() +
         "' is not a protocol: a protocol is a letter, then letters, digits "
         "and +-., e.g. 'https:'";
}

std::string ErrorTextUrlSetter::port(llvm::StringRef text) {
  return "'" + text.str() + "' is not a port: a port is digits from 0 to 65535, or '' for none";
}

std::string ErrorTextUrlSetter::host(llvm::StringRef text, bool withPort) {
  return "'" + text.str() + "' is not a host" +
         std::string(withPort ? ", with an optional port" : "");
}

}
