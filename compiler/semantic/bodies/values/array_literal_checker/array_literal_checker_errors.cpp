#include "compiler/semantic/bodies/values/array_literal_checker/array_literal_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextArrayHole::hole() {
  return "arrays cannot have holes; write the element explicitly";
}

std::string ErrorTextTupleSpread::differ(llvm::StringRef type) {
  return (llvm::Twine("the elements of '") + type +
          "' have different types, and an array holds elements of one type")
      .str();
}

std::string ErrorTextSpreadable::notSpread(llvm::StringRef type, bool record) {
  return (llvm::Twine("only arrays, strings, iterators, Maps and Sets can be spread "
                      "here, found '") +
          type + "'" +
          (record ? "; for the entries of a Record, spread "
                    "'Object.entries(r)'"
                  : ""))
      .str();
}

std::string ErrorTextArrayEmpty::empty() {
  return "cannot infer the element type of an empty array; add a type, "
         "e.g. 'let a: int[] = []'";
}

std::string ErrorTextArrayTypes::differ(llvm::StringRef element, llvm::StringRef other) {
  return (llvm::Twine("array elements must have one type; found '") + element + "' and '" + other +
          "'")
      .str();
}

std::string ErrorTextArrayTypes::literals() {
  return "; each object literal with methods has a type of its own, so objects made "
         "by different literals differ; an interface with their fields and methods "
         "joins them: 'const xs: Counter[] = [...]'";
}

std::string ErrorTextArrayTypes::tuple(llvm::StringRef types) {
  return (llvm::Twine("; for a value of each type in its own place, declare a tuple, e.g. "
                      "'const t: [") +
          types + "] = ...'")
      .str();
}

std::string ErrorTextArrayElement::alone(llvm::StringRef element) {
  return (llvm::Twine("cannot infer the element type from '") + element +
          "' alone; add a type, e.g. 'let a: (int | undefined)[] = ...'")
      .str();
}

std::string ErrorTextSpreadFits::into(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("cannot spread '") + from + "' into '" + to + "'").str();
}

std::string ErrorTextSpreadable::spread() {
  return "a spread";
}

}
