#include "compiler/semantic/bodies/objects/dict_checker/dict_checker_errors.h"
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

std::string ErrorTextKeyType::keyLabel() {
  return "the key";
}

}
