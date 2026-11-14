#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextEarlyCall::importBack(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: it uses a file that imports it back and calls it while loading; declare "
          "its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::differ(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns values of different types; a function returns values of one type")
      .str();
}

std::string ErrorTextCycleType::selfOnly(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from a call to itself; declare its return "
          "type, e.g. '): int'")
      .str();
}

std::string ErrorTextCycleType::calledBack(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine("cannot tell what ") + quoted +
          " returns: every value it returns comes from the functions that call it back; "
          "declare its return type, e.g. '): int'")
      .str();
}

std::string ErrorTextCommonReturn::differ(llvm::StringRef function, llvm::StringRef here,
                                          llvm::StringRef elsewhere) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) + " returns '" + here + "' here and '" + elsewhere +
          "' elsewhere; a function returns values of one type")
      .str();
}

std::string ErrorTextDefinedReturn::undefined(llvm::StringRef function) {
  auto quoted = ErrorTextFunctionName::quoted(function);

  return (llvm::Twine(quoted) +
          " returns only undefined; declare the return type, e.g. '): int | undefined'")
      .str();
}

std::string ErrorTextYields::differ(llvm::StringRef fn, llvm::StringRef here,
                                    llvm::StringRef elsewhere) {
  return (llvm::Twine(fn) + " yields '" + here + "' here and '" + elsewhere +
          "' elsewhere; a generator yields values of "
          "one type")
      .str();
}

std::string ErrorTextYields::unknown(llvm::StringRef fn, llvm::StringRef why) {
  return (llvm::Twine("cannot tell what ") + fn + " yields" + why +
          "; declare its type, e.g. ': Generator<int>'")
      .str();
}

std::string ErrorTextYields::noYield() {
  return ": it has no 'yield'";
}

std::string ErrorTextYields::onlyUndef() {
  return ": it yields only undefined";
}

std::string ErrorTextGeneratorReturns::differ(llvm::StringRef fn, llvm::StringRef here,
                                              llvm::StringRef elsewhere) {
  return (llvm::Twine(fn) + " returns '" + here + "' here and '" + elsewhere +
          "' elsewhere; a function returns values of one type")
      .str();
}

std::string ErrorTextInheritedReturn::returns(llvm::StringRef name, llvm::StringRef base,
                                              llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' overrides '" + base + "." + name + "', so it returns '" +
          type +
          "' too; to return another type, "
          "declare it in '" +
          base + "'")
      .str();
}

}
