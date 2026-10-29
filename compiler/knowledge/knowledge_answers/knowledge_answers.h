#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "compiler/knowledge/knowledge_target/knowledge_target.h"
#include "compiler/knowledge/project_text/project_text.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <string>
#include <vector>

namespace tz {

class KnowledgeAnswers {
public:
  KnowledgeAnswers(const Knowledge &knowledge, ProjectText &text);

  std::string map(size_t budget, bool library) const;
  llvm::json::Value mapJson(bool library) const;
  std::string context(const std::vector<size_t> &symbols, size_t budget);
  llvm::json::Value contextJson(const std::vector<size_t> &symbols);
  std::string query(llvm::StringRef kind, const KnowledgeTarget &target,
                    const std::vector<size_t> &symbols) const;
  llvm::json::Value queryJson(llvm::StringRef kind, const KnowledgeTarget &target,
                              const std::vector<size_t> &symbols) const;
  std::string impact(const std::vector<size_t> &symbols, size_t budget) const;
  std::string local(size_t local, llvm::StringRef kind) const;
  llvm::json::Value impactJson(const std::vector<size_t> &symbols) const;

private:
  std::string title(size_t symbol) const;
  std::string fromText(size_t from, size_t module) const;
  llvm::json::Object symbolJson(size_t symbol) const;
  llvm::json::Object referenceJson(size_t reference) const;
  std::vector<size_t> callers(size_t symbol) const;
  std::vector<std::vector<size_t>> impactLevels(const std::vector<size_t> &symbols,
                                                std::vector<size_t> &modules) const;
  std::vector<size_t> problemsIn(size_t symbol) const;

  const Knowledge &knowledge_;
  ProjectText &text_;
};

}
