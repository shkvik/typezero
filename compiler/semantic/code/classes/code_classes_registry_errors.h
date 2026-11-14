#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeClassesRegistryCheckThisErrorCode {
  THIS_NOT_FOUND,
  THIS_NOT_DECLARED,
  THIS_NOT_READY,
};

struct CodeClassesRegistryCheckThisError {
  CodeClassesRegistryCheckThisErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckClassStmtErrorCode {
  NOT_TOP_LEVEL,
};

struct CodeClassesRegistryCheckClassStmtError {
  CodeClassesRegistryCheckClassStmtErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckFieldsSetErrorCode {
  FIELD_NOT_SET_ON_EVERY_PATH,
};

struct CodeClassesRegistryCheckFieldsSetError {
  CodeClassesRegistryCheckFieldsSetErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryIsInitThisErrorCode {
  NOT_IN_CONSTRUCTOR,
  NOT_THIS,
};

struct CodeClassesRegistryIsInitThisError {
  CodeClassesRegistryIsInitThisErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryDeclareThisErrorCode {
  NOT_A_METHOD,
};

struct CodeClassesRegistryDeclareThisError {
  CodeClassesRegistryDeclareThisErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryHasDecoratorsErrorCode {
  NO_DECORATORS,
};

struct CodeClassesRegistryHasDecoratorsError {
  CodeClassesRegistryHasDecoratorsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryThisReadyErrorCode {
  BEFORE_SUPER,
  FIELD_NOT_SET,
};

struct CodeClassesRegistryThisReadyError {
  CodeClassesRegistryThisReadyErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFirstUnsetErrorCode {
  NOT_IN_CONSTRUCTOR,
  ALL_FIELDS_SET,
};

struct CodeClassesRegistryFirstUnsetError {
  CodeClassesRegistryFirstUnsetErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldStartValueErrorCode {
  NO_START_VALUE,
  NEVER_SET,
};

struct CodeClassesRegistryFieldStartValueError {
  CodeClassesRegistryFieldStartValueErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryOverrideReturnErrorCode {
  RETURNS_OTHER_TYPE,
};

struct CodeClassesRegistryOverrideReturnError {
  CodeClassesRegistryOverrideReturnErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryThisOfErrorCode {
  OUTSIDE_METHOD,
  IN_STATIC_FIELD,
  IN_STATIC_METHOD,
  IN_PLAIN_FUNCTION,
};

struct CodeClassesRegistryThisOfError {
  CodeClassesRegistryThisOfErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryReadyThisErrorCode {
  IN_CLOSURE_BEFORE_FIELDS_SET,
  BEFORE_SUPER,
  BEFORE_FIELDS_SET,
};

struct CodeClassesRegistryReadyThisError {
  CodeClassesRegistryReadyThisErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckAccessErrorCode {
  PROTECTED_ELSEWHERE,
  PRIVATE_MEMBER,
  PROTECTED_MEMBER,
};

struct CodeClassesRegistryCheckAccessError {
  CodeClassesRegistryCheckAccessErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldSetErrorCode {
  READ_BEFORE_SET,
};

struct CodeClassesRegistryFieldSetError {
  CodeClassesRegistryFieldSetErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldTypedErrorCode {
  TYPE_FROM_INITIALIZER,
};

struct CodeClassesRegistryFieldTypedError {
  CodeClassesRegistryFieldTypedErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryInstanceMethodsErrorCode {
  STATIC_MEMBER,
  NO_SUCH_PROPERTY,
};

struct CodeClassesRegistryInstanceMethodsError {
  CodeClassesRegistryInstanceMethodsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryObjectReadyErrorCode {
  USED_BEFORE_SET,
};

struct CodeClassesRegistryObjectReadyError {
  CodeClassesRegistryObjectReadyErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMethodValueErrorCode {
  GENERIC_METHOD,
};

struct CodeClassesRegistryMethodValueError {
  CodeClassesRegistryMethodValueErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryStatsDateErrorCode {
  DERIVED_DATE,
};

struct CodeClassesRegistryStatsDateError {
  CodeClassesRegistryStatsDateErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryAssignableFieldErrorCode {
  SETTER_METHOD,
  LIBRARY_METHOD,
  CLASS_METHOD,
  NO_SUCH_PROPERTY,
};

struct CodeClassesRegistryAssignableFieldError {
  CodeClassesRegistryAssignableFieldErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldWritableErrorCode {
  READ_ONLY,
};

struct CodeClassesRegistryFieldWritableError {
  CodeClassesRegistryFieldWritableErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckClassRefMemberErrorCode {
  NO_PROTOTYPES,
  RUNTIME_CLASS,
  VALUE_ONLY_CLASS,
};

struct CodeClassesRegistryCheckClassRefMemberError {
  CodeClassesRegistryCheckClassRefMemberErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryValueNewClassErrorCode {
  NOT_A_CLASS,
};

struct CodeClassesRegistryValueNewClassError {
  CodeClassesRegistryValueNewClassErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryTypedClassErrorCode {
  VALUE_ONLY_CLASS,
};

struct CodeClassesRegistryTypedClassError {
  CodeClassesRegistryTypedClassErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryRuntimeTypeArgsErrorCode {
  TYPE_ARGUMENTS_GIVEN,
};

struct CodeClassesRegistryRuntimeTypeArgsError {
  CodeClassesRegistryRuntimeTypeArgsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryArgumentValueErrorCode {
  VOID_ARGUMENT,
};

struct CodeClassesRegistryArgumentValueError {
  CodeClassesRegistryArgumentValueErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryClassDeclaredErrorCode {
  USED_BEFORE_DECLARATION,
};

struct CodeClassesRegistryClassDeclaredError {
  CodeClassesRegistryClassDeclaredErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryNamesDistinctErrorCode {
  SAME_NAME,
};

struct CodeClassesRegistryNamesDistinctError {
  CodeClassesRegistryNamesDistinctErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySecondConstructorErrorCode {
  TWO_DECORATED_CONSTRUCTORS,
};

struct CodeClassesRegistrySecondConstructorError {
  CodeClassesRegistrySecondConstructorErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMemberDecoratableErrorCode {
  ACCESSOR_FIELD,
  ABSTRACT_METHOD,
};

struct CodeClassesRegistryMemberDecoratableError {
  CodeClassesRegistryMemberDecoratableErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryDecoratorFitsErrorCode {
  TOO_MANY_ARGUMENTS,
  DESCRIPTOR_FOR_PARAMETER,
  POSITION_FOR_METHOD,
};

struct CodeClassesRegistryDecoratorFitsError {
  CodeClassesRegistryDecoratorFitsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryGenericDescriptorErrorCode {
  GENERIC_CLASS_DESCRIPTOR,
};

struct CodeClassesRegistryGenericDescriptorError {
  CodeClassesRegistryGenericDescriptorErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMetadataArgsErrorCode {
  WRONG_METADATA_ARGUMENTS,
};

struct CodeClassesRegistryMetadataArgsError {
  CodeClassesRegistryMetadataArgsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryDecoratorResultErrorCode {
  RUNTIME_CLASS,
  CLASS_RESULT,
  METHOD_RESULT,
  OTHER_RESULT,
};

struct CodeClassesRegistryDecoratorResultError {
  CodeClassesRegistryDecoratorResultErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryAbstractNeedsClassErrorCode {
  ABSTRACT_IN_CONCRETE,
};

struct CodeClassesRegistryAbstractNeedsClassError {
  CodeClassesRegistryAbstractNeedsClassErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryAbstractImplementedErrorCode {
  NOT_IMPLEMENTED,
};

struct CodeClassesRegistryAbstractImplementedError {
  CodeClassesRegistryAbstractImplementedErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldMatchesErrorCode {
  FIELD_TYPE_DIFFERS,
};

struct CodeClassesRegistryFieldMatchesError {
  CodeClassesRegistryFieldMatchesErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMemberDeclaredErrorCode {
  NO_MEMBER,
};

struct CodeClassesRegistryMemberDeclaredError {
  CodeClassesRegistryMemberDeclaredErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMethodForFieldErrorCode {
  FIELD_AS_METHOD,
};

struct CodeClassesRegistryMethodForFieldError {
  CodeClassesRegistryMethodForFieldErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryReplaceableErrorCode {
  METHOD_NOT_FIELD,
};

struct CodeClassesRegistryReplaceableError {
  CodeClassesRegistryReplaceableErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryReturnMatchesErrorCode {
  RETURN_DIFFERS,
};

struct CodeClassesRegistryReturnMatchesError {
  CodeClassesRegistryReturnMatchesErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryVariantFoundErrorCode {
  OTHER_PARAMETERS,
};

struct CodeClassesRegistryVariantFoundError {
  CodeClassesRegistryVariantFoundErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySignatureVariantErrorCode {
  NO_VARIANT,
};

struct CodeClassesRegistrySignatureVariantError {
  CodeClassesRegistrySignatureVariantErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySuperAllowedErrorCode {
  NOT_IN_CONSTRUCTOR,
};

struct CodeClassesRegistrySuperAllowedError {
  CodeClassesRegistrySuperAllowedErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryParentClassErrorCode {
  NO_PARENT,
  BASE_ONLY,
};

struct CodeClassesRegistryParentClassError {
  CodeClassesRegistryParentClassErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryRootSuperErrorCode {
  NOT_A_STATEMENT,
};

struct CodeClassesRegistryRootSuperError {
  CodeClassesRegistryRootSuperErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySuperOnceErrorCode {
  CALLED_TWICE,
};

struct CodeClassesRegistrySuperOnceError {
  CodeClassesRegistrySuperOnceErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySuperArgumentsErrorCode {
  NO_CONSTRUCTOR,
};

struct CodeClassesRegistrySuperArgumentsError {
  CodeClassesRegistrySuperArgumentsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistrySuperContextErrorCode {
  NO_SUPER_CLASS,
  BASE_ONLY,
};

struct CodeClassesRegistrySuperContextError {
  CodeClassesRegistrySuperContextErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryObjectCompleteErrorCode {
  CALLED_BEFORE_SET,
};

struct CodeClassesRegistryObjectCompleteError {
  CodeClassesRegistryObjectCompleteErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryParentMethodsErrorCode {
  FIELD_OF_PARENT,
  NO_SUCH_METHOD,
};

struct CodeClassesRegistryParentMethodsError {
  CodeClassesRegistryParentMethodsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMethodBodyErrorCode {
  ABSTRACT_METHOD,
};

struct CodeClassesRegistryMethodBodyError {
  CodeClassesRegistryMethodBodyErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckInstanceOfErrorCode {
  UNTYPED_COLLECTION,
  NONE_MATCHES,
  ALL_MATCH,
  ALWAYS_TRUE,
  TEST_UNDEFINED,
  ALWAYS_FALSE,
  BYTES_INSTANCE,
  NOT_AN_OBJECT,
  NOT_A_CLASS,
  GENERIC_UNKNOWN,
  NEVER_GENERIC,
  NEVER_IN_UNION,
  NEVER_CLASS,
  NOT_IMPLEMENTED,
  NEVER_CREATED,
};

struct CodeClassesRegistryCheckInstanceOfError {
  CodeClassesRegistryCheckInstanceOfErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryCheckBytesInstanceOfErrorCode {
  NONE_MATCHES,
  ALL_MATCH,
  ALWAYS_TRUE,
  TEST_UNDEFINED,
  ALWAYS_FALSE,
};

struct CodeClassesRegistryCheckBytesInstanceOfError {
  CodeClassesRegistryCheckBytesInstanceOfErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldCallableErrorCode {
  MAY_BE_UNDEFINED,
  NOT_A_FUNCTION,
  ALREADY_REPORTED,
};

struct CodeClassesRegistryFieldCallableError {
  CodeClassesRegistryFieldCallableErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMethodCallInstanceMethodsErrorCode {
  STATIC_METHOD,
  NO_SUCH_PROPERTY,
};

struct CodeClassesRegistryMethodCallInstanceMethodsError {
  CodeClassesRegistryMethodCallInstanceMethodsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldsSetErrorCode {
  CALLED_BEFORE_SET,
};

struct CodeClassesRegistryFieldsSetError {
  CodeClassesRegistryFieldsSetErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryMethodTypeArgsErrorCode {
  NOT_GENERIC,
};

struct CodeClassesRegistryMethodTypeArgsError {
  CodeClassesRegistryMethodTypeArgsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryNotRuntimeErrorCode {
  RUNTIME_CLASS,
};

struct CodeClassesRegistryNotRuntimeError {
  CodeClassesRegistryNotRuntimeErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryClassTypeArgsErrorCode {
  NOT_GENERIC,
};

struct CodeClassesRegistryClassTypeArgsError {
  CodeClassesRegistryClassTypeArgsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryClassConcreteErrorCode {
  ABSTRACT_CLASS,
};

struct CodeClassesRegistryClassConcreteError {
  CodeClassesRegistryClassConcreteErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryArgumentsAbsentErrorCode {
  NO_CONSTRUCTOR,
};

struct CodeClassesRegistryArgumentsAbsentError {
  CodeClassesRegistryArgumentsAbsentErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryChooseVariantErrorCode {
  ALREADY_REPORTED,
  SPREAD_ARGUMENT,
  NO_VARIANT_FOR_COUNT,
  NO_VARIANT_FITS,
  GENERIC_VARIANT,
  SEVERAL_VARIANTS_FIT,
};

struct CodeClassesRegistryChooseVariantError {
  CodeClassesRegistryChooseVariantErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryVariantForTypeErrorCode {
  SEVERAL_VARIANTS,
};

struct CodeClassesRegistryVariantForTypeError {
  CodeClassesRegistryVariantForTypeErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryStaticFieldTypedErrorCode {
  USED_BEFORE_DECLARATION,
  TYPE_FROM_LATER_VALUE,
};

struct CodeClassesRegistryStaticFieldTypedError {
  CodeClassesRegistryStaticFieldTypedErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldReadyErrorCode {
  USED_BEFORE_DECLARATION,
  READ_BEFORE_SET,
};

struct CodeClassesRegistryFieldReadyError {
  CodeClassesRegistryFieldReadyErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryTypeDeclaredErrorCode {
  TYPE_FROM_LATER_VALUE,
};

struct CodeClassesRegistryTypeDeclaredError {
  CodeClassesRegistryTypeDeclaredErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryStaticMethodsErrorCode {
  INSTANCE_MEMBER,
  NO_STATIC_MEMBER,
};

struct CodeClassesRegistryStaticMethodsError {
  CodeClassesRegistryStaticMethodsErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryFieldFoundErrorCode {
  STATIC_METHOD,
  NO_STATIC_FIELD,
};

struct CodeClassesRegistryFieldFoundError {
  CodeClassesRegistryFieldFoundErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryStaticCallableErrorCode {
  NOT_A_FUNCTION,
  ALREADY_REPORTED,
};

struct CodeClassesRegistryStaticCallableError {
  CodeClassesRegistryStaticCallableErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryBaseClassErrorCode {
  ALREADY_REPORTED,
  KNOWN_ONLY_AT_RUN,
  NOT_A_CLASS,
  JAVASCRIPT_CLASS,
  GENERIC_CLASS,
  MADE_BY_FS,
};

struct CodeClassesRegistryBaseClassError {
  CodeClassesRegistryBaseClassErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryStaticTypeErrorCode {
  UNKNOWN_FROM_VALUE,
};

struct CodeClassesRegistryStaticTypeError {
  CodeClassesRegistryStaticTypeErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryOldObjectErrorCode {
  HOLDS_OLD_OBJECT,
};

struct CodeClassesRegistryOldObjectError {
  CodeClassesRegistryOldObjectErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryReplacedFieldErrorCode {
  FIELD_IN_REPLACEMENT,
};

struct CodeClassesRegistryReplacedFieldError {
  CodeClassesRegistryReplacedFieldErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryOverriddenCallErrorCode {
  CONSTRUCTOR_CALLS_OVERRIDE,
  CONSTRUCTOR_REACHES_OVERRIDE,
  INITIALIZERS_CALL_OVERRIDE,
  INITIALIZERS_REACH_OVERRIDE,
};

struct CodeClassesRegistryOverriddenCallError {
  CodeClassesRegistryOverriddenCallErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryConstructorTypeErrorCode {
  WRONG_TYPE_ARGUMENTS,
  NOT_AN_OBJECT_TYPE,
};

struct CodeClassesRegistryConstructorTypeError {
  CodeClassesRegistryConstructorTypeErrorCode code;
  std::string message;
};

enum class CodeClassesRegistryConstructorArgsErrorCode {
  NOT_IN_SUBCLASS,
  SEVERAL_CONSTRUCTORS,
  DECLARED_LATER,
};

struct CodeClassesRegistryConstructorArgsError {
  CodeClassesRegistryConstructorArgsErrorCode code;
  std::string message;
};

class ErrorTextThis {
public:
  static std::string undeclared(llvm::StringRef function);
};

class ErrorTextClassStmt {
public:
  static std::string nested(llvm::StringRef className);
};

class ErrorTextFieldsSet {
public:
  static std::string pathUnset(llvm::StringRef className, llvm::StringRef field);
};

class ErrorTextInitThis {
public:
  static std::string outside();
  static std::string notThis();
};

class ErrorTextDeclareThis {
public:
  static std::string notMethod(llvm::StringRef function);
};

class ErrorTextDecorators {
public:
  static std::string none(llvm::StringRef className);
};

class ErrorTextThisReady {
public:
  static std::string noSuper();
  static std::string unset(llvm::StringRef field);
};

class ErrorTextFirstUnset {
public:
  static std::string outside();
  static std::string allSet(llvm::StringRef className);
};

class ErrorTextStartValue {
public:
  static std::string noStart(llvm::StringRef field, llvm::StringRef type);
  static std::string neverSet(llvm::StringRef field, llvm::StringRef type);
};

class ErrorTextOverride {
public:
  static std::string otherType(llvm::StringRef method, llvm::StringRef mine,
                               llvm::StringRef className, llvm::StringRef theirs,
                               llvm::StringRef baseClass);
};

class ErrorTextThisOf {
public:
  static std::string outside();
  static std::string inField(llvm::StringRef className, llvm::StringRef member);
  static std::string inStatic(llvm::StringRef className, llvm::StringRef member);
  static std::string inFunction();
};

class ErrorTextReadyThis {
public:
  static std::string inClosure();
  static std::string noSuper(llvm::StringRef parentClass);
  static std::string unset(llvm::StringRef field);
};

class ErrorTextAccess {
public:
  static std::string elsewhere(llvm::StringRef name, llvm::StringRef cls, llvm::StringRef current,
                               llvm::StringRef other);
  static std::string hiddenPrivate(llvm::StringRef name, llvm::StringRef word, llvm::StringRef cls);
  static std::string hiddenProtected(llvm::StringRef name, llvm::StringRef word,
                                     llvm::StringRef cls);
};

class ErrorTextClassField {
public:
  static std::string readEarly(llvm::StringRef name);
  static std::string laterType(llvm::StringRef name);
  static std::string isStatic(llvm::StringRef name, llvm::StringRef cls);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
  static std::string earlyThis(llvm::StringRef name, llvm::StringRef field);
  static std::string genericVal(llvm::StringRef name);
};

class ErrorTextFieldTarget {
public:
  static std::string statsDate(llvm::StringRef name);
  static std::string setter(llvm::StringRef name, llvm::StringRef type, llvm::StringRef setter);
  static std::string library(llvm::StringRef name, llvm::StringRef type);
  static std::string method(llvm::StringRef name, llvm::StringRef type);
  static std::string readOnly(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextClassRefMember {
public:
  static std::string prototype();
  static std::string runtime(llvm::StringRef type);
  static std::string nameOnly(llvm::StringRef name);
};

class ErrorTextValueNew {
public:
  static std::string notClass(llvm::StringRef type);
  static std::string valueOnly(llvm::StringRef name);
  static std::string typeArgs();
  static std::string voidArg();
};

class ErrorTextDecoratedNames {
public:
  static std::string shared(llvm::StringRef what, llvm::StringRef cls, llvm::StringRef records);
  static std::string bothKinds(llvm::StringRef name);
  static std::string variants(llvm::StringRef name);
  static std::string design();
  static std::string recordsOf(llvm::StringRef shared);
  static std::string other();
  static std::string sameRecord(llvm::StringRef cls);
};

class ErrorTextDecorate {
public:
  static std::string accessor();
  static std::string abstract();
  static std::string arguments(llvm::StringRef label, llvm::StringRef place, llvm::StringRef needs,
                               llvm::StringRef given, llvm::StringRef args);
  static std::string descriptor(llvm::StringRef label);
  static std::string position(llvm::StringRef label);
  static std::string generic(llvm::StringRef cls);
  static std::string metadata();
};

class ErrorTextDecoratorResult {
public:
  static std::string runtime(llvm::StringRef label, llvm::StringRef result,
                             llvm::StringRef current);
  static std::string forClass(llvm::StringRef label, llvm::StringRef result);
  static std::string forMethod(llvm::StringRef label, llvm::StringRef result);
  static std::string other(llvm::StringRef label, llvm::StringRef result, llvm::StringRef place);
};

class ErrorTextDecoratorPlace {
public:
  static std::string aClass();
  static std::string aField();
  static std::string aMethod();
  static std::string aParameter();
  static std::string classArgs();
  static std::string fieldArgs();
  static std::string methodArgs();
  static std::string paramArgs();
  static std::string label(llvm::StringRef name);
};

class ErrorTextAbstract {
public:
  static std::string needsClass(llvm::StringRef fn, llvm::StringRef cls);
  static std::string notImpl(llvm::StringRef cls, llvm::StringRef variant, llvm::StringRef owner);
};

class ErrorTextImplements {
public:
  static std::string prefix(llvm::StringRef cls, llvm::StringRef iface);
  static std::string fieldType(llvm::StringRef prefix, llvm::StringRef name, llvm::StringRef type,
                               llvm::StringRef cls, llvm::StringRef actual);
  static std::string noMember(llvm::StringRef prefix, llvm::StringRef name, bool optional,
                              llvm::StringRef type);
  static std::string fieldNotFn(llvm::StringRef name, llvm::StringRef type, llvm::StringRef iface,
                                llvm::StringRef cls);
  static std::string mustField(llvm::StringRef name, llvm::StringRef iface, llvm::StringRef cls);
  static std::string returns(llvm::StringRef name, llvm::StringRef ret, llvm::StringRef iface,
                             llvm::StringRef cls, llvm::StringRef says);
  static std::string params(llvm::StringRef prefix, llvm::StringRef name, llvm::StringRef type,
                            llvm::StringRef cls, llvm::StringRef variants);
  static std::string noVariant(llvm::StringRef prefix, llvm::StringRef name,
                               llvm::StringRef signature);
};

class ErrorTextSuperCall {
public:
  static std::string outside();
  static std::string noParent(llvm::StringRef cls);
  static std::string statement();
  static std::string twice(llvm::StringRef parent);
  static std::string noArgs(llvm::StringRef parent);
};

class ErrorTextSuperMethod {
public:
  static std::string outside();
  static std::string early(llvm::StringRef name, llvm::StringRef field);
  static std::string isField(llvm::StringRef name, llvm::StringRef parent);
  static std::string noMethod(llvm::StringRef parent, llvm::StringRef name);
  static std::string abstract(llvm::StringRef name, llvm::StringRef owner);
};

class ErrorTextInstanceOf {
public:
  static std::string untypedMap(llvm::StringRef builtin, llvm::StringRef example);
  static std::string noneIs(llvm::StringRef base, llvm::StringRef what);
  static std::string allAre(llvm::StringRef base, llvm::StringRef what, llvm::StringRef name);
  static std::string alwaysTrue(llvm::StringRef type, llvm::StringRef name);
  static std::string orUndef(llvm::StringRef type);
  static std::string neverIs(llvm::StringRef type, llvm::StringRef what, llvm::StringRef name);
  static std::string notObject(llvm::StringRef type);
  static std::string notClass();
  static std::string cantTell(llvm::StringRef generic, llvm::StringRef base, llvm::StringRef hint);
  static std::string extendHint(llvm::StringRef generic);
  static std::string unionHint(llvm::StringRef generic, llvm::StringRef other);
  static std::string neverThat(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverUnion(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverClass(llvm::StringRef base, llvm::StringRef cls);
  static std::string notImpl(llvm::StringRef base, llvm::StringRef cls);
  static std::string neverMade(llvm::StringRef base, llvm::StringRef cls);
};

class ErrorTextFieldCall {
public:
  static std::string undefined();
  static std::string notFunc(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextMethodLookup {
public:
  static std::string isStatic(llvm::StringRef name, llvm::StringRef cls);
  static std::string noProperty(llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextUnsetCall {
public:
  static std::string beforeSet(llvm::StringRef name, llvm::StringRef field);
};

class ErrorTextMethodTypeArgs {
public:
  static std::string notGeneric(llvm::StringRef name);
};

class ErrorTextRuntimeNew {
public:
  static std::string onlyFs(llvm::StringRef name);
};

class ErrorTextClassTypeArgs {
public:
  static std::string notGeneric(llvm::StringRef name);
};

class ErrorTextClassOrder {
public:
  static std::string usedEarly(llvm::StringRef name);
};

class ErrorTextAbstractNew {
public:
  static std::string abstract(llvm::StringRef name);
};

class ErrorTextNoConstructor {
public:
  static std::string noArgs(llvm::StringRef name);
};

class ErrorTextVariant {
public:
  static std::string text(const FunctionDecl *fn);
  static std::string list(llvm::ArrayRef<FunctionDecl *> variants);
  static std::string ctorOf(llvm::StringRef cls);
  static std::string quoted(llvm::StringRef name);
  static std::string spread(llvm::StringRef what);
  static std::string count(llvm::StringRef what, size_t count, llvm::StringRef list);
  static std::string typeParams(llvm::StringRef variant);
  static std::string mismatch(llvm::StringRef variant, llvm::StringRef param, size_t index,
                              llvm::StringRef type);
  static std::string noFit(llvm::StringRef what, llvm::StringRef why, llvm::StringRef hint);
  static std::string generic(llvm::StringRef variant);
  static std::string tie(llvm::StringRef what, llvm::StringRef list, bool literals);
};

class ErrorTextVariantType {
public:
  static std::string several(llvm::StringRef name, llvm::StringRef list, llvm::StringRef type,
                             llvm::StringRef object);
};

class ErrorTextStaticOrder {
public:
  static std::string usedEarly(llvm::StringRef var, llvm::StringRef cls);
  static std::string beforeSet(llvm::StringRef var);
};

class ErrorTextStaticLater {
public:
  static std::string typeOf(llvm::StringRef var, llvm::StringRef field);
};

class ErrorTextStaticMember {
public:
  static std::string instance(llvm::StringRef name, llvm::StringRef cls);
  static std::string noMember(llvm::StringRef cls, llvm::StringRef name);
};

class ErrorTextStaticField {
public:
  static std::string isMethod(llvm::StringRef name, llvm::StringRef cls);
  static std::string noField(llvm::StringRef cls, llvm::StringRef name);
  static std::string readOnly(llvm::StringRef var);
};

class ErrorTextStaticCall {
public:
  static std::string notFunc(llvm::StringRef cls, llvm::StringRef name, llvm::StringRef type);
};

class ErrorTextBaseClass {
public:
  static std::string atRun(llvm::StringRef type);
  static std::string notClass(llvm::StringRef type);
  static std::string javascript(llvm::StringRef className);
  static std::string generic(llvm::StringRef className);
  static std::string madeByFs(llvm::StringRef className);
};

class ErrorTextStaticType {
public:
  static std::string unknown(llvm::StringRef field);
  static std::string fieldLabel(llvm::StringRef name);
};

class ErrorTextOldObject {
public:
  static std::string held(llvm::StringRef className, llvm::StringRef field);
  static std::string decorator();
};

class ErrorTextReplacedField {
public:
  static std::string clash(llvm::StringRef child, llvm::StringRef className, llvm::StringRef field);
};

class ErrorTextOverriddenCall {
public:
  static std::string ctorCall(llvm::StringRef className, llvm::StringRef method,
                              llvm::StringRef child);
  static std::string ctorReach(llvm::StringRef className, llvm::StringRef method,
                               llvm::StringRef child);
  static std::string fieldsCall(llvm::StringRef className, llvm::StringRef method,
                                llvm::StringRef child);
  static std::string fieldsReach(llvm::StringRef className, llvm::StringRef method,
                                 llvm::StringRef child);

private:
  static std::string overridden(llvm::StringRef caller, llvm::StringRef method,
                                llvm::StringRef path, llvm::StringRef child);
};

}
