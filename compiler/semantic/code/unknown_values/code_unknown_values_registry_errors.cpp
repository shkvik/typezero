#include "compiler/semantic/code/unknown_values/code_unknown_values_registry_errors.h"
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

}
