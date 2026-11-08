#include "compiler/semantic/library/reflect/reflect_call_checker/reflect_call_checker_errors.h"

namespace tz {

std::string ErrorTextReflectCall::decorator() {
  return "'Reflect.metadata(key, value)' makes a decorator; write it as "
         "one, before a class or a member: '@Reflect.metadata(key, value)'";
}

std::string ErrorTextReflectCall::keys(llvm::StringRef fn) {
  return "'Reflect." + fn.str() +
         "' is not supported: the keys of records are "
         "decorators, classes and strings, which no one type holds; ask "
         "for each key with 'Reflect.hasMetadata(key, target)'";
}

std::string ErrorTextReflectCall::unknown(llvm::StringRef fn) {
  return "'Reflect." + fn.str() +
         "' is not supported; Reflect has get, apply, "
         "construct, fill, defineMetadata, getMetadata, getOwnMetadata, "
         "hasMetadata, hasOwnMetadata and deleteMetadata";
}

std::string ErrorTextMetadataKey::type(llvm::StringRef type) {
  return "a key of metadata is a decorator, a class or a constant, whose records "
         "have the type of what is written, or a string, whose records are "
         "'unknown'; found '" +
         type.str() + "'";
}

std::string ErrorTextMetadataKey::design() {
  return "the records 'design:*' are what the compiler writes for decorated "
         "members; write a key of your own";
}

std::string ErrorTextMetadataValue::voidValue() {
  return "an expression of type 'void' has no value to record";
}

std::string ErrorTextMetadataValue::label(llvm::StringRef label) {
  return "the records of '" + label.str() + "'";
}

std::string ErrorTextMetadataValue::since(llvm::StringRef label, llvm::StringRef type) {
  return "the records of '" + label.str() + "' are '" + type.str() + "' since the first one, here";
}

std::string ErrorTextMetadataValue::undefined(llvm::StringRef label) {
  return "cannot tell the type of the records of '" + label.str() +
         "' from 'undefined'; write a value of the type first";
}

std::string ErrorTextMetadataTarget::notObject(llvm::StringRef type) {
  return "the target of metadata is an object, such as a class, found '" + type.str() + "'";
}

std::string ErrorTextReflectArgs::tuple() {
  return "the arguments are an array, and this is a tuple; copy it into one, "
         "'[...t]'";
}

std::string ErrorTextReflectArgs::notArray(llvm::StringRef type) {
  return "the arguments are an array, e.g. '[a, b]', found '" + type.str() + "'";
}

}
