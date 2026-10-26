#pragma once

#include "compiler/check/checker/checker.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

namespace tz {

std::optional<std::string> stringLiteral(const Expr *e);

const std::vector<llvm::StringLiteral> &algorithmsOf(llvm::StringRef op);

bool experimentalAlgorithm(llvm::StringRef upper);

std::string quotedList(llvm::ArrayRef<llvm::StringLiteral> names, llvm::StringRef last);

llvm::StringRef familyOf(llvm::StringRef name);

std::vector<llvm::StringLiteral> usagesOf(llvm::StringRef name, bool isPublic);

std::vector<llvm::StringLiteral> formatsOf(llvm::StringRef family);

struct Dictionary {
  llvm::StringLiteral name;
  std::vector<llvm::StringLiteral> required;
};

Dictionary dictionaryOf(llvm::StringRef op, llvm::StringRef name);

std::string verbOf(llvm::StringRef op);

std::string exampleOf(llvm::StringRef member);

inline const llvm::StringLiteral kUsages[] = {"encrypt",   "decrypt",    "sign",    "verify",
                                              "deriveKey", "deriveBits", "wrapKey", "unwrapKey"};

std::string literalAlgorithm(Diagnostics &diags, const Expr *arg, llvm::StringRef op,
                             llvm::StringRef fallback, bool &bad);

}
