#include "compiler/semantic/code/conversions/code_conversions_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextMismatch::extraField(llvm::StringRef name) {
  return (llvm::Twine(
              "; an object must have exactly the fields of its type, and this one has the extra "
              "field '") +
          name + "'")
      .str();
}

std::string ErrorTextMismatch::noField(llvm::StringRef name) {
  return (llvm::Twine("; it has no field '") + name + "'").str();
}

std::string ErrorTextMismatch::fieldType(llvm::StringRef name, llvm::StringRef here,
                                         llvm::StringRef there, llvm::StringRef to) {
  return (llvm::Twine("; field '") + name + "' is '" + here + "' here and '" + there + "' in '" +
          to + "'")
      .str();
}

std::string ErrorTextMismatch::typeHint(llvm::StringRef to) {
  return (llvm::Twine(": give the object its type where it is created, e.g. 'const p: ") + to +
          " = { ... }'")
      .str();
}

std::string ErrorTextMismatch::readonly(llvm::StringRef name, llvm::StringRef to) {
  return (llvm::Twine("; field '") + name + "' is read-only here, and '" + to +
          "' would let it be changed")
      .str();
}

std::string ErrorTextUnknownHint::instanceOf(llvm::StringRef value, llvm::StringRef cls) {
  return (llvm::Twine(value) + " instanceof " + cls).str();
}

std::string ErrorTextUnknownHint::typeOf(llvm::StringRef value, llvm::StringRef type) {
  return (llvm::Twine("typeof ") + value + " == \"" + type + "\"").str();
}

std::string ErrorTextUnknownHint::isArray(llvm::StringRef value) {
  return (llvm::Twine("Array.isArray(") + value + ")").str();
}

std::string ErrorTextUnknownHint::anyClass(llvm::StringRef check) {
  return (llvm::Twine("; an object may be of any class: check which first, e.g. 'if (") + check +
          ") ...'")
      .str();
}

std::string ErrorTextUnknownHint::anything(llvm::StringRef check) {
  return (llvm::Twine(
              "; an unknown value may hold anything: check what it holds first, e.g. 'if (") +
          check + ") ...'")
      .str();
}

std::string ErrorTextRange::holds(llvm::ArrayRef<std::pair<int64_t, int64_t>> intervals) {
  auto holds = std::string();
  for (const std::pair<int64_t, int64_t> &interval : intervals) {
    holds += holds.empty() ? "" : " or ";
    holds += std::to_string(interval.first);
    if (interval.second != interval.first) {
      holds += " to " + std::to_string(interval.second);
    }
  }

  return ", which holds " + holds;
}

std::string ErrorTextRange::outside(llvm::StringRef check) {
  return (llvm::Twine("; the value may be outside: check it first, e.g. 'if (") + check + ")'")
      .str();
}

std::string ErrorTextRange::status(llvm::StringRef value) {
  return (llvm::Twine("isHttpStatus(") + value + ")").str();
}

std::string ErrorTextRange::between(llvm::StringRef value, llvm::StringRef lo, llvm::StringRef hi) {
  return (llvm::Twine(value) + " >= " + lo + " && " + value + " <= " + hi).str();
}

std::string ErrorTextAssignable::notTo(llvm::StringRef from, llvm::StringRef to,
                                       llvm::StringRef what) {
  std::string text =
      (llvm::Twine("type '") + from + "' is not assignable to type '" + to + "'").str();
  if (!what.empty()) {
    text += (llvm::Twine(" of ") + what).str();
  }

  return text;
}

std::string ErrorTextAssignable::functions() {
  return "; the types of functions must match exactly: wrap it in an arrow that converts "
         "what it needs";
}

std::string ErrorTextAssignable::stream() {
  return "; 'new Response' and 'new Request' take no streams: a stream is sent with "
         "'Response.stream(stream, init)', 'Request.stream(input, stream, options)' or "
         "'fetch(url, { body: stream })'";
}

std::string ErrorTextAssignable::request() {
  return "; 'new Request' takes 'RequestOptions', whose body is never a stream; a "
         "'RequestInit' goes to 'fetch(url, init)'";
}

std::string ErrorTextAssignable::never() {
  return "; nothing fits 'never': if it comes from a type argument left out, write it, "
         "e.g. 'new ReadableStream<int, MyError>(...)' for a stream that fails with "
         "MyError";
}

std::string ErrorTextAssignable::extendsBus(llvm::StringRef name) {
  return (llvm::Twine("class Bus extends ") + name + " {}").str();
}

std::string ErrorTextAssignable::extendsInt(llvm::StringRef name) {
  return (llvm::Twine("class Int") + name + " extends " + name + "<int> {}").str();
}

std::string ErrorTextAssignable::generic(llvm::StringRef name, llvm::StringRef example) {
  return (llvm::Twine("; each list of type arguments of '") + name +
          "' makes a class of its own, and 'new' of this value could make none of them: "
          "give a class that extends it, e.g. '" +
          example + "', or take the class as an 'object'")
      .str();
}

std::string ErrorTextAssignable::literal(llvm::StringRef to) {
  return (llvm::Twine(
              "; an object literal is never an instance of a class: create one with 'new ") +
          to + "(...)'")
      .str();
}

std::string ErrorTextAssignable::methods() {
  return "; an object literal with methods has a type of its own; to give it this type, "
         "write its functions as fields, 'f: () => ...'";
}

std::string ErrorTextAssignable::classes(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("; classes are told apart by their names, whatever their fields, and '") +
          from + "' does not extend '" + to + "'")
      .str();
}

std::string ErrorTextAssignable::implements(llvm::StringRef to) {
  return (llvm::Twine("; an instance of a class is only of its class's type and of those it "
                      "extends or implements: write 'implements ") +
          to + "' in the class")
      .str();
}

std::string ErrorTextAssignable::inUnion() {
  return "; an object literal goes into a union as one of its members: it needs the "
         "fields of one of them";
}

std::string ErrorTextAssignable::withValue(llvm::StringRef field) {
  return (llvm::Twine(", with its value of '") + field + "'").str();
}

std::string ErrorTextAssignable::tuple() {
  return "; a tuple is not an array, though it is written like one: copy it into an array with "
         "'[...t]'";
}

std::string ErrorTextAssignable::both(llvm::StringRef other) {
  return (llvm::Twine("; the same object also goes where '") + other +
          "' is expected, and one object cannot be both")
      .str();
}

std::string ErrorTextAssignable::undefined() {
  return "; the value may be undefined: check it first (e.g. 'x != undefined') or give a "
         "default with '??'";
}

std::string ErrorTextAssignable::json() {
  return "; a Json may hold any JSON value: check what it holds first (e.g. 'typeof x == "
         "\"string\"' or 'Array.isArray(x)')";
}

std::string ErrorTextAssignable::object() {
  return "; only objects of the language are 'object's (instances of classes, object "
         "literals, dates, dictionaries...); for any value at all there is 'unknown'";
}

std::string ErrorTextAssignable::jsonObject() {
  return "; an array or an object becomes a Json only as a literal written where a Json is "
         "expected ('[...]', '{ ... }'), or as a 'Json[]', which stays the same array";
}

std::string ErrorTextAssignable::wideJson() {
  return "; a number in a Json is a double, which holds every integer only within "
         "±9007199254740992 (2^53), and nothing proves the values of this dictionary are "
         "there: build a 'Record<string, Json>' and check each value as it goes in, or "
         "keep them in a narrower type such as 'int32'";
}

std::string ErrorTextAssignable::noDecimal() {
  return "; JSON has no decimals: write it as text with 'String(x)', as JSON.stringify does";
}

std::string ErrorTextAssignable::toDecimal() {
  return "; a double is inexact already, so it becomes a decimal only explicitly: "
         "'decimal(x)'";
}

std::string ErrorTextAssignable::toNumber() {
  return "; a decimal becomes a number only explicitly: 'Number(x)' gives the nearest double";
}

std::string ErrorTextAssignable::alsoHere(llvm::StringRef other) {
  return (llvm::Twine("the object also goes here, as '") + other + "'").str();
}

std::string ErrorTextConvert::direct(llvm::StringRef text, llvm::StringRef method) {
  return (llvm::Twine("; convert it explicitly: '") + text + "." + method + "()'").str();
}

std::string ErrorTextConvert::rounded(llvm::StringRef text, llvm::StringRef method) {
  return (llvm::Twine("; round it first, then convert: 'Math.trunc(") + text + ")." + method +
          "()' (or Math.round, Math.floor, Math.ceil)")
      .str();
}

}
