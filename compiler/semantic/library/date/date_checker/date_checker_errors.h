#pragma once

#include "compiler/program/types/types.h"
#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace tz {

enum class DateCheckerLiteralDateErrorCode {
  UNREADABLE,
  DATE_ONLY,
  NO_SUCH_DAY,
  OUT_OF_RANGE,
};

struct DateCheckerLiteralDateError {
  DateCheckerLiteralDateErrorCode code;
  std::string message;
};

class ErrorTextDateDay {
public:
  static std::string month(int64_t month);
  static std::string missing(std::optional<int64_t> year, int64_t month, int64_t day,
                             int64_t toMonth, int64_t toDay);
};

class ErrorTextDateLiteral {
public:
  static std::string unreadable(llvm::StringRef shown);
  static std::string dateOnly(llvm::StringRef shown);
  static std::string noDay(llvm::StringRef shown, llvm::StringRef missing);
  static std::string outside(llvm::StringRef shown);
};

class ErrorTextDateFields {
public:
  static std::string timeField(size_t index);
  static std::string shortYear(int64_t year);
  static std::string month12();
  static std::string month(int64_t month);
  static std::string dayBelow(int64_t day);
  static std::string february(llvm::StringRef missing, llvm::StringRef setter,
                              std::optional<int64_t> year);
  static std::string monthDays(llvm::StringRef missing, int64_t month, int64_t last);
  static std::string lastDay(llvm::StringRef missing, int64_t month,
                             std::optional<int64_t> nextYear, int64_t next);
  static std::string dayAbove(int64_t day);
  static std::string range(int64_t value, llvm::StringRef what, int64_t max);
};

class ErrorTextDateNew {
public:
  static std::string arity(size_t count);
  static std::string unknown();
  static std::string call();
};

class ErrorTextDateStatic {
public:
  static std::string onlyCalled(llvm::StringRef name);
  static std::string noMember(llvm::StringRef name);
};

class ErrorTextDateMethod {
public:
  static std::string getYear();
  static std::string setYear();
  static std::string gmt();
  static std::string noMember(llvm::StringRef name);
  static std::string onlyCalled(llvm::StringRef name);
  static std::string getYearVal();
};

class ErrorTextDateOperator {
public:
  static std::string plus();
  static std::string minus();
  static std::string compare(llvm::StringRef op);
};

class ErrorTextDateValue {
public:
  static std::string takes(const Type *type);
};

}
