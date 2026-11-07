#include "compiler/check/expr/argument_checker/argument_checker_errors.h"
#include "compiler/check/checker/checker.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextArity::count(unsigned min, unsigned max, size_t got) {
  std::string expected = argumentCount(min, max);

  return (llvm::Twine("expected ") + expected + ", but got " + llvm::Twine(got)).str();
}

std::string ErrorTextArity::tagged(unsigned min, unsigned max, size_t got) {
  std::string expected = argumentCount(min, max);
  std::string values;
  if (got == 2) {
    values = " and 1 value";
  } else if (got != 1) {
    values = " and " + std::to_string(got - 1) + " values";
  }

  return (llvm::Twine("expected ") + expected + ", but the tagged template passes " +
          llvm::Twine(got) + ": the array of its texts" + values)
      .str();
}

std::string ErrorTextSpreadRest::otherType(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("cannot spread '") + from + "' into '" + to + "'").str();
}

std::string ErrorTextTupleRest::count(size_t expected, size_t got) {
  return (llvm::Twine("expected ") + llvm::Twine(expected) + " arguments, but got " +
          llvm::Twine(got))
      .str();
}

}
