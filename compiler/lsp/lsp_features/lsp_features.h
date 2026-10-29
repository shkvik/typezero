#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <string>

namespace tz {

class LspFeatures {
public:
  using UriOf = llvm::function_ref<std::string(llvm::StringRef path)>;
  using TextOf = llvm::function_ref<std::string(llvm::StringRef path)>;

  LspFeatures(const Knowledge &knowledge, size_t module, llvm::StringRef text, UriOf uriOf,
              TextOf textOf);

  llvm::json::Value hover(unsigned line, unsigned character) const;
  llvm::json::Value definition(unsigned line, unsigned character) const;
  llvm::json::Value references(unsigned line, unsigned character, bool declaration) const;
  llvm::json::Value completion(unsigned line, unsigned character) const;
  llvm::json::Value prepareRename(unsigned line, unsigned character) const;
  llvm::json::Value rename(unsigned line, unsigned character, llvm::StringRef name,
                           std::string &error) const;

  static llvm::json::Object range(const SourcePlace &place);
  static bool isName(llvm::StringRef name);

private:
  llvm::json::Object location(const SourcePlace &place) const;
  std::string uriOfPlace(const SourcePlace &place) const;
  std::vector<SourcePlace> occurrences(unsigned line, unsigned character, bool declaration,
                                       std::string &name, bool &library) const;
  size_t offset(unsigned line, unsigned character) const;

  const Knowledge &knowledge_;
  size_t module_;
  llvm::StringRef text_;
  UriOf uriOf_;
  TextOf textOf_;
};

}
