#include "compiler/semantic/library/crypto/subtle_algorithms/subtle_algorithms.h"
#include "compiler/program/library/crypto/crypto_api/crypto_api.h"
#include "compiler/program/to_utf8/to_utf8.h"
#include "compiler/semantic/flow/expr_parts/expr_parts.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ConvertUTF.h"
#include <functional>

using llvm::cast;
using llvm::dyn_cast;
using llvm::dyn_cast_or_null;
using llvm::isa;

namespace tz {

std::optional<std::string> SubtleAlgorithms::stringLiteral(const Expr *expr) {
  const Expr *bare = expr != nullptr ? ExprParts::withoutImplicitCasts(expr) : nullptr;
  const auto *literal = dyn_cast_or_null<StringLitExpr>(bare);
  if (literal != nullptr) {
    return ToUtf8::toUtf8(literal->value);
  }
  const auto *templateExpr = dyn_cast_or_null<TemplateExpr>(bare);
  if (templateExpr != nullptr && templateExpr->exprs.empty()) {
    return ToUtf8::toUtf8(templateExpr->quasis[0]);
  }

  return std::nullopt;
}

std::string SubtleAlgorithms::quotedList(llvm::ArrayRef<llvm::StringRef> names,
                                         llvm::StringRef last) {
  auto out = std::string();
  for (size_t index = 0; index < names.size(); ++index) {
    out += (index == 0                  ? "'"
            : index + 1 == names.size() ? "' " + last.str() + " '"
                                        : "', '") +
           names[index].str();
  }

  return out + "'";
}

std::vector<llvm::StringRef> SubtleAlgorithms::usageNames(llvm::ArrayRef<SubtleKeyUsage> usages) {
  auto names = std::vector<llvm::StringRef>();
  for (SubtleKeyUsage usage : usages) {
    names.push_back(SubtleKeyUsageTable::getName(usage));
  }

  return names;
}

std::vector<llvm::StringRef>
SubtleAlgorithms::formatNames(llvm::ArrayRef<SubtleKeyFormat> formats) {
  auto names = std::vector<llvm::StringRef>();
  for (SubtleKeyFormat format : formats) {
    names.push_back(SubtleKeyFormatTable::getName(format));
  }

  return names;
}

Dictionary SubtleAlgorithms::dictionaryOf(SubtleOperation operation,
                                          const SubtleAlgorithmInfo &algorithm) {
  auto operationName = SubtleOperationTable::getName(operation);
  const auto *info = SubtleDictionaryTable::find(operationName, algorithm.name);
  if (info == nullptr) {
    return {"", {}};
  }

  return info->dictionary;
}

}
