#include "compiler/semantic/library_v2/values/library_values_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextStringMethod::missing(llvm::StringRef name) {
  return (llvm::Twine("property '") + name + "' does not exist on type 'string'").str();
}

std::string ErrorTextStringMethod::repeatOf() {
  return "the count of 'repeat'";
}

std::string ErrorTextNumberStatic::rawTag() {
  return "String.raw can only tag a template, e.g. String.raw`C:\\dir`";
}

std::string ErrorTextNumberStatic::noProperty(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("property '") + name + "' does not exist on '" + owner + "'").str();
}

std::string ErrorTextConversion::toDecimal(llvm::StringRef value) {
  return (llvm::Twine("a double is inexact already, so it becomes a decimal through "
                      "'decimal(") +
          value + ")', which says so")
      .str();
}

std::string ErrorTextConversion::toInteger(llvm::StringRef value, llvm::StringRef method) {
  return (llvm::Twine("a double becomes an integer only after rounding, which says what "
                      "happens to the fraction: 'Math.trunc(") +
          value + ")." + method + "()' (or Math.round, Math.floor, Math.ceil)")
      .str();
}

std::string ErrorTextNumberMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextNumberMethod::codePoint() {
  return "a code point";
}

std::string ErrorTextNumberMethod::radix() {
  return "the radix of 'toString'";
}

std::string ErrorTextNumberMethod::digits() {
  return "the number of digits of 'toFixed'";
}

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
  bool isIterating = name == "map" || name == "filter" || name == "reduce" || name == "some" ||
                     name == "every" || name == "find";
  if (name == "length") {
    message += (llvm::Twine("; the number of entries is '") + text + ".size'").str();
  } else if (isMap && name == "add") {
    message += (llvm::Twine("; a Map sets an entry with '") + text + ".set(key, value)'").str();
  } else if (!isMap && (name == "set" || name == "get")) {
    message += (llvm::Twine("; a Set adds a value with '") + text +
                ".add(value)' and checks one with '" + text + ".has(value)'")
                   .str();
  } else if (isIterating) {
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

std::string ErrorTextStaticFrom::iterator(llvm::StringRef name) {
  return (llvm::Twine("'Iterator.") + name +
          "' is not supported; of Iterator only 'Iterator.from' is")
      .str();
}

std::string ErrorTextStaticFrom::array(llvm::StringRef name) {
  return (llvm::Twine("'Array.") + name +
          "' is not supported; of Array only 'Array.from' and 'new Array' are")
      .str();
}

std::string ErrorTextIterSource::notIter(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'") + fn + "' needs an array, a string or an iterator, found '" + type + "'")
      .str();
}

std::string ErrorTextCallbackValue::missing(llvm::StringRef fn) {
  return (llvm::Twine("the callback of '") + fn + "' must return a value").str();
}

std::string ErrorTextNextValue::noUnion(llvm::StringRef iterator, llvm::StringRef element,
                                        llvm::StringRef returned, llvm::StringRef problem) {
  return (llvm::Twine("next() of '") + iterator + "' gives '" + element + "', or '" + returned +
          "' when it is done, and no union holds both: " + problem +
          "; iterate it with 'for...of', which ignores what it returns")
      .str();
}

std::string ErrorTextIteratorMethod::noThrow() {
  return "an iterator has no 'throw()' in tz: an error is a value, "
         "not something thrown into a generator";
}

std::string ErrorTextIteratorMethod::noReturn() {
  return "'return()' of an iterator is not supported yet; stop "
         "with 'break' in 'for of'";
}

std::string ErrorTextIteratorMethod::noProperty(llvm::StringRef name, llvm::StringRef type) {
  std::string message =
      (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
  if (name == "length") {
    message += "; an iterator does not know how many values it gives: collect them with '[...it]'";
  }

  return message;
}

std::string ErrorTextIteratorMethod::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs a function").str();
}

std::string ErrorTextReduceInit::untyped() {
  return "the initial value of 'reduce' needs a type; declare it in a variable "
         "first, e.g. 'const start: int | undefined = undefined'";
}

std::string ErrorTextFlatMap::notIter(llvm::StringRef type) {
  return (llvm::Twine("the callback of 'flatMap' must return an array or an iterator, "
                      "found '") +
          type + "'")
      .str();
}

std::string ErrorTextIteratorMethod::countOf(llvm::StringRef name) {
  return (llvm::Twine("the count of '") + name + "'").str();
}

std::string ErrorTextGlobalFunction::toBytes() {
  return "'unescape(encodeURIComponent(s))' makes a string of the UTF-8 bytes of s, "
         "a legacy trick; get the bytes themselves with 'new "
         "TextEncoder().encode(s)'";
}

std::string ErrorTextGlobalFunction::escape() {
  return "'escape' is a legacy function whose '%uXXXX' escapes nothing decodes but "
         "'unescape'; encode with 'encodeURIComponent(s)'";
}

std::string ErrorTextGlobalFunction::unescape() {
  return "'unescape' is a legacy function that decodes the '%uXXXX' escapes of "
         "'escape'; decode with 'decodeURIComponent(s)'";
}

std::string ErrorTextGlobalFunction::fromBytes() {
  return "'decodeURIComponent(escape(s))' reads the UTF-8 bytes that s holds as "
         "characters, a legacy trick; keep the bytes in a Uint8Array and decode "
         "them with 'new TextDecoder().decode(bytes)'";
}

std::string ErrorTextGlobalFunction::undefined() {
  return "Number() of a value that may be undefined would be NaN, "
         "and tz has no NaN; check it first or give a default "
         "with '??'";
}

std::string ErrorTextGlobalFunction::wrongType(llvm::StringRef type) {
  return (llvm::Twine("Number() takes a number, a boolean, a string or a decimal, "
                      "found '") +
          type + "'")
      .str();
}

std::string ErrorTextObjectCall::noSupport(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' is not supported; only Object.keys, Object.values, "
          "Object.entries, Object.fromEntries and Object.groupBy are")
      .str();
}

std::string ErrorTextObjectCall::undefined() {
  return "the object may be undefined; check it first";
}

std::string ErrorTextObjectCall::needObject(llvm::StringRef fn, llvm::StringRef type) {
  return (llvm::Twine("'Object.") + fn + "' needs an object, found '" + type + "'").str();
}

std::string ErrorTextObjectCall::emitter(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' of an emitter is not supported: its fields are the "
          "listeners it keeps; read the fields of its class by name")
      .str();
}

std::string ErrorTextObjectCall::methods(llvm::StringRef fn) {
  return (llvm::Twine("'Object.") + fn +
          "' of an object with methods is not supported; "
          "read the fields by name")
      .str();
}

std::string ErrorTextObjectCall::noUnion(llvm::StringRef fn, llvm::StringRef type,
                                         llvm::StringRef problem) {
  return (llvm::Twine("'Object.") + fn +
          "' puts the values of all the fields in one array, of the "
          "union of their types, and the fields of '" +
          type + "' have types that no union holds: " + problem + "; read the fields by name")
      .str();
}

std::string ErrorTextObjectCall::noFields(llvm::StringRef type, llvm::StringRef fn) {
  return (llvm::Twine("'") + type + "' has no fields, so 'Object." + fn + "' would always be empty")
      .str();
}

std::string ErrorTextObjectCall::numberEnum(llvm::StringRef fn, llvm::StringRef name,
                                            llvm::StringRef member) {
  return (llvm::Twine("'Object.") + fn + "' of numeric enum '" + name +
          "' is not supported: in JavaScript it mixes the names of the "
          "members with their numbers; list the members you need, e.g. '[" +
          name + "." + member + "]'")
      .str();
}