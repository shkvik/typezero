#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class LibraryValuesRegistryGroupKeyErrorCode {
  NO_GROUP_KEY,
  KEYS_NOT_STRINGS,
  BAD_KEY_TYPE,
};

struct LibraryValuesRegistryGroupKeyError {
  LibraryValuesRegistryGroupKeyErrorCode code;
  std::string message;
};

enum class LibraryValuesRegistryListedTypeErrorCode {
  NO_COMMON_TYPE,
  NO_FIELDS,
};

struct LibraryValuesRegistryListedTypeError {
  LibraryValuesRegistryListedTypeErrorCode code;
  std::string message;
};

enum class LibraryValuesRegistryStringMethodErrorCode {
  NO_SUCH_METHOD,
};

struct LibraryValuesRegistryStringMethodError {
  LibraryValuesRegistryStringMethodErrorCode code;
  std::string message;
};

class ErrorTextStringMethod {
public:
  static std::string missing(llvm::StringRef name);
  static std::string repeatOf();
};

class ErrorTextNumberStatic {
public:
  static std::string rawTag();
  static std::string noProperty(llvm::StringRef name, llvm::StringRef owner);
};

enum class LibraryValuesRegistryCheckConversionErrorCode {
  NOT_A_CONVERSION,
  WRONG_ARITY,
  DOUBLE_TO_DECIMAL,
  DOUBLE_TO_INTEGER,
};

struct LibraryValuesRegistryCheckConversionError {
  LibraryValuesRegistryCheckConversionErrorCode code;
  std::string message;
};

class ErrorTextConversion {
public:
  static std::string toDecimal(llvm::StringRef value);
  static std::string toInteger(llvm::StringRef value, llvm::StringRef method);
};

class ErrorTextNumberMethod {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string codePoint();
  static std::string radix();
  static std::string digits();
};

class ErrorTextArrayMethod {
public:
  static std::string results(llvm::StringRef name);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

enum class LibraryValuesRegistryTextElementsErrorCode {
  FUNCTIONS,
  RESULTS,
  UNKNOWN,
  JSON,
  OBJECTS,
};

struct LibraryValuesRegistryTextElementsError {
  LibraryValuesRegistryTextElementsErrorCode code;
  std::string message;
};

class ErrorTextArrayText {
public:
  static std::string functions();
  static std::string results(llvm::StringRef name);
  static std::string unknown(llvm::StringRef name);
  static std::string json(llvm::StringRef name);
  static std::string objects(llvm::StringRef name);
};

enum class LibraryValuesRegistrySortableErrorCode {
  FUNCTIONS,
  UNKNOWN,
  JSON,
  DATES,
  OBJECTS,
  MIXED_UNION,
  NUMBERS,
};

struct LibraryValuesRegistrySortableError {
  LibraryValuesRegistrySortableErrorCode code;
  std::string message;
};

class ErrorTextArraySort {
public:
  static std::string functions();
  static std::string unknown();
  static std::string json();
  static std::string dates();
  static std::string objects();
  static std::string mixed(llvm::StringRef type);
  static std::string numbers(bool optional);
};

class ErrorTextRecordMethod {
public:
  static std::string hasOwn(llvm::StringRef key, llvm::StringRef text);
  static std::string undefined(llvm::StringRef key);
  static std::string noMethods(llvm::StringRef key, llvm::StringRef value, llvm::StringRef text);
};

class ErrorTextDictForEach {
public:
  static std::string thisArg();
};

class ErrorTextSetCombine {
public:
  static std::string notSet(llvm::StringRef name, llvm::StringRef key, llvm::StringRef other);
};

class ErrorTextDictMethod {
public:
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type, llvm::StringRef text,
                                bool isMap);
};

enum class LibraryValuesRegistryCheckFromEntriesErrorCode {
  ALREADY_REPORTED,
  WRONG_ARITY,
  KEYS_NOT_STRINGS,
  VALUES_UNDEFINED,
};

struct LibraryValuesRegistryCheckFromEntriesError {
  LibraryValuesRegistryCheckFromEntriesErrorCode code;
  std::string message;
};

class ErrorTextFromEntries {
public:
  static std::string keys(llvm::StringRef type);
  static std::string undefined();
};

class ErrorTextGroupBy {
public:
  static std::string noKey(llvm::StringRef fn);
  static std::string keys(llvm::StringRef type);
};

class ErrorTextMapStatic {
public:
  static std::string unknown(llvm::StringRef name);
};

enum class LibraryValuesRegistryCheckDictObjectCallErrorCode {
  ALWAYS_EMPTY,
};

struct LibraryValuesRegistryCheckDictObjectCallError {
  LibraryValuesRegistryCheckDictObjectCallErrorCode code;
  std::string message;
};

class ErrorTextDictObject {
public:
  static std::string empty(llvm::StringRef fn, llvm::StringRef kind, llvm::StringRef text,
                           llvm::StringRef method);
};

class ErrorTextStaticFrom {
public:
  static std::string iterator(llvm::StringRef name);
  static std::string array(llvm::StringRef name);
};

enum class LibraryValuesRegistryArrayStaticErrorCode {
  NOT_SUPPORTED,
};

struct LibraryValuesRegistryArrayStaticError {
  LibraryValuesRegistryArrayStaticErrorCode code;
  std::string message;
};

enum class LibraryValuesRegistryIteratedSourceErrorCode {
  NOT_ITERABLE,
  ALREADY_REPORTED,
};

struct LibraryValuesRegistryIteratedSourceError {
  LibraryValuesRegistryIteratedSourceErrorCode code;
  std::string message;
};

class ErrorTextIterSource {
public:
  static std::string notIter(llvm::StringRef fn, llvm::StringRef type);
};

enum class LibraryValuesRegistryCallbackValueErrorCode {
  NO_VALUE,
};

struct LibraryValuesRegistryCallbackValueError {
  LibraryValuesRegistryCallbackValueErrorCode code;
  std::string message;
};

class ErrorTextCallbackValue {
public:
  static std::string missing(llvm::StringRef fn);
};

enum class LibraryValuesRegistryNextValueErrorCode {
  UNION_PROBLEM,
};

struct LibraryValuesRegistryNextValueError {
  LibraryValuesRegistryNextValueErrorCode code;
  std::string message;
};

class ErrorTextNextValue {
public:
  static std::string noUnion(llvm::StringRef iterator, llvm::StringRef element,
                             llvm::StringRef returned, llvm::StringRef problem);
};

enum class LibraryValuesRegistryPlainMethodErrorCode {
  NO_THROW,
  NO_RETURN,
};

struct LibraryValuesRegistryPlainMethodError {
  LibraryValuesRegistryPlainMethodErrorCode code;
  std::string message;
};

class ErrorTextIteratorMethod {
public:
  static std::string noThrow();
  static std::string noReturn();
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string noFunction(llvm::StringRef name);
  static std::string countOf(llvm::StringRef name);
};

enum class LibraryValuesRegistryIteratorHelperErrorCode {
  NO_SUCH_PROPERTY,
};

struct LibraryValuesRegistryIteratorHelperError {
  LibraryValuesRegistryIteratorHelperErrorCode code;
  std::string message;
};

enum class LibraryValuesRegistryCallbackGivenErrorCode {
  NO_FUNCTION,
  TOO_MANY_ARGUMENTS,
};

struct LibraryValuesRegistryCallbackGivenError {
  LibraryValuesRegistryCallbackGivenErrorCode code;
  std::string message;
};

enum class LibraryValuesRegistryInitialValueErrorCode {
  UNTYPED_INITIAL,
};

struct LibraryValuesRegistryInitialValueError {
  LibraryValuesRegistryInitialValueErrorCode code;
  std::string message;
};

class ErrorTextReduceInit {
public:
  static std::string untyped();
};

enum class LibraryValuesRegistryFlatMapInnerErrorCode {
  NOT_ITERABLE,
};

struct LibraryValuesRegistryFlatMapInnerError {
  LibraryValuesRegistryFlatMapInnerErrorCode code;
  std::string message;
};

class ErrorTextFlatMap {
public:
  static std::string notIter(llvm::StringRef type);
};

enum class LibraryValuesRegistryCheckGlobalFunctionErrorCode {
  ALREADY_REPORTED,
  UNESCAPE_OF_ENCODE,
  LEGACY_ESCAPE,
  LEGACY_UNESCAPE,
  DECODE_OF_ESCAPE,
  WRONG_ARITY,
  NUMBER_OF_UNDEFINED,
  NUMBER_OF_WRONG_TYPE,
};

struct LibraryValuesRegistryCheckGlobalFunctionError {
  LibraryValuesRegistryCheckGlobalFunctionErrorCode code;
  std::string message;
};

class ErrorTextGlobalFunction {
public:
  static std::string toBytes();
  static std::string escape();
  static std::string unescape();
  static std::string fromBytes();
  static std::string undefined();
  static std::string wrongType(llvm::StringRef type);
};

class ErrorTextObjectCall {
public:
  static std::string noSupport(llvm::StringRef fn);
  static std::string undefined();
  static std::string needObject(llvm::StringRef fn, llvm::StringRef type);
  static std::string emitter(llvm::StringRef fn);
  static std::string methods(llvm::StringRef fn);
  static std::string noUnion(llvm::StringRef fn, llvm::StringRef type, llvm::StringRef problem);
  static std::string noFields(llvm::StringRef type, llvm::StringRef fn);
  static std::string numberEnum(llvm::StringRef fn, llvm::StringRef name, llvm::StringRef member);
};

}
