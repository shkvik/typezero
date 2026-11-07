#include "compiler/check/objects/dict_new_checker/dict_new_checker_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextPairParts::differ(bool keys, llvm::StringRef type, llvm::StringRef other) {
  return (llvm::Twine(std::string("the ")) + (keys ? "keys" : "values") +
          " of a Map have one type; found '" + type + "' and '" + other + "'")
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

std::string ErrorTextDictSource::newName(llvm::StringRef name) {
  return (llvm::Twine("'new ") + name + "'").str();
}

}
