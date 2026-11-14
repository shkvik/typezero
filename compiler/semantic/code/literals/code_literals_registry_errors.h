#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeLiteralsRegistryCheckIntLitErrorCode {
  DOES_NOT_FIT,
};

struct CodeLiteralsRegistryCheckIntLitError {
  CodeLiteralsRegistryCheckIntLitErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryPropertyPlainErrorCode {
  DEFAULT_VALUE,
};

struct CodeLiteralsRegistryPropertyPlainError {
  CodeLiteralsRegistryPropertyPlainErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryComputedKeyErrorCode {
  NUMBER_KEY,
  RUNTIME_KEY,
  NOT_A_STRING,
};

struct CodeLiteralsRegistryComputedKeyError {
  CodeLiteralsRegistryComputedKeyErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryUniqueNameErrorCode {
  DUPLICATE_FIELD,
};

struct CodeLiteralsRegistryUniqueNameError {
  CodeLiteralsRegistryUniqueNameErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryInferredValueErrorCode {
  CANNOT_INFER,
};

struct CodeLiteralsRegistryInferredValueError {
  CodeLiteralsRegistryInferredValueErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySameKeyedTypeErrorCode {
  FIELD_TYPES_DIFFER,
};

struct CodeLiteralsRegistrySameKeyedTypeError {
  CodeLiteralsRegistrySameKeyedTypeErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryKeyedPlacementErrorCode {
  KEY_WITH_SPREAD,
};

struct CodeLiteralsRegistryKeyedPlacementError {
  CodeLiteralsRegistryKeyedPlacementErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryKeyedTargetErrorCode {
  NO_OBJECT_TYPE,
};

struct CodeLiteralsRegistryKeyedTargetError {
  CodeLiteralsRegistryKeyedTargetErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryKeyedFieldErrorCode {
  FIELDS_DIFFER,
  NOT_OPTIONAL,
};

struct CodeLiteralsRegistryKeyedFieldError {
  CodeLiteralsRegistryKeyedFieldErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryTargetFieldErrorCode {
  NO_SUCH_FIELD,
  OVERLOAD_NAME,
};

struct CodeLiteralsRegistryTargetFieldError {
  CodeLiteralsRegistryTargetFieldErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryValueTypedErrorCode {
  VOID_VALUE,
  UNDEFINED_VALUE,
};

struct CodeLiteralsRegistryValueTypedError {
  CodeLiteralsRegistryValueTypedErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryFieldGivenErrorCode {
  MISSING_FIELD,
};

struct CodeLiteralsRegistryFieldGivenError {
  CodeLiteralsRegistryFieldGivenErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadDefinedErrorCode {
  MAY_BE_UNDEFINED,
};

struct CodeLiteralsRegistrySpreadDefinedError {
  CodeLiteralsRegistrySpreadDefinedErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadObjectErrorCode {
  NOT_AN_OBJECT,
};

struct CodeLiteralsRegistrySpreadObjectError {
  CodeLiteralsRegistrySpreadObjectErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadFieldsErrorCode {
  METHOD_FIELDS,
};

struct CodeLiteralsRegistrySpreadFieldsError {
  CodeLiteralsRegistrySpreadFieldsErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadJoinErrorCode {
  TYPES_DIFFER,
};

struct CodeLiteralsRegistrySpreadJoinError {
  CodeLiteralsRegistrySpreadJoinErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryElementGivenErrorCode {
  HOLE_IN_ARRAY,
};

struct CodeLiteralsRegistryElementGivenError {
  CodeLiteralsRegistryElementGivenErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryTupleSpreadErrorCode {
  ELEMENTS_DIFFER,
};

struct CodeLiteralsRegistryTupleSpreadError {
  CodeLiteralsRegistryTupleSpreadErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadableErrorCode {
  NOT_SPREADABLE,
};

struct CodeLiteralsRegistrySpreadableError {
  CodeLiteralsRegistrySpreadableErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryNotEmptyErrorCode {
  EMPTY_ARRAY,
};

struct CodeLiteralsRegistryNotEmptyError {
  CodeLiteralsRegistryNotEmptyErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryTypesJoinErrorCode {
  ELEMENT_TYPES_DIFFER,
};

struct CodeLiteralsRegistryTypesJoinError {
  CodeLiteralsRegistryTypesJoinErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryElementKnownErrorCode {
  ONLY_UNDEFINED,
};

struct CodeLiteralsRegistryElementKnownError {
  CodeLiteralsRegistryElementKnownErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistrySpreadFitsErrorCode {
  SPREAD_MISMATCH,
};

struct CodeLiteralsRegistrySpreadFitsError {
  CodeLiteralsRegistrySpreadFitsErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryPlainPropertyErrorCode {
  DEFAULT_IN_LITERAL,
  METHOD_IN_RECORD,
};

struct CodeLiteralsRegistryPlainPropertyError {
  CodeLiteralsRegistryPlainPropertyErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryCheckRecordSpreadErrorCode {
  MAY_BE_UNDEFINED,
  KEYED_REST,
  OTHER_VALUES,
  FIELD_TYPE_DIFFERS,
  HAS_METHODS,
  NOT_SPREADABLE,
};

struct CodeLiteralsRegistryCheckRecordSpreadError {
  CodeLiteralsRegistryCheckRecordSpreadErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryValuesJoinErrorCode {
  VALUE_TYPES_DIFFER,
};

struct CodeLiteralsRegistryValuesJoinError {
  CodeLiteralsRegistryValuesJoinErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryValueKnownErrorCode {
  ONLY_UNDEFINED,
};

struct CodeLiteralsRegistryValueKnownError {
  CodeLiteralsRegistryValueKnownErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryNoSpreadErrorCode {
  SPREAD_IN_LITERAL,
};

struct CodeLiteralsRegistryNoSpreadError {
  CodeLiteralsRegistryNoSpreadErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryPlainMethodErrorCode {
  GENERIC_METHOD,
};

struct CodeLiteralsRegistryPlainMethodError {
  CodeLiteralsRegistryPlainMethodErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryFieldValueErrorCode {
  VOID_VALUE,
  UNDEFINED_VALUE,
};

struct CodeLiteralsRegistryFieldValueError {
  CodeLiteralsRegistryFieldValueErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryPropertyFitsErrorCode {
  NO_SUCH_FIELD,
  NOT_A_METHOD,
  REPLACEABLE_FIELD,
  METHOD_TYPE_DIFFERS,
};

struct CodeLiteralsRegistryPropertyFitsError {
  CodeLiteralsRegistryPropertyFitsErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryMembersMatchErrorCode {
  MISSING_MEMBER,
  OVERLOADED_METHOD,
};

struct CodeLiteralsRegistryMembersMatchError {
  CodeLiteralsRegistryMembersMatchErrorCode code;
  std::string message;
};

enum class CodeLiteralsRegistryCheckTupleLitErrorCode {
  SPREAD_IN_TUPLE,
  WRONG_LENGTH,
};

struct CodeLiteralsRegistryCheckTupleLitError {
  CodeLiteralsRegistryCheckTupleLitErrorCode code;
  std::string message;
};

class ErrorTextIntLit {
public:
  static std::string tooBig(llvm::StringRef type);
};

class ErrorTextObjectLiteral {
public:
  static std::string defaulted(llvm::StringRef name);
  static std::string numberKey();
  static std::string runtimeKey();
  static std::string keyType(llvm::StringRef type);
  static std::string duplicate(llvm::StringRef name);
  static std::string inferFrom(llvm::StringRef type);
  static std::string keyTypes(llvm::StringRef name);
  static std::string keySpread(llvm::StringRef type);
  static std::string keyNoType(llvm::StringRef type);
  static std::string keyFields(llvm::StringRef type, llvm::StringRef target);
  static std::string keyedOpt(llvm::StringRef type, llvm::StringRef field);
  static std::string noField(llvm::StringRef target, llvm::StringRef name, bool stream);
  static std::string voidValue(llvm::StringRef what);
  static std::string undefOf(llvm::StringRef what, llvm::StringRef name);
  static std::string missing(llvm::StringRef name, llvm::StringRef target);
  static std::string overloads(llvm::StringRef target, llvm::StringRef name);
  static std::string keyedName(llvm::StringRef type);
  static std::string fieldName(llvm::StringRef name);
};

class ErrorTextSpreadLiteral {
public:
  static std::string undefined();
  static std::string notObject(llvm::StringRef type, bool indices);
  static std::string methods();
  static std::string twoTypes(llvm::StringRef name, llvm::StringRef before, llvm::StringRef type,
                              llvm::StringRef spread);
};

class ErrorTextArrayHole {
public:
  static std::string hole();
};

class ErrorTextTupleSpread {
public:
  static std::string differ(llvm::StringRef type);
};

class ErrorTextSpreadable {
public:
  static std::string notSpread(llvm::StringRef type, bool record);
  static std::string spread();
};

class ErrorTextArrayEmpty {
public:
  static std::string empty();
};

class ErrorTextArrayTypes {
public:
  static std::string differ(llvm::StringRef element, llvm::StringRef other);
  static std::string literals();
  static std::string tuple(llvm::StringRef types);
};

class ErrorTextArrayElement {
public:
  static std::string alone(llvm::StringRef element);
};

class ErrorTextSpreadFits {
public:
  static std::string into(llvm::StringRef from, llvm::StringRef to);
};

class ErrorTextRecordProperty {
public:
  static std::string defaulted(llvm::StringRef name);
  static std::string method(llvm::StringRef name);
  static std::string keyLabel(llvm::StringRef name);
  static std::string valueLabel();
};

class ErrorTextRecordSpread {
public:
  static std::string undefined();
  static std::string keyedRest();
  static std::string values(llvm::StringRef type, llvm::StringRef value);
  static std::string field(llvm::StringRef field, llvm::StringRef type, llvm::StringRef fieldType,
                           llvm::StringRef value);
  static std::string methods();
  static std::string notObject(llvm::StringRef type, bool collection);
};

class ErrorTextRecordValues {
public:
  static std::string differ(llvm::StringRef value, llvm::StringRef other);
};

class ErrorTextRecordValue {
public:
  static std::string undefined();
};

class ErrorTextLiteralSpread {
public:
  static std::string spread();
};

class ErrorTextPlainMethod {
public:
  static std::string generic();
};

class ErrorTextFieldValue {
public:
  static std::string label(llvm::StringRef name);
  static std::string isVoid(llvm::StringRef label);
  static std::string undefined(llvm::StringRef label);
};

class ErrorTextLiteralFits {
public:
  static std::string noField(llvm::StringRef target, llvm::StringRef name);
  static std::string notMethod(llvm::StringRef target, llvm::StringRef name, llvm::StringRef type);
  static std::string replaced(llvm::StringRef target, llvm::StringRef name);
  static std::string otherType(llvm::StringRef target, llvm::StringRef name, llvm::StringRef mine,
                               llvm::StringRef theirs);
};

class ErrorTextLiteralMembers {
public:
  static std::string missing(llvm::StringRef target, llvm::StringRef member);
  static std::string overloads(llvm::StringRef target, llvm::StringRef method);
};

class ErrorTextTupleLit {
public:
  static std::string spread(llvm::StringRef tuple);
  static std::string length(llvm::StringRef tuple, size_t want, size_t got);
  static std::string element(size_t index);
};

}
