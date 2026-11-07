#include "compiler/pipeline/check_program/declare/constant_folder/constant_folder_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDivisor::zero() {
  return "division by zero";
}

std::string ErrorTextShiftCount::outside(int64_t count, int last, llvm::StringRef type) {
  return (llvm::Twine("the shift count ") + llvm::Twine(count) + " is outside 0 to " +
          llvm::Twine(last) + ", the bits of '" + type + "'")
      .str();
}

std::string ErrorTextOverflow::pastExact(llvm::StringRef value) {
  return (llvm::Twine("this constant is ") + value +
          ", beyond 2^53, where JavaScript rounds it before taking its low bits; for a 32-bit "
          "product use Math.imul")
      .str();
}

std::string ErrorTextOverflow::inInt(llvm::StringRef value, llvm::StringRef type, int64_t wrapped) {
  return (llvm::Twine("this constant is ") + value + ", which does not fit in '" + type +
          "' and would wrap around to " + llvm::Twine(wrapped) +
          "; compute it as an 'int', e.g. 'const x: int = ...'")
      .str();
}

std::string ErrorTextOverflow::inDouble(llvm::StringRef value, llvm::StringRef type,
                                        int64_t wrapped) {
  return (llvm::Twine("this constant is ") + value + ", which does not fit in '" + type +
          "' and would wrap around to " + llvm::Twine(wrapped) +
          "; compute it as a 'double', e.g. 'const x: double = ...'")
      .str();
}

}
