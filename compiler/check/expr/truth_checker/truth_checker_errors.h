#pragma once

#include "llvm/ADT/StringRef.h"
#include <string>

namespace tz {

enum class TruthCheckerCheckTruthyErrorCode {
  UNION_ALWAYS_TRUTHY,
  UNION_WITH_NUMBER,
  VOID_VALUE,
  ALWAYS_UNDEFINED,
  PARAM_MAY_BE_NUMBER,
  JSON_VALUE,
  UNKNOWN_VALUE,
  RESULT_VALUE,
  NUMBER_VALUE,
  ALWAYS_TRUTHY,
};

struct TruthCheckerCheckTruthyError {
  TruthCheckerCheckTruthyErrorCode code;
  std::string message;
};

class ErrorTextTruthy {
public:
  static std::string unionTrue(llvm::StringRef type);
  static std::string unionNum(llvm::StringRef type, llvm::StringRef number, llvm::StringRef value);
  static std::string cannotTest(llvm::StringRef op, llvm::StringRef what);
  static std::string typeValue(llvm::StringRef type);
  static std::string voidValue();
  static std::string undefined();
  static std::string mayBeNum(llvm::StringRef base);
  static std::string orDefault(llvm::StringRef x, llvm::StringRef y);
  static std::string orAssign(llvm::StringRef x, llvm::StringRef y);
  static std::string andAssign(llvm::StringRef x, llvm::StringRef y);
  static std::string undefTest(llvm::StringRef x, bool negated);
  static std::string jsonHead(llvm::StringRef op);
  static std::string jsonMay();
  static std::string jsonOr(llvm::StringRef x, llvm::StringRef y);
  static std::string jsonTest(llvm::StringRef x, bool negated);
  static std::string unknownHd(llvm::StringRef op);
  static std::string unknownMay();
  static std::string unknownTst(llvm::StringRef x, bool negated);
  static std::string result(llvm::StringRef x, bool negated);
  static std::string number(bool decimal, bool optional);
  static std::string zero();
  static std::string too();
  static std::string lengthTest(llvm::StringRef x, bool negated);
  static std::string floatTest(llvm::StringRef x, bool negated);
  static std::string zeroTest(llvm::StringRef x, bool negated, bool length);
  static std::string orCompare(llvm::StringRef test, llvm::StringRef x, llvm::StringRef y);
  static std::string orAssignIf(llvm::StringRef x, llvm::StringRef y);
  static std::string andIf(llvm::StringRef test, llvm::StringRef x, llvm::StringRef y);
  static std::string compare(llvm::StringRef test);
  static std::string kind(bool array, bool promise, bool cls, bool object);
  static std::string holds(llvm::StringRef name, llvm::StringRef what);
  static std::string arrayTrue();
  static std::string alwaysTrue(llvm::StringRef what);
  static std::string soAlways();
  static std::string soNot();
  static std::string soOr();
  static std::string soAnd();
  static std::string soOrSet();
  static std::string soAndSet();
  static std::string lengthCmp(llvm::StringRef x, bool negated);
  static std::string callOf(llvm::StringRef x, bool negated, bool noArgs);
  static std::string useAssign();
  static std::string dropOr(llvm::StringRef y);
  static std::string arrayLen(llvm::StringRef x, llvm::StringRef y);
  static std::string dropAssign();
  static std::string dropAnd(llvm::StringRef x);
  static std::string dropOrLen(llvm::StringRef length);
  static std::string dropCheck();
  static std::string checkLen(llvm::StringRef length);
  static std::string allowUndef(llvm::StringRef base);
  static std::string callIt(llvm::StringRef call);
};

enum class TruthCheckerLogicalValueErrorCode {
  RIGHT_SIDE_VOID,
  NO_COMMON_TYPE,
};

struct TruthCheckerLogicalValueError {
  TruthCheckerLogicalValueErrorCode code;
  std::string message;
};

class ErrorTextLogical {
public:
  static std::string rightVoid(llvm::StringRef op, bool isAnd, llvm::StringRef test,
                               llvm::StringRef rhs);
  static std::string noCommon(llvm::StringRef op, llvm::StringRef kept, bool isAnd,
                              llvm::StringRef other);
};

enum class TruthCheckerAssignmentConditionErrorCode {
  COMPARE_INSTEAD,
  TEST_ASSIGNED,
};

struct TruthCheckerAssignmentConditionError {
  TruthCheckerAssignmentConditionErrorCode code;
  std::string message;
};

class ErrorTextAssignTest {
public:
  static std::string compare(llvm::StringRef eq, llvm::StringRef target, llvm::StringRef value);
  static std::string testValue(llvm::StringRef target, llvm::StringRef value, bool negated,
                               bool optional, bool text);
};

}
