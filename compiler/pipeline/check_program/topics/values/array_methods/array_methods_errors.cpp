#include "compiler/pipeline/check_program/topics/values/array_methods/array_methods_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextArrayMethod::results(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' cannot compare Results: in JavaScript each Result is an "
          "object of its own, equal to no other; search with a "
          "function, e.g. 'some' or 'findIndex'")
      .str();
}

std::string ErrorTextArrayMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextArrayText::functions() {
  return "functions cannot be converted to strings";
}

std::string ErrorTextArrayText::results(llvm::StringRef name) {
  return (llvm::Twine("Results cannot be converted to strings: JavaScript would give "
                      "'[object Object]'; make the text of each first, e.g. 'a.map(r => "
                      "r.match(...)).") +
          name + "()'")
      .str();
}

std::string ErrorTextArrayText::unknown(llvm::StringRef name) {
  return (llvm::Twine("unknown values cannot be converted to strings: one may hold an "
                      "object; map them to text first, e.g. 'a.map(x => typeof x == "
                      "\"string\" ? x : \"?\").") +
          name + "()'")
      .str();
}

std::string ErrorTextArrayText::json(llvm::StringRef name) {
  return (llvm::Twine("Json values cannot be converted to strings: an object would be "
                      "'[object Object]'; map them to text first, e.g. 'a.map(x => "
                      "JSON.stringify(x)).") +
          name + "()'")
      .str();
}

std::string ErrorTextArrayText::objects(llvm::StringRef name) {
  return (llvm::Twine("objects cannot be converted to strings: JavaScript would give "
                      "'[object Object]'; map them to text first, e.g. 'a.map(o => o.name).") +
          name + "()'")
      .str();
}

std::string ErrorTextArraySort::functions() {
  return "sorting functions needs a comparison function: 'sort()' "
         "without one compares their source code";
}

std::string ErrorTextArraySort::unknown() {
  return "sorting unknown values needs a comparison function: 'sort()' "
         "without one compares them as strings, objects as '[object "
         "Object]'; pass one, e.g. 'sort((a, b) => ...)'";
}

std::string ErrorTextArraySort::json() {
  return "sorting Json values needs a comparison function: 'sort()' "
         "without one compares them as strings, objects as '[object "
         "Object]'; pass one, e.g. 'sort((a, b) => ...)'";
}

std::string ErrorTextArraySort::dates() {
  return "sorting dates needs a comparison function: 'sort()' without one "
         "compares the texts of toString(), 'Thu Feb 29 2024 ...', which "
         "puts them in the order of the names of the days; pass one: "
         "'sort((a, b) => a.getTime() - b.getTime())'";
}

std::string ErrorTextArraySort::objects() {
  return "sorting objects needs a comparison function: 'sort()' without one "
         "compares them all as '[object Object]'; pass one, e.g. 'sort((a, b) "
         "=> a.x - b.x)'";
}

std::string ErrorTextArraySort::mixed(llvm::StringRef type) {
  return (llvm::Twine("'sort()' without a comparison function compares the values "
                      "of '") +
          type +
          "' as strings, numbers and objects among "
          "them; pass one, e.g. 'sort((a, b) => ...)'")
      .str();
}

std::string ErrorTextArraySort::numbers(bool optional) {
  return (llvm::Twine("'sort()' without a comparison function compares "
                      "numbers as strings, as in JavaScript: [2, 10, 1] "
                      "becomes [1, 10, 2]; pass one: 'sort((x, y) => ") +
          (optional ? "(x ?? 0) - (y ?? 0)" : "x - y") + ")'")
      .str();
}

}
