#include "compiler/pipeline/check_program/topics/stmt/loop_checker/loop_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextForOf::mayBeUndef(llvm::StringRef type) {
  return (llvm::Twine("'") + type + "' may be undefined; check it before iterating").str();
}

std::string ErrorTextForOf::tuple() {
  return "cannot iterate over a tuple, whose elements have types of their own; "
         "destructure it, or copy it into an array: '[...t]'";
}

std::string ErrorTextForOf::record() {
  return "a Record is not iterable, as an object is not in JavaScript; walk its keys "
         "with 'for...in', or its entries with 'for (const [k, v] of Object.entries(r))'";
}

std::string ErrorTextForOf::later(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' gives its values later: iterate it with 'for await "
          "(const x of ...)'")
      .str();
}

std::string ErrorTextForOf::concurrent() {
  return "a ConcurrentMap is not iterated directly, as other tasks change it "
         "during the loop; iterate a snapshot: 'for (const [key, value] of "
         "map.entries())'";
}

std::string ErrorTextForOf::cannot(llvm::StringRef type) {
  return (llvm::Twine("cannot iterate over a value of type '") + type + "'").str();
}

std::string ErrorTextLoopBinding::forIn() {
  return "a 'for...in' variable cannot have a type annotation";
}

std::string ErrorTextLoopBinding::forOf() {
  return "a 'for...of' variable cannot have a type annotation";
}

std::string ErrorTextLoopShape::notArray(llvm::StringRef element) {
  return (llvm::Twine("only arrays can be destructured; the elements are '") + element + "'").str();
}

}
