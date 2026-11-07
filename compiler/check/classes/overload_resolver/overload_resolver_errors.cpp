#include "compiler/check/classes/overload_resolver/overload_resolver_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextVariant::ctorOf(llvm::StringRef cls) {
  return (llvm::Twine("the constructor of '") + cls + "'").str();
}

std::string ErrorTextVariant::quoted(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextVariant::spread(llvm::StringRef what) {
  return (llvm::Twine("a spread argument cannot choose among the variants of ") + what +
          "; pass the arguments one by one")
      .str();
}

std::string ErrorTextVariant::count(llvm::StringRef what, size_t count, llvm::StringRef list) {
  return (llvm::Twine("no variant of ") + what + " takes " + std::to_string(count) + " argument" +
          (count == 1 ? "" : "s") + ": there are " + list)
      .str();
}

std::string ErrorTextVariant::typeParams(llvm::StringRef variant) {
  return (llvm::Twine("; ") + variant + " cannot take these arguments for its type parameters")
      .str();
}

std::string ErrorTextVariant::mismatch(llvm::StringRef variant, llvm::StringRef param, size_t index,
                                       llvm::StringRef type) {
  return (llvm::Twine(variant) + " takes '" + param + "' as argument " + std::to_string(index) +
          ", which is '" + type + "'")
      .str();
}

std::string ErrorTextVariant::noFit(llvm::StringRef what, llvm::StringRef why,
                                    llvm::StringRef hint) {
  return (llvm::Twine("no variant of ") + what + " fits these arguments" + why + hint).str();
}

std::string ErrorTextVariant::generic(llvm::StringRef variant) {
  return (llvm::Twine("the call chooses ") + variant + ", a generic variant, which " +
          "cannot be chosen here")
      .str();
}

std::string ErrorTextVariant::tie(llvm::StringRef what, llvm::StringRef list, bool literals) {
  return (llvm::Twine("the call fits several variants of ") + what + " equally well: " + list +
          (literals ? "; a number literal fits each of them: write it so "
                      "that it has the type of one, e.g. '2.0' for a "
                      "double, or give it a type through a constant, "
                      "'const k: int32 = 2'"
                    : "; convert an argument to choose one"))
      .str();
}

std::string ErrorTextVariantType::several(llvm::StringRef name, llvm::StringRef list,
                                          llvm::StringRef type, llvm::StringRef object) {
  return (llvm::Twine("'") + name + "' has several variants, " + list +
          ": say which one with the type of the place it goes, e.g. "
          "'const f: " +
          type + " = " + object + "." + name + "'")
      .str();
}

}
