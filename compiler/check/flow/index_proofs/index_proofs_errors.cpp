#include "compiler/check/flow/index_proofs/index_proofs_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextBetween::range(int64_t lo, std::optional<int64_t> hi) {
  if (hi) {
    return "from " + std::to_string(lo) + " to " + std::to_string(*hi);
  }
  if (lo == 0) {
    return "not negative";
  }

  return "at least " + std::to_string(lo);
}

std::string ErrorTextBetween::negative(llvm::StringRef what, int64_t found) {
  return (llvm::Twine(what) + " cannot be negative" + ", found " + std::to_string(found)).str();
}

std::string ErrorTextBetween::outside(llvm::StringRef what, llvm::StringRef range, int64_t found) {
  return (llvm::Twine(what) + " must be " + range + ", found " + std::to_string(found)).str();
}

std::string ErrorTextBetween::notWhole(llvm::StringRef what, llvm::StringRef range,
                                       llvm::StringRef type) {
  return (llvm::Twine(what) + " must be a whole number " + range + ", found '" + type + "'").str();
}

std::string ErrorTextBetween::test(llvm::StringRef value, int64_t lo, std::optional<int64_t> hi) {
  std::string test = (llvm::Twine(value) + " >= " + std::to_string(lo)).str();
  if (hi) {
    test += (llvm::Twine(" && ") + value + " <= " + std::to_string(*hi)).str();
  }

  return test;
}

std::string ErrorTextBetween::unproven(llvm::StringRef what, llvm::StringRef range,
                                       llvm::StringRef test) {
  return (llvm::Twine("cannot prove that ") + what + " is " + range +
          ", and JavaScript would throw a RangeError; check it first, e.g. 'if (" + test + ")'")
      .str();
}

std::string ErrorTextLess::unproven(llvm::StringRef what, llvm::StringRef left,
                                    llvm::StringRef right) {
  return (llvm::Twine("cannot prove that ") + what +
          ", and JavaScript would throw a RangeError; check it first, e.g. 'if (" + left + " < " +
          right + ")'")
      .str();
}

std::string ErrorTextUpTo::limit(llvm::StringRef object, bool bytes, int64_t less) {
  std::string limit = (llvm::Twine(object) + (bytes ? ".byteLength" : ".length")).str();
  if (less) {
    limit += " - " + std::to_string(less);
  }

  return limit;
}

std::string ErrorTextUpTo::unproven(llvm::StringRef what, std::optional<std::string> value,
                                    llvm::StringRef limit) {
  std::string place = value ? "'" + *value + "'" : "0";
  std::string from = value ? *value + " >= 0 && " + *value : "0";

  return (llvm::Twine("cannot prove that ") + what + " " + place + " is from 0 to " + limit +
          ", and JavaScript would throw a RangeError; check it first, e.g. 'if (" + from +
          " <= " + limit + ")'")
      .str();
}

}
