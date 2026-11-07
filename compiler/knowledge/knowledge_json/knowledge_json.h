#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "llvm/Support/JSON.h"
#include <set>
#include <string>

namespace tz {

class KnowledgeJson {
public:
  static llvm::json::Value write(const Knowledge &knowledge, bool libraryBodies,
                                 const std::set<std::string> *detailed = nullptr);
  static bool read(const llvm::json::Value &value, Knowledge &out);

  static llvm::json::Object range(const SourcePlace &place);
  static void readRange(const llvm::json::Object *range, SourcePlace &place);

private:
  static llvm::json::Value keyOf(const Knowledge &knowledge, size_t symbol);
  static std::string text(const llvm::json::Object &object, llvm::StringRef key);
};

}
