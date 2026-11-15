#include "compiler/semantic/library_v2/tz_modules/library_tz_modules_registry_errors.h"

namespace tz {

std::string ErrorTextIntrinsic::plain() {
  return "an intrinsic takes plain parameters: no type parameters, no rest";
}

std::string ErrorTextIntrinsic::optional() {
  return "an intrinsic has no optional parameters";
}

std::string ErrorTextIntrinsic::takes(llvm::StringRef type) {
  return "an intrinsic cannot take a value of type '" + type.str() + "'";
}

std::string ErrorTextIntrinsic::gives(llvm::StringRef type) {
  return "an intrinsic cannot give a value of type '" + type.str() + "'";
}

std::string ErrorTextHeader::name(llvm::StringRef name) {
  return "'" + name.str() +
         "' is not a name of a header: a name is letters, "
         "digits and !#$%&'*+-.^_`|~, without spaces";
}

std::string ErrorTextHeader::value() {
  return "the value of a header cannot hold a line break or NUL";
}

std::string ErrorTextHttpResponse::noBody(int64_t status) {
  std::string code = std::to_string(status);

  return "a response with status " + code +
         " has no body; write 'new Response(undefined, "
         "{ status: " +
         code + " })'";
}

std::string ErrorTextHttpResponse::statusText() {
  return "a statusText holds only tabs, spaces and visible characters up "
         "to U+00FF: no line breaks, no other control characters";
}

std::string ErrorTextHttpRequest::badUrl(llvm::StringRef text) {
  return "'" + text.str() + "' is not a valid URL";
}

std::string ErrorTextHttpRequest::userInUrl() {
  return "a request cannot go to a URL with a user name or "
         "a password in it, as in node: send them in a "
         "header, e.g. 'authorization'";
}

std::string ErrorTextHttpRequest::unknown() {
  return "'new Request' of a text known only when the program runs may fail, "
         "and 'new' gives no Result; parse the text with 'URL.parse(text)' "
         "and pass the URL, or give the text to fetch, which gives a Result";
}

std::string ErrorTextHttpRequest::badMethod(llvm::StringRef method) {
  return "'" + method.str() + "' is not a valid HTTP method";
}

std::string ErrorTextHttpRequest::forbidden(llvm::StringRef method) {
  return "fetch does not make requests with method '" + method.str() + "'";
}

std::string ErrorTextHttpRequest::getBody(llvm::StringRef method, bool given) {
  return "a " + method.str() + " request has no body" +
         (given ? "" : " (a request without 'method' is a GET)") +
         "; give the method, e.g. 'method: \"POST\"'";
}

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
