#include "compiler/knowledge/knowledge_target/knowledge_target.h"

namespace tz {

KnowledgeTarget::KnowledgeTarget(llvm::StringRef text) : text_(text.str()) {
  size_t at = text.find_insensitive(".tz:");
  if (at == llvm::StringRef::npos) {
    return;
  }
  llvm::StringRef rest = text.drop_front(at + 4);
  auto [line, column] = rest.split(':');
  unsigned number = 0;
  if (line.getAsInteger(10, number) || number == 0) {
    return;
  }
  unsigned col = 0;
  if (!column.empty() && (column.getAsInteger(10, col) || col == 0)) {
    return;
  }
  file_ = text.take_front(at + 3).str();
  line_ = number;
  if (col != 0) {
    column_ = col;
  }
}

std::vector<size_t> KnowledgeTarget::resolve(const Knowledge &knowledge) const {
  if (!isPlace()) {
    return knowledge.named(text_);
  }
  std::optional<size_t> module = knowledge.moduleAt(file_);
  if (!module) {
    return {};
  }
  std::optional<unsigned> character;
  if (column_) {
    character = *column_ - 1;
  }

  return knowledge.at(*module, line_ - 1, character);
}

size_t KnowledgeTarget::localAt(const Knowledge &knowledge) const {
  if (!isPlace() || !column_) {
    return Knowledge::kNone;
  }
  std::optional<size_t> module = knowledge.moduleAt(file_);
  if (!module) {
    return Knowledge::kNone;
  }

  return knowledge.localAt(*module, line_ - 1, *column_ - 1, nullptr);
}

const KnownType *KnowledgeTarget::typeAt(const Knowledge &knowledge) const {
  if (!isPlace() || !column_) {
    return nullptr;
  }
  std::optional<size_t> module = knowledge.moduleAt(file_);
  if (!module) {
    return nullptr;
  }

  return knowledge.typeAt(*module, line_ - 1, *column_ - 1);
}

}
