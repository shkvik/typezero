#include "compiler/pipeline/check_program/topics/expr/member_checker/member_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextMemberRead::decimal(llvm::StringRef name) {
  return (llvm::Twine("'decimal.") + name +
          "' can only be called, e.g. 'decimal.max(a, b)'; decimal "
          "has the functions max, min and sum")
      .str();
}

std::string ErrorTextMemberRead::crypto() {
  return "'globalThis.crypto' is used as "
         "'globalThis.crypto.subtle' and the like";
}

std::string ErrorTextMemberRead::globalThis(llvm::StringRef name) {
  return (llvm::Twine("'globalThis.") + name +
          "' is not supported: tz has only 'globalThis.crypto'; "
          "write '" +
          name + "'")
      .str();
}

std::string ErrorTextMemberRead::intl(llvm::StringRef name) {
  return (llvm::Twine("'Intl.") + name +
          "' can only be called or made with 'new', e.g. 'new "
          "Intl.DateTimeFormat(\"ru\")'")
      .str();
}

std::string ErrorTextMemberRead::numberProp(llvm::StringRef name) {
  return (llvm::Twine("property '") + name + "' does not exist on 'Number'").str();
}

std::string ErrorTextMemberRead::asValue(llvm::StringRef global, llvm::StringRef name) {
  return (llvm::Twine("'") + global + "." + name +
          "' can be used as a value only where a function type is "
          "expected, e.g. as a callback")
      .str();
}

std::string ErrorTextMemberRead::superField(llvm::StringRef name) {
  return (llvm::Twine("'super.") + name + "' can only be called, 'super." + name +
          "(...)'; to read the field or the method, use "
          "'this." +
          name + "'")
      .str();
}

std::string ErrorTextMemberRead::weakRef(llvm::StringRef name) {
  return (llvm::Twine("property '") + name + "' of a WeakRef can only be called: 'deref()'").str();
}

std::string ErrorTextMemberRead::undefined(llvm::StringRef name) {
  return (llvm::Twine("the value may be undefined; check it before reading '") + name + "'").str();
}

std::string ErrorTextMemberRead::raw() {
  return "'raw' is not supported: a tag gets the texts of its template as a "
         "plain 'string[]', with the escapes already applied";
}

std::string ErrorTextMemberRead::mustCall(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type +
          "'; methods must be called")
      .str();
}

std::string ErrorTextMemberRead::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextIndexRead::undefined() {
  return "the value may be undefined; check it before indexing";
}

std::string ErrorTextIndexRead::notIndexed(llvm::StringRef type) {
  return (llvm::Twine("type '") + type + "' cannot be indexed").str();
}

std::string ErrorTextPosition::notInt(llvm::StringRef type, bool fraction, bool undefined) {
  std::string message =
      (llvm::Twine("an index or position must be an integer, found '") + type + "'").str();
  if (fraction) {
    message += "; round it with Math.floor(x) or drop the fraction with 'x | 0'";
  } else if (undefined) {
    message += "; the value may be undefined: check it first or give a default with '??'";
  }

  return message;
}

}
