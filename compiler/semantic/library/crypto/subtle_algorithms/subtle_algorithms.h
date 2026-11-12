#pragma once

#include "compiler/program/library/crypto/subtle_dictionary_table/subtle_dictionary_table.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

namespace tz {

inline const llvm::StringLiteral kUsages[] = {"encrypt",   "decrypt",    "sign",    "verify",
                                              "deriveKey", "deriveBits", "wrapKey", "unwrapKey"};

class SubtleAlgorithms {
public:
  static std::optional<std::string> stringLiteral(const Expr *expr);
  static const std::vector<llvm::StringLiteral> &algorithmsOf(llvm::StringRef op);
  static bool experimentalAlgorithm(llvm::StringRef upper);
  static std::string quotedList(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef last);
  static llvm::StringRef familyOf(llvm::StringRef name);
  static std::vector<llvm::StringLiteral> usagesOf(llvm::StringRef name, bool isPublic);
  static std::vector<llvm::StringLiteral> formatsOf(llvm::StringRef family);
  static Dictionary dictionaryOf(llvm::StringRef op, llvm::StringRef name);
  static std::string verbOf(llvm::StringRef op);
  static std::string exampleOf(llvm::StringRef member);
};

}
