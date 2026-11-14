#include "compiler/semantic/code/loops/code_loops_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextJump::noLabel(llvm::StringRef label) {
  return (llvm::Twine("cannot find label '") + label + "'").str();
}

std::string ErrorTextJump::breakOut() {
  return "'break' can only be used inside a loop or a 'switch'";
}

std::string ErrorTextJump::continueOut() {
  return "'continue' can only be used inside a loop";
}

std::string ErrorTextJump::notLoop(llvm::StringRef keyword, llvm::StringRef label) {
  return (llvm::Twine("'") + keyword + " " + label + "' needs a loop, and '" + label +
          "' does not label one")
      .str();
}

std::string ErrorTextLabel::duplicate(llvm::StringRef label) {
  return (llvm::Twine("duplicate label '") + label + "'").str();
}

std::string ErrorTextLabelBody::declared() {
  return "a declaration cannot have a label";
}

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

std::string ErrorTextForOf::loop() {
  return "'for...of'";
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

std::string ErrorTextForIn::indices(llvm::StringRef what, llvm::StringRef text, bool array) {
  std::string forEach =
      array ? (llvm::Twine(" or '") + text + ".forEach((x, i) => ...)'").str() : std::string();

  return (llvm::Twine("'for...in' over ") + what +
          " gives its indices as strings ('0', '1'...), so 'i + 1' would be '01'; walk "
          "the elements with 'for (const x of " +
          text + ")', or the indices with 'for (let i = 0; i < " + text + ".length; i++)'" +
          forEach)
      .str();
}

std::string ErrorTextForIn::undefined() {
  return "the object may be undefined; check it before walking its keys";
}

std::string ErrorTextForIn::dict(llvm::StringRef text, bool isMap) {
  return (llvm::Twine("'for...in' walks the properties of an object, and a ") +
          (isMap ? "Map" : "Set") + " has none in JavaScript; walk its " +
          (isMap ? "keys" : "values") + " with 'for (const x of " + text +
          (isMap ? ".keys()" : "") + ")'")
      .str();
}

std::string ErrorTextForIn::notObject(llvm::StringRef type) {
  return (llvm::Twine("'for...in' walks the keys of an object, found '") + type + "'").str();
}

std::string ErrorTextForIn::anArray() {
  return "an array";
}

std::string ErrorTextForIn::aString() {
  return "a string";
}

std::string ErrorTextForIn::aTuple() {
  return "a tuple";
}

}
