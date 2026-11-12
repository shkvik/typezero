#pragma once

#include "compiler/program/library/builtin_names/library_function_table/library_function_table.h"
#include "compiler/program/library/crypto/subtle_algorithm_table/subtle_algorithm_table.h"
#include "compiler/program/library/crypto/subtle_key_format_table/subtle_key_format_table.h"
#include <optional>
#include <vector>

namespace tz {

struct SubtleFamilyInfo {
  SubtleFamily family;
  bool isSecret;
  std::vector<SubtleKeyFormat> formats;
  std::optional<LibraryFunction> generate;
  std::optional<LibraryFunction> import;
  std::optional<LibraryFunction> unwrap;
};

class SubtleFamilyTable {
public:
  static const SubtleFamilyInfo &find(SubtleFamily family);
};

}
