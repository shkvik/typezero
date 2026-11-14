#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextDictType::weak(llvm::StringRef name, llvm::StringRef plain) {
  return (llvm::Twine("'") + name + "' is not supported yet; a '" + plain +
          "' holds its keys, and frees them with itself")
      .str();
}

std::string ErrorTextDictType::setArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs the type of its elements, e.g. '" + name + "<int>'")
      .str();
}

std::string ErrorTextDictType::mapArgs(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs the types of its keys and values, e.g. '" + name +
          "<string, int>'")
      .str();
}

std::string ErrorTextDictType::recordArgs() {
  return "'Record' needs the types of its keys and values, "
         "e.g. 'Record<string, int>'";
}

std::string ErrorTextDictType::numberKeys(llvm::StringRef key, llvm::StringRef value) {
  return (llvm::Twine("the keys of a Record are strings: JavaScript would turn a number key into a "
                      "string, so 'r[1]' and 'r[\"1\"]' would be one; for numbers use a Map, e.g. "
                      "'Map<") +
          key + ", " + value + ">'")
      .str();
}

std::string ErrorTextDictType::otherKeys(llvm::StringRef key, llvm::StringRef value) {
  return (llvm::Twine("the keys of a Record are strings, found '") + key +
          "'; for other keys use a Map, e.g. 'Map<" + key + ", " + value + ">'")
      .str();
}

std::string ErrorTextKeyType::unknown() {
  return "an unknown cannot be a key: a key is found by its value, and what an "
         "unknown holds is known only when the program runs; use keys of one type, "
         "e.g. 'Map<string, unknown>'";
}

std::string ErrorTextKeyType::result() {
  return "a Result cannot be a key: Results are not compared, as in JavaScript each "
         "is an object of its own, equal to no other; use what it holds";
}

std::string ErrorTextKeyType::keyLabel() {
  return "the key";
}

std::string ErrorTextRecordKey::number(llvm::StringRef type) {
  return (llvm::Twine("the keys of a Record are strings, and JavaScript would turn this '") + type +
          "' into one, so 1 and \"1\" would be one key; convert it, "
          "'String(i)', or use a Map, whose keys may be numbers")
      .str();
}

std::string ErrorTextRecordKey::numberIn(llvm::StringRef type) {
  return (llvm::Twine("the keys of a Record are strings, and JavaScript would turn this '") + type +
          "' into one; convert it, 'String(i)'")
      .str();
}

std::string ErrorTextDictMember::callOnly(llvm::StringRef text, llvm::StringRef name) {
  return (llvm::Twine("'") + text + "." + name + "' can only be called, e.g. '" + text + "." +
          name +
          "(...)'; pass an arrow that calls it where a function is "
          "expected")
      .str();
}

std::string ErrorTextDictMember::noProperty(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef text) {
  std::string message =
      (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
  if (name == "length") {
    message += (llvm::Twine("; the number of entries is '") + text + ".size'").str();
  }

  return message;
}

std::string ErrorTextDictIndex::mapIndex(llvm::StringRef text) {
  return (llvm::Twine("'") + text +
          "[key]' reads a property of the Map object in "
          "JavaScript, not one of its entries; read an entry with '" +
          text + ".get(key)'")
      .str();
}

std::string ErrorTextDictIndex::setIndex(llvm::StringRef text) {
  return (llvm::Twine("a Set has no elements by position; check one with '") + text +
          ".has(value)', or copy them into an array, '[..." + text + "]'")
      .str();
}

std::string ErrorTextDictTarget::mapSet(llvm::StringRef text, llvm::StringRef access) {
  return (llvm::Twine("'") + text + access +
          " = ...' sets a property of the Map object in JavaScript, "
          "not one of its entries; set an entry with '" +
          text + ".set(key, value)'")
      .str();
}

std::string ErrorTextDictTarget::setSize(llvm::StringRef text) {
  return (llvm::Twine("the size of a Set cannot be assigned to; add elements with '") + text +
          ".add(value)'")
      .str();
}

std::string ErrorTextDictTarget::setIndex(llvm::StringRef text) {
  return (llvm::Twine("a Set has no elements by position; add one with '") + text + ".add(value)'")
      .str();
}

std::string ErrorTextDictTarget::missing(llvm::StringRef key, llvm::StringRef hint) {
  return (llvm::Twine("'") + key +
          "' may be undefined, as a key may be missing: give it a "
          "default with '??'" +
          hint)
      .str();
}

std::string ErrorTextDictTarget::numberHint(llvm::StringRef key) {
  return (llvm::Twine(", e.g. '") + key + " = (" + key + " ?? 0) + 1'").str();
}

std::string ErrorTextDictTarget::textHint(llvm::StringRef key) {
  return (llvm::Twine(", e.g. '") + key + " = (" + key + " ?? \"\") + s'").str();
}

std::string ErrorTextDictDelete::entryPart(bool isMap) {
  return isMap ? "key" : "value";
}

std::string ErrorTextDictDelete::property(llvm::StringRef kind, llvm::StringRef text,
                                          llvm::StringRef what) {
  return (llvm::Twine("'delete' removes a property of the ") + kind +
          " object in JavaScript, not an entry; remove one with '" + text + ".delete(" + what +
          ")'")
      .str();
}

std::string ErrorTextDictIn::undefined() {
  return "the dictionary may be undefined; check it first";
}

std::string ErrorTextDictIn::property(llvm::StringRef kind, llvm::StringRef text,
                                      llvm::StringRef what) {
  return (llvm::Twine("'in' asks for a property of the ") + kind +
          " object in JavaScript, not for an entry; ask with '" + text + ".has(" + what + ")'")
      .str();
}

std::string ErrorTextPairParts::keysDiffer(llvm::StringRef type, llvm::StringRef other) {
  return ErrorTextPairParts::differ("keys", type, other);
}

std::string ErrorTextPairParts::valuesDiffer(llvm::StringRef type, llvm::StringRef other) {
  return ErrorTextPairParts::differ("values", type, other);
}

std::string ErrorTextPairParts::differ(llvm::StringRef parts, llvm::StringRef type,
                                       llvm::StringRef other) {
  return (llvm::Twine(std::string("the ")) + parts + " of a Map have one type; found '" + type +
          "' and '" + other + "'")
      .str();
}

std::string ErrorTextPairsKnown::undefined() {
  return "cannot infer the types of the Map from 'undefined'; give them, e.g. "
         "'new Map<string, int | undefined>(...)'";
}

std::string ErrorTextDictSource::notIter(llvm::StringRef what, llvm::StringRef type, bool record) {
  return (llvm::Twine(what) + " needs an array, a string, an iterator, a Map or a Set, found '" +
          type + "'" +
          (record ? "; for the entries of a Record, pass "
                    "'Object.entries(r)'"
                  : ""))
      .str();
}

std::string ErrorTextDictSource::notPairs(llvm::StringRef what, llvm::StringRef wanted,
                                          llvm::StringRef given, bool array) {
  std::string of = wanted.empty() ? std::string() : (llvm::Twine(" of '") + wanted + "'").str();

  return (llvm::Twine(what) + " takes [key, value] pairs" + of + ", found '" + given + "'" +
          (array ? "; an array literal is a pair only where one "
                   "is expected: give the callback its type, "
                   "e.g. '(x): [string, int] => [x.name, x.n]'"
                 : ""))
      .str();
}

std::string ErrorTextDictSource::values(llvm::StringRef what, llvm::StringRef wanted,
                                        llvm::StringRef given) {
  return (llvm::Twine(what) + " takes values of type '" + wanted + "', found '" + given + "'")
      .str();
}

std::string ErrorTextDictSource::newName(llvm::StringRef name) {
  return (llvm::Twine("'new ") + name + "'").str();
}

std::string ErrorTextDictArgs::oneArg(llvm::StringRef name, bool isMap) {
  return (llvm::Twine("'new ") + name + "' takes one argument, the " +
          (isMap ? "entries" : "values") + " to start with")
      .str();
}

std::string ErrorTextDictTypes::needsMap() {
  return "'new Map()' needs the types of its keys and values, e.g. 'new "
         "Map<string, int>()'";
}

std::string ErrorTextDictTypes::needsSet() {
  return "'new Set()' needs the type of its elements, e.g. 'new "
         "Set<int>()'";
}

std::string ErrorTextDictTypes::undefinedArg(llvm::StringRef name, bool isMap) {
  return (llvm::Twine("'new ") + name + "(undefined)' needs the types of the " +
          (isMap ? "Map, e.g. 'new Map<string, int>()'" : "Set, e.g. 'new Set<int>()'"))
      .str();
}

std::string ErrorTextDictPairs::pairs(llvm::StringRef element) {
  return (llvm::Twine("'new Map' takes [key, value] pairs, found '") + element + "'").str();
}

std::string ErrorTextDictElements::undefined(llvm::StringRef name) {
  return (llvm::Twine("cannot infer the types of the ") + name + " from 'undefined'; give them")
      .str();
}

}
