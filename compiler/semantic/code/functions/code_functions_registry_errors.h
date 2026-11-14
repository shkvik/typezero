#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class CodeFunctionsRegistrySuperCalledErrorCode {
  NO_SUPER_CALL,
};

struct CodeFunctionsRegistrySuperCalledError {
  CodeFunctionsRegistrySuperCalledErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryDefaultOrderErrorCode {
  FORWARD_REFERENCE,
};

struct CodeFunctionsRegistryDefaultOrderError {
  CodeFunctionsRegistryDefaultOrderErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryReturnsEverywhereErrorCode {
  MISSING_RETURN,
};

struct CodeFunctionsRegistryReturnsEverywhereError {
  CodeFunctionsRegistryReturnsEverywhereErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryNotGenericErrorCode {
  GENERIC_FUNCTION_EXPRESSION,
};

struct CodeFunctionsRegistryNotGenericError {
  CodeFunctionsRegistryNotGenericErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryParamAgreesErrorCode {
  DECLARED_DIFFERENTLY,
};

struct CodeFunctionsRegistryParamAgreesError {
  CodeFunctionsRegistryParamAgreesErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryDefaultMattersErrorCode {
  DEFAULT_NEVER_USED,
};

struct CodeFunctionsRegistryDefaultMattersError {
  CodeFunctionsRegistryDefaultMattersErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryClosureParamTypeKnownErrorCode {
  NEEDS_TYPE,
};

struct CodeFunctionsRegistryClosureParamTypeKnownError {
  CodeFunctionsRegistryClosureParamTypeKnownErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryClosureThisParamErrorCode {
  ARROW_THIS_PARAMETER,
};

struct CodeFunctionsRegistryClosureThisParamError {
  CodeFunctionsRegistryClosureThisParamErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryRestParamErrorCode {
  REST_NOT_ARRAY,
};

struct CodeFunctionsRegistryRestParamError {
  CodeFunctionsRegistryRestParamErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryParamOrderErrorCode {
  REQUIRED_AFTER_OPTIONAL,
};

struct CodeFunctionsRegistryParamOrderError {
  CodeFunctionsRegistryParamOrderErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryPromiseAwaitedErrorCode {
  PROMISE_DROPPED,
};

struct CodeFunctionsRegistryPromiseAwaitedError {
  CodeFunctionsRegistryPromiseAwaitedErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryFunctionGivenErrorCode {
  NOT_A_FUNCTION,
};

struct CodeFunctionsRegistryFunctionGivenError {
  CodeFunctionsRegistryFunctionGivenErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryFixedParamsErrorCode {
  REST_PARAMETER,
};

struct CodeFunctionsRegistryFixedParamsError {
  CodeFunctionsRegistryFixedParamsErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryCallbackParamErrorCode {
  PARAM_TYPE,
  EXTRA_OPTIONAL,
};

struct CodeFunctionsRegistryCallbackParamError {
  CodeFunctionsRegistryCallbackParamErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryArgumentsEnoughErrorCode {
  TOO_FEW_PASSED,
};

struct CodeFunctionsRegistryArgumentsEnoughError {
  CodeFunctionsRegistryArgumentsEnoughErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryReturnMatchesErrorCode {
  WRONG_RETURN,
};

struct CodeFunctionsRegistryReturnMatchesError {
  CodeFunctionsRegistryReturnMatchesErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryResultKeptErrorCode {
  RESULT_DROPPED,
  PROMISE_DROPPED,
};

struct CodeFunctionsRegistryResultKeptError {
  CodeFunctionsRegistryResultKeptErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryCallbackGivenErrorCode {
  NO_FUNCTION,
};

struct CodeFunctionsRegistryCallbackGivenError {
  CodeFunctionsRegistryCallbackGivenErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryExtraArgumentsErrorCode {
  THIS_ARGUMENT,
  TOO_MANY_ARGUMENTS,
};

struct CodeFunctionsRegistryExtraArgumentsError {
  CodeFunctionsRegistryExtraArgumentsErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryInitialValueErrorCode {
  UNTYPED_INITIAL,
};

struct CodeFunctionsRegistryInitialValueError {
  CodeFunctionsRegistryInitialValueErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryMappedValueErrorCode {
  NO_VALUE,
};

struct CodeFunctionsRegistryMappedValueError {
  CodeFunctionsRegistryMappedValueErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryComparisonErrorCode {
  NOT_A_NUMBER,
};

struct CodeFunctionsRegistryComparisonError {
  CodeFunctionsRegistryComparisonErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryUseOrderErrorCode {
  USED_BEFORE_DECLARED,
};

struct CodeFunctionsRegistryUseOrderError {
  CodeFunctionsRegistryUseOrderErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryCheckThisCallErrorCode {
  NO_BIND,
  NO_SUCH_PROPERTY,
  NO_THIS_ARGUMENT,
  APPLY_ARGUMENTS,
};

struct CodeFunctionsRegistryCheckThisCallError {
  CodeFunctionsRegistryCheckThisCallErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryCheckListMethodErrorCode {
  NO_SUCH_METHOD,
};

struct CodeFunctionsRegistryCheckListMethodError {
  CodeFunctionsRegistryCheckListMethodErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistrySpreadFreeErrorCode {
  SPREAD_ARGUMENT,
};

struct CodeFunctionsRegistrySpreadFreeError {
  CodeFunctionsRegistrySpreadFreeErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryJoinArityErrorCode {
  WRONG_COUNT_TAGGED,
  WRONG_COUNT,
};

struct CodeFunctionsRegistryJoinArityError {
  CodeFunctionsRegistryJoinArityErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryFunctionValueExtraArgumentsErrorCode {
  MORE_ARGUMENTS,
};

struct CodeFunctionsRegistryFunctionValueExtraArgumentsError {
  CodeFunctionsRegistryFunctionValueExtraArgumentsErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryAsyncCallbackErrorCode {
  REPLACE_GETS_PROMISE,
  TEST_GETS_PROMISE,
  VALUE_GETS_PROMISE,
};

struct CodeFunctionsRegistryAsyncCallbackError {
  CodeFunctionsRegistryAsyncCallbackErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryRestListValueErrorCode {
  REST_LIST_AS_VALUE,
};

struct CodeFunctionsRegistryRestListValueError {
  CodeFunctionsRegistryRestListValueErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryListArgumentErrorCode {
  TOO_MANY_ARGUMENTS,
  SPREAD_MISMATCH,
  LIST_NOT_SPREAD,
};

struct CodeFunctionsRegistryListArgumentError {
  CodeFunctionsRegistryListArgumentErrorCode code;
  std::string message;
};

enum class CodeFunctionsRegistryListFilledErrorCode {
  MISSING_ARGUMENTS,
};

struct CodeFunctionsRegistryListFilledError {
  CodeFunctionsRegistryListFilledErrorCode code;
  std::string message;
};

class ErrorTextFunctionBody {
public:
  static std::string noSuper(llvm::StringRef cls, llvm::StringRef parent);
  static std::string forward(llvm::StringRef label, llvm::StringRef used);
  static std::string noReturn(llvm::StringRef label, llvm::StringRef type);
};

class ErrorTextFunctionName {
public:
  static std::string quoted(llvm::StringRef name);
  static std::string label(llvm::StringRef name);
};

class ErrorTextNotGeneric {
public:
  static std::string generic();
};

class ErrorTextParamAgrees {
public:
  static std::string declared(llvm::StringRef label, llvm::StringRef passed, llvm::StringRef given);
};

class ErrorTextDefaultMatters {
public:
  static std::string unused(llvm::StringRef label);
};

class ErrorTextParamTypeKnown {
public:
  static std::string needsType(llvm::StringRef label);
};

class ErrorTextThisParam {
public:
  static std::string arrowThis();
};

class ErrorTextRestParam {
public:
  static std::string notArray(llvm::StringRef name);
};

class ErrorTextParamOrder {
public:
  static std::string order();
};

class ErrorTextPromiseAwaited {
public:
  static std::string dropped();
};

class ErrorTextCallbackArg {
public:
  static std::string notFunc(llvm::StringRef method, llvm::StringRef type);
  static std::string rest(llvm::StringRef name);
  static std::string paramType(llvm::StringRef name, llvm::StringRef param, llvm::StringRef ordinal,
                               llvm::StringRef method, llvm::StringRef given);
  static std::string extraParam(llvm::StringRef name, llvm::StringRef argName,
                                llvm::StringRef paramName, llvm::StringRef method,
                                llvm::StringRef args);
  static std::string tooFew(llvm::StringRef method, llvm::StringRef passes, llvm::StringRef name,
                            llvm::StringRef needs);
  static std::string badReturn(llvm::StringRef method, llvm::StringRef wanted, llvm::StringRef name,
                               llvm::StringRef returns);
  static std::string noCheck(llvm::StringRef name, llvm::StringRef method);
  static std::string noWait(llvm::StringRef name, llvm::StringRef method);
  static std::string argsShape(llvm::StringRef method);
  static std::string position(size_t index);
  static std::string argName(llvm::StringRef method, size_t index);
  static std::string paramName(llvm::StringRef name);
};

class ErrorTextCallbackMethod {
public:
  static std::string noFunction(llvm::StringRef name);
  static std::string thisArg(llvm::StringRef name);
  static std::string untyped(llvm::StringRef name);
  static std::string noValue(llvm::StringRef name);
  static std::string notNumber(llvm::StringRef type);
};

class ErrorTextUseOrder {
public:
  static std::string early(llvm::StringRef function, llvm::StringRef variable);
  static std::string declared(llvm::StringRef variable);
};

class ErrorTextThisCall {
public:
  static std::string noBind();
  static std::string noProperty(llvm::StringRef name);
  static std::string thisFirst(llvm::StringRef name);
  static std::string applyArgs();
};

class ErrorTextListMethod {
public:
  static std::string noMethod(llvm::StringRef name);
};

class ErrorTextExtraArguments {
public:
  static std::string passed(llvm::StringRef function, llvm::StringRef argument,
                            llvm::StringRef ordinal, llvm::StringRef method,
                            llvm::StringRef passes);
};

class ErrorTextAsyncCallback {
public:
  static std::string replace(llvm::StringRef method, llvm::StringRef type);
  static std::string test(llvm::StringRef method, llvm::StringRef type);
  static std::string value(llvm::StringRef method, llvm::StringRef type);
};

class ErrorTextRestList {
public:
  static std::string spreadOnly(llvm::StringRef list, llvm::StringRef type);
};

class ErrorTextListArgument {
public:
  static std::string tooMany(llvm::StringRef list);
  static std::string spreadWrong(llvm::StringRef spread, llvm::StringRef list, llvm::StringRef part,
                                 bool partIsList);
  static std::string notSpread(llvm::StringRef part);
};

class ErrorTextListFilled {
public:
  static std::string missing(llvm::StringRef list, llvm::StringRef part, bool partIsList);
};

}
