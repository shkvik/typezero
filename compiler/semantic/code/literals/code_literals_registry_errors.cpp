#include "compiler/semantic/code/literals/code_literals_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextIntLit::tooBig(llvm::StringRef type) {
  return (llvm::Twine("integer literal does not fit in type '") + type + "'").str();
}

std::string ErrorTextObjectLiteral::defaulted(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          " = ...' gives a default value, which only a destructuring "
          "pattern such as '({ " +
          name + " = 1 } = o)' has; in an object literal write '" + name + ": ...'")
      .str();
}

std::string ErrorTextObjectLiteral::numberKey() {
  return "a number cannot name a field; an object with numbered entries is an "
         "array";
}

std::string ErrorTextObjectLiteral::runtimeKey() {
  return "a key in brackets must be known when compiling: a string "
         "literal, or a constant declared with one, such as 'const key = "
         "\"name\"'; "
         "keys computed when the program runs need a dictionary: declare it as "
         "one, e.g. "
         "'const d: Record<string, int> = {}'";
}

std::string ErrorTextObjectLiteral::keyType(llvm::StringRef type) {
  return (llvm::Twine("a key must be a string, found '") + type + "'").str();
}

std::string ErrorTextObjectLiteral::duplicate(llvm::StringRef name) {
  return (llvm::Twine("duplicate field '") + name + "' in the object literal").str();
}

std::string ErrorTextObjectLiteral::inferFrom(llvm::StringRef type) {
  return (llvm::Twine("cannot infer the type of the fields from '") + type +
          "'; declare the type of the object, e.g. 'const o: { a?: int "
          "} = ...'")
      .str();
}

std::string ErrorTextObjectLiteral::keyTypes(llvm::StringRef name) {
  return (llvm::Twine("field '") + name +
          "' gets values of different types; declare "
          "the type of the object")
      .str();
}

std::string ErrorTextObjectLiteral::keySpread(llvm::StringRef type) {
  return (llvm::Twine("a key of type '") + type +
          "' in brackets works in an object literal without '...' "
          "and methods; assign it after: 'o[key] = value'")
      .str();
}

std::string ErrorTextObjectLiteral::keyNoType(llvm::StringRef type) {
  return (llvm::Twine("a key of type '") + type +
          "' in brackets needs the type of the object, which says "
          "the fields it may name; write it, e.g. 'const o: T = { [key]: "
          "value }'")
      .str();
}

std::string ErrorTextObjectLiteral::keyFields(llvm::StringRef type, llvm::StringRef target) {
  return (llvm::Twine("the fields that '") + type + "' names must all be in '" + target +
          "' and have one type")
      .str();
}

std::string ErrorTextObjectLiteral::keyedOpt(llvm::StringRef type, llvm::StringRef field) {
  return (llvm::Twine("the fields that '") + type + "' names are of type '" + field +
          "', not optional; the object gets only one of them, so the "
          "others would be missing")
      .str();
}

std::string ErrorTextObjectLiteral::noField(llvm::StringRef target, llvm::StringRef name,
                                            bool stream) {
  std::string message = (llvm::Twine("type '") + target + "' has no field '" + name +
                         "'; an object must have exactly the fields of its type")
                            .str();
  if (stream) {
    message += "; an error with fields of its own has a type of its own, e.g. "
               "'interface "
               "MyError extends StreamError { ... }', and the stream names it: "
               "'new ReadableStream<int, MyError>(...)', 'class Lines extends "
               "Readable<string, MyError>'";
  }

  return message;
}

std::string ErrorTextObjectLiteral::voidValue(llvm::StringRef what) {
  return (llvm::Twine("an expression of type 'void' cannot be the value of ") + what).str();
}

std::string ErrorTextObjectLiteral::undefOf(llvm::StringRef what, llvm::StringRef name) {
  return (llvm::Twine("cannot infer the type of ") + what +
          " from 'undefined'; declare the type of the object, e.g. "
          "'const o: { " +
          name + "?: int } = ...'")
      .str();
}

std::string ErrorTextObjectLiteral::missing(llvm::StringRef name, llvm::StringRef target) {
  return (llvm::Twine("field '") + name + "' is missing; type '" + target + "' requires it").str();
}

std::string ErrorTextObjectLiteral::overloads(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("an object literal cannot be of type '") + target + "': its method '" + name +
          "' has several signatures, and only a class implements "
          "those, each with a variant of its own")
      .str();
}

std::string ErrorTextObjectLiteral::keyedName(llvm::StringRef type) {
  return (llvm::Twine("the field that '") + type + "' names").str();
}

std::string ErrorTextObjectLiteral::fieldName(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "'").str();
}

std::string ErrorTextSpreadLiteral::undefined() {
  return "the object may be undefined; check it before spreading it";
}

std::string ErrorTextSpreadLiteral::notObject(llvm::StringRef type, bool indices) {
  std::string message =
      (llvm::Twine("only objects can be spread into an object literal, found '") + type + "'")
          .str();
  if (indices) {
    message += "; JavaScript would make its indices the keys";
  }

  return message;
}

std::string ErrorTextSpreadLiteral::methods() {
  return "an object with methods cannot be spread into an object "
         "literal yet; copy the fields by name";
}

std::string ErrorTextSpreadLiteral::twoTypes(llvm::StringRef name, llvm::StringRef before,
                                             llvm::StringRef type, llvm::StringRef spread) {
  return (llvm::Twine("field '") + name + "' is '" + before + "' before the spread and '" + type +
          "' in '" + spread + "'; a field has one type")
      .str();
}

std::string ErrorTextArrayHole::hole() {
  return "arrays cannot have holes; write the element explicitly";
}

std::string ErrorTextTupleSpread::differ(llvm::StringRef type) {
  return (llvm::Twine("the elements of '") + type +
          "' have different types, and an array holds elements of one type")
      .str();
}

std::string ErrorTextSpreadable::notSpread(llvm::StringRef type, bool record) {
  return (llvm::Twine("only arrays, strings, iterators, Maps and Sets can be spread "
                      "here, found '") +
          type + "'" +
          (record ? "; for the entries of a Record, spread "
                    "'Object.entries(r)'"
                  : ""))
      .str();
}

std::string ErrorTextSpreadable::spread() {
  return "a spread";
}

std::string ErrorTextArrayEmpty::empty() {
  return "cannot infer the element type of an empty array; add a type, "
         "e.g. 'let a: int[] = []'";
}

std::string ErrorTextArrayTypes::differ(llvm::StringRef element, llvm::StringRef other) {
  return (llvm::Twine("array elements must have one type; found '") + element + "' and '" + other +
          "'")
      .str();
}

std::string ErrorTextArrayTypes::literals() {
  return "; each object literal with methods has a type of its own, so objects "
         "made "
         "by different literals differ; an interface with their fields and "
         "methods "
         "joins them: 'const xs: Counter[] = [...]'";
}

std::string ErrorTextArrayTypes::tuple(llvm::StringRef types) {
  return (llvm::Twine("; for a value of each type in its own place, declare a "
                      "tuple, e.g. "
                      "'const t: [") +
          types + "] = ...'")
      .str();
}

std::string ErrorTextArrayElement::alone(llvm::StringRef element) {
  return (llvm::Twine("cannot infer the element type from '") + element +
          "' alone; add a type, e.g. 'let a: (int | undefined)[] = ...'")
      .str();
}

std::string ErrorTextSpreadFits::into(llvm::StringRef from, llvm::StringRef to) {
  return (llvm::Twine("cannot spread '") + from + "' into '" + to + "'").str();
}

std::string ErrorTextRecordProperty::defaulted(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          " = ...' gives a default value, which only a "
          "destructuring pattern has; in an object literal "
          "write '" +
          name + ": ...'")
      .str();
}

std::string ErrorTextRecordProperty::method(llvm::StringRef name) {
  return (llvm::Twine("a Record holds values, not methods; store a function, '") + name +
          ": (...) => ...'")
      .str();
}

std::string ErrorTextRecordProperty::keyLabel(llvm::StringRef name) {
  return (llvm::Twine("key '") + name + "'").str();
}

std::string ErrorTextRecordProperty::valueLabel() {
  return "the value";
}

std::string ErrorTextRecordSpread::undefined() {
  return "the object may be undefined; check it before spreading it";
}

std::string ErrorTextRecordSpread::keyedRest() {
  return "the rest of a pattern that reads a key in brackets is not supported "
         "for a Record; name the keys it leaves out";
}

std::string ErrorTextRecordSpread::values(llvm::StringRef type, llvm::StringRef value) {
  return (llvm::Twine("the values of '") + type + "' are not '" + value + "'").str();
}

std::string ErrorTextRecordSpread::field(llvm::StringRef field, llvm::StringRef type,
                                         llvm::StringRef fieldType, llvm::StringRef value) {
  return (llvm::Twine("field '") + field + "' of '" + type + "' is '" + fieldType +
          "', and the values here are '" + value + "'")
      .str();
}

std::string ErrorTextRecordSpread::methods() {
  return "an object with methods cannot be spread into a Record; copy "
         "its fields by name";
}

std::string ErrorTextRecordSpread::notObject(llvm::StringRef type, bool collection) {
  return (llvm::Twine("only objects and Records can be spread into a Record, found '") + type +
          "'" +
          (collection ? "; in JavaScript a Map or a Set has no own properties, "
                        "so this would "
                        "add nothing"
                      : ""))
      .str();
}

std::string ErrorTextRecordValues::differ(llvm::StringRef value, llvm::StringRef other) {
  return (llvm::Twine("spreading a Record makes a Record, whose values have "
                      "one type; found '") +
          value + "' and '" + other + "'; declare its type, e.g. 'const r: Record<string, " +
          value + "> = { ... }'")
      .str();
}

std::string ErrorTextRecordValue::undefined() {
  return "cannot infer the type of the values from 'undefined'; declare it, "
         "e.g. 'const r: Record<string, int | undefined> = { ... }'";
}

std::string ErrorTextLiteralSpread::spread() {
  return "an object literal with methods cannot take the fields of another "
         "object with '...'; write them out";
}

std::string ErrorTextPlainMethod::generic() {
  return "a method of an object literal cannot be generic; declare a class, "
         "whose methods can";
}

std::string ErrorTextFieldValue::label(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "'").str();
}

std::string ErrorTextFieldValue::isVoid(llvm::StringRef label) {
  return (llvm::Twine("an expression of type 'void' cannot be the value of ") + label).str();
}

std::string ErrorTextFieldValue::undefined(llvm::StringRef label) {
  return (llvm::Twine("cannot infer the type of ") + label + " from 'undefined'").str();
}

std::string ErrorTextLiteralFits::noField(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("the object literal does not fit '") + target + "': it has no field '" +
          name + "'; an object must have exactly the fields of its type")
      .str();
}

std::string ErrorTextLiteralFits::notMethod(llvm::StringRef target, llvm::StringRef name,
                                            llvm::StringRef type) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name +
          "' is a field of type '" + type + "' there, not a method")
      .str();
}

std::string ErrorTextLiteralFits::replaced(llvm::StringRef target, llvm::StringRef name) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name +
          "' is a field there that code may replace, "
          "which a method cannot be; write it as a field: '" +
          name + ": (...) => ...'")
      .str();
}

std::string ErrorTextLiteralFits::otherType(llvm::StringRef target, llvm::StringRef name,
                                            llvm::StringRef mine, llvm::StringRef theirs) {
  return (llvm::Twine("the object literal does not fit '") + target + "': '" + name + "' is '" +
          mine + "' here and '" + theirs + "' there")
      .str();
}

std::string ErrorTextLiteralMembers::missing(llvm::StringRef target, llvm::StringRef member) {
  return (llvm::Twine("the object literal does not fit '") + target + "': it has no '" + member +
          "'")
      .str();
}

std::string ErrorTextLiteralMembers::overloads(llvm::StringRef target, llvm::StringRef method) {
  return (llvm::Twine("the object literal does not fit '") + target + "': its method '" + method +
          "' has several signatures, and only a class implements those, "
          "each with a variant of its own")
      .str();
}

std::string ErrorTextTupleLit::spread(llvm::StringRef tuple) {
  return (llvm::Twine("a tuple cannot take spread values: type '") + tuple +
          "' has a fixed length; write each element")
      .str();
}

std::string ErrorTextTupleLit::length(llvm::StringRef tuple, size_t want, size_t got) {
  return (llvm::Twine("type '") + tuple + "' has " + std::to_string(want) +
          " elements, and this literal has " + std::to_string(got))
      .str();
}

std::string ErrorTextTupleLit::element(size_t index) {
  return (llvm::Twine("element ") + std::to_string(index)).str();
}

}
