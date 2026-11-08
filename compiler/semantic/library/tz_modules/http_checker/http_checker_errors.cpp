#include "compiler/semantic/library/tz_modules/http_checker/http_checker_errors.h"

namespace tz {

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

}
