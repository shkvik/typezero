#include "compiler/check/objects/dict_methods/dict_methods_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextRecordMethod::hasOwn(llvm::StringRef key, llvm::StringRef text) {
  return (llvm::Twine("a Record inherits nothing from Object.prototype, and '") + key +
          "' is a key like any other; ask for a key with 'key in " + text + "'")
      .str();
}

std::string ErrorTextRecordMethod::undefined(llvm::StringRef key) {
  return (llvm::Twine("'") + key +
          "' may be undefined, as a key may be missing; call it with "
          "'" +
          key + "?.(...)'")
      .str();
}

std::string ErrorTextRecordMethod::noMethods(llvm::StringRef key, llvm::StringRef value,
                                             llvm::StringRef text) {
  return (llvm::Twine("a Record has no methods, only keys, and '") + key + "' is a '" + value +
          " | undefined'; for the keys use 'Object.keys(" + text + ")'")
      .str();
}

std::string ErrorTextDictForEach::thisArg() {
  return "the second argument of 'forEach' sets 'this' for the callback, which is not "
         "supported";
}

std::string ErrorTextSetCombine::notSet(llvm::StringRef name, llvm::StringRef key,
                                        llvm::StringRef other) {
  return (llvm::Twine("'") + name + "' needs a 'Set<" + key + ">' or a 'Map<" + key +
          ", ...>', found '" + other + "'; make a Set first, e.g. 'new Set(xs)'")
      .str();
}

std::string ErrorTextDictMethod::noProperty(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef text, bool isMap) {
  std::string message =
      (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
  bool iterates = name == "map" || name == "filter" || name == "reduce" || name == "some" ||
                  name == "every" || name == "find";
  if (name == "length") {
    message += (llvm::Twine("; the number of entries is '") + text + ".size'").str();
  } else if (isMap && name == "add") {
    message += (llvm::Twine("; a Map sets an entry with '") + text + ".set(key, value)'").str();
  } else if (!isMap && (name == "set" || name == "get")) {
    message += (llvm::Twine("; a Set adds a value with '") + text +
                ".add(value)' and checks one with '" + text + ".has(value)'")
                   .str();
  } else if (iterates) {
    message += (llvm::Twine("; call it on an iterator, e.g. '") + text +
                (isMap ? ".entries()" : ".values()") + "." + name +
                "(...)', or on an array, '[..." + text + "]." + name + "(...)'")
                   .str();
  }

  return message;
}

std::string ErrorTextFromEntries::keys(llvm::StringRef type) {
  return (llvm::Twine("the keys of an object are strings, and JavaScript would turn '") + type +
          "' keys into them; convert them first, or make a Map: 'new Map(...)'")
      .str();
}

std::string ErrorTextFromEntries::undefined() {
  return "cannot infer the type of the values from 'undefined'; "
         "declare it, e.g. 'const r: Record<string, int | "
         "undefined> = Object.fromEntries(...)'";
}

std::string ErrorTextGroupBy::noKey(llvm::StringRef fn) {
  return (llvm::Twine("the callback of '") + fn + "' must return the key of a group").str();
}

std::string ErrorTextGroupBy::keys(llvm::StringRef type) {
  return (llvm::Twine("the keys of an object are strings, and JavaScript would turn '") + type +
          "' keys into them; return a string, or group into a Map: 'Map.groupBy(...)'")
      .str();
}

std::string ErrorTextMapStatic::unknown(llvm::StringRef name) {
  return (llvm::Twine("'Map.") + name +
          "' is not supported; of Map only "
          "'Map.groupBy' and 'new Map' are")
      .str();
}

std::string ErrorTextDictObject::empty(llvm::StringRef fn, llvm::StringRef kind,
                                       llvm::StringRef text, llvm::StringRef method) {
  return (llvm::Twine("'Object.") + fn + "' of a " + kind +
          " is always empty in JavaScript: its entries are no properties; "
          "use '[..." +
          text + "." + method + "()]'")
      .str();
}

}
