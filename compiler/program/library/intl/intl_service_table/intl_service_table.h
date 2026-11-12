#pragma once

#include "compiler/program/library/intl/intl_option_key_table/intl_option_key_table.h"
#include "compiler/program/types/intl_kind/intl_kind.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class IntlOptionKind {
  TEXT,
  NUMBER,
  FLAG,
  GROUPING,
};

struct IntlOption {
  IntlOptionKey key;
  IntlOptionKind kind;
  std::vector<llvm::StringLiteral> values;
};

struct IntlService {
  IntlKind kind;
  llvm::StringLiteral name;
  int runtimeKind;
  std::vector<IntlOption> options;
  bool callable;
};

struct IntlDigitRange {
  IntlOptionKey key;
  int64_t low;
  int64_t high;
};

class IntlServiceTable {
public:
  static const std::vector<IntlService> &getServices();
  static const IntlService *find(llvm::StringRef name);
  static const IntlService *findKind(IntlKind kind);
  static const IntlService &getSupportedLocalesService();
  static const IntlService *findByOptionsTypeName(llvm::StringRef name);
  static const IntlService *findByResolvedTypeName(llvm::StringRef name);
  static std::string getOptionsTypeName(llvm::StringRef serviceName);
  static std::string getQualifiedName(llvm::StringRef name);
  static std::optional<IntlDigitRange> findDigitRange(IntlOptionKey key);
  static llvm::ArrayRef<int64_t> getRoundingIncrements();
};

}
