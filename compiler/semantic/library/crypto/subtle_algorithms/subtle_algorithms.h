#pragma once

#include "compiler/program/library/crypto/subtle_algorithm_table/subtle_algorithm_table.h"
#include "compiler/program/library/crypto/subtle_dictionary_table/subtle_dictionary_table.h"
#include "compiler/program/library/crypto/subtle_key_format_table/subtle_key_format_table.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

namespace tz {

class SubtleAlgorithms {
public:
  static std::optional<std::string> stringLiteral(const Expr *expr);
  static std::string quotedList(llvm::ArrayRef<llvm::StringRef> names, llvm::StringRef last);
  static std::vector<llvm::StringRef> usageNames(llvm::ArrayRef<SubtleKeyUsage> usages);
  static std::vector<llvm::StringRef> formatNames(llvm::ArrayRef<SubtleKeyFormat> formats);
  static Dictionary dictionaryOf(SubtleOperation operation, const SubtleAlgorithmInfo &algorithm);
};

}
