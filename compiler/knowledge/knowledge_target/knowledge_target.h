#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "llvm/ADT/StringRef.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class KnowledgeTarget {
public:
  explicit KnowledgeTarget(llvm::StringRef text);

  std::vector<size_t> resolve(const Knowledge &knowledge) const;
  const KnownType *typeAt(const Knowledge &knowledge) const;
  size_t localAt(const Knowledge &knowledge) const;

  const std::string &text() const { return text_; }
  const std::string &file() const { return file_; }
  bool isPlace() const { return !file_.empty(); }
  bool hasColumn() const { return column_.has_value(); }

private:
  std::string text_;
  std::string file_;
  unsigned line_ = 0;
  std::optional<unsigned> column_;
};

}
