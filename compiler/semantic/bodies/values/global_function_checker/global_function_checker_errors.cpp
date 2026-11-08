#include "compiler/semantic/bodies/values/global_function_checker/global_function_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextGlobalFunction::toBytes() {
  return "'unescape(encodeURIComponent(s))' makes a string of the UTF-8 bytes of s, "
         "a legacy trick; get the bytes themselves with 'new "
         "TextEncoder().encode(s)'";
}

std::string ErrorTextGlobalFunction::escape() {
  return "'escape' is a legacy function whose '%uXXXX' escapes nothing decodes but "
         "'unescape'; encode with 'encodeURIComponent(s)'";
}

std::string ErrorTextGlobalFunction::unescape() {
  return "'unescape' is a legacy function that decodes the '%uXXXX' escapes of "
         "'escape'; decode with 'decodeURIComponent(s)'";
}

std::string ErrorTextGlobalFunction::fromBytes() {
  return "'decodeURIComponent(escape(s))' reads the UTF-8 bytes that s holds as "
         "characters, a legacy trick; keep the bytes in a Uint8Array and decode "
         "them with 'new TextDecoder().decode(bytes)'";
}

std::string ErrorTextGlobalFunction::undefined() {
  return "Number() of a value that may be undefined would be NaN, "
         "and tz has no NaN; check it first or give a default "
         "with '??'";
}

std::string ErrorTextGlobalFunction::wrongType(llvm::StringRef type) {
  return (llvm::Twine("Number() takes a number, a boolean, a string or a decimal, "
                      "found '") +
          type + "'")
      .str();
}

}
