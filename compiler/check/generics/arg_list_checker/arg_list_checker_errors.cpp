#include "compiler/check/generics/arg_list_checker/arg_list_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextRestList::spreadOnly(llvm::StringRef list, llvm::StringRef type) {
  return (llvm::Twine("'") + list + "' of the parameters '" + type +
          "', which end with a rest parameter, can only be spread, '..." + list + "'")
      .str();
}

std::string ErrorTextListArgument::tooMany(llvm::StringRef list) {
  return (llvm::Twine("too many arguments for the parameters '") + list + "'").str();
}

std::string ErrorTextListArgument::spreadWrong(llvm::StringRef spread, llvm::StringRef list,
                                               llvm::StringRef part, bool partIsList) {
  return (llvm::Twine("'...") + spread + "' is spread where the parameters '" + list + "' take " +
          (partIsList ? "'..." : "'") + part + "'")
      .str();
}

std::string ErrorTextListArgument::notSpread(llvm::StringRef part) {
  return (llvm::Twine("the arguments for the parameters '") + part +
          "' of some function are spread from a list of them, e.g. "
          "'...args'")
      .str();
}

std::string ErrorTextListFilled::missing(llvm::StringRef list, llvm::StringRef part,
                                         bool partIsList) {
  return (llvm::Twine("the parameters '") + list + "' take " + (partIsList ? "'..." : "'") + part +
          "' too, after these arguments")
      .str();
}

}
