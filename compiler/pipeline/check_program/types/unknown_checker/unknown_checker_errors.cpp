#include "compiler/pipeline/check_program/types/unknown_checker/unknown_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextUnknownUse::property(llvm::StringRef name) {
  return (llvm::Twine("property '") + name + "'").str();
}

std::string ErrorTextUnknownUse::method(llvm::StringRef name) {
  return (llvm::Twine("method '") + name + "'").str();
}

std::string ErrorTextUnknownUse::object(llvm::StringRef value, llvm::StringRef what) {
  std::string use = what.empty() ? std::string("cannot be indexed") : "has no " + what.str();

  return (llvm::Twine("an object ") + use + " until 'instanceof' shows its class, e.g. 'if (" +
          value + " instanceof C) ...'")
      .str();
}

std::string ErrorTextUnknownUse::unknown(llvm::StringRef value, llvm::StringRef what) {
  std::string use = what.empty() ? std::string("cannot be indexed") : "has no " + what.str();
  std::string example =
      what.empty() ? "Array.isArray(" + value.str() + ")" : value.str() + " instanceof C";

  return (llvm::Twine("an unknown value ") + use +
          " until a check shows what it holds, e.g. 'if (" + example + ") ...' or 'typeof " +
          value + " == \"string\"'")
      .str();
}

std::string ErrorTextUnknownMember::noCtor() {
  return "'constructor' is not supported: an object has no prototype in tz; "
         "a decorator gets the class itself, use it as it is";
}

std::string ErrorTextUnknownMethod::runtime(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is some class known only when the "
          "program runs, whose static members are not known; call its "
          "methods through reflection (docs/decorators.md, section 6)")
      .str();
}

std::string ErrorTextUnknownMethod::classValue(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' as a value has no methods "
          "in tz")
      .str();
}

std::string ErrorTextUnknownHint::instanceOf(llvm::StringRef value, llvm::StringRef cls) {
  return (llvm::Twine(value) + " instanceof " + cls).str();
}

std::string ErrorTextUnknownHint::typeOf(llvm::StringRef value, llvm::StringRef type) {
  return (llvm::Twine("typeof ") + value + " == \"" + type + "\"").str();
}

std::string ErrorTextUnknownHint::isArray(llvm::StringRef value) {
  return (llvm::Twine("Array.isArray(") + value + ")").str();
}

std::string ErrorTextUnknownHint::anyClass(llvm::StringRef check) {
  return (llvm::Twine("; an object may be of any class: check which first, e.g. 'if (") + check +
          ") ...'")
      .str();
}

std::string ErrorTextUnknownHint::anything(llvm::StringRef check) {
  return (llvm::Twine(
              "; an unknown value may hold anything: check what it holds first, e.g. 'if (") +
          check + ") ...'")
      .str();
}

}
