#pragma once

#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/JSON.h"
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tz {

struct KnownSymbol {
  std::string kind;
  std::string name;
  std::string container;
  std::string signature;
  std::string key;
  std::string source;
  size_t module = 0;
  size_t parent = static_cast<size_t>(-1);
  SourcePlace place;
  SourcePlace extent;
  bool exported = false;
};

struct KnownReference {
  size_t symbol = 0;
  size_t from = static_cast<size_t>(-1);
  size_t module = 0;
  SourcePlace place;
  std::string line;
  bool call = false;
};

struct KnownType {
  std::string name;
  std::string type;
  std::string base;
  size_t module = 0;
  SourcePlace place;
};

struct KnownLocal {
  std::string name;
  std::string type;
  size_t module = 0;
  size_t owner = static_cast<size_t>(-1);
  SourcePlace place;
  std::vector<SourcePlace> uses;
};

struct KnownMember {
  std::string name;
  std::string kind;
  std::string detail;
};

struct KnownProblem {
  int severity = 1;
  std::string message;
  size_t module = static_cast<size_t>(-1);
  SourcePlace place;
};

struct KnownModule {
  std::string path;
  std::string file;
  std::string name;
  std::string hash;
  bool library = false;
  bool entry = false;
  unsigned lines = 0;
  std::vector<size_t> imports;
  std::vector<std::string> specifiers;
};

class Knowledge {
public:
  static constexpr size_t kNone = static_cast<size_t>(-1);

  std::string entry;
  std::string root;
  std::vector<KnownModule> modules;
  std::vector<KnownSymbol> symbols;
  std::vector<KnownReference> references;
  std::vector<KnownType> types;
  std::vector<KnownLocal> locals;
  std::map<std::string, std::vector<KnownMember>> typeMembers;
  std::vector<KnownProblem> problems;

  std::string qualified(size_t symbol) const;
  std::string sourceOf(size_t symbol) const;
  std::string where(const SourcePlace &place) const;
  std::string relative(llvm::StringRef path) const;
  bool library(size_t symbol) const { return modules[symbols[symbol].module].library; }

  std::vector<size_t> referencesTo(size_t symbol) const;
  std::vector<size_t> referencesFrom(size_t symbol) const;
  std::vector<size_t> usersOf(size_t symbol) const;
  std::vector<size_t> usesOf(size_t symbol) const;
  std::vector<size_t> members(size_t symbol) const;
  size_t useCount(size_t symbol) const;
  std::vector<size_t> importersOf(size_t module) const;

  std::vector<size_t> named(llvm::StringRef name) const;
  std::optional<size_t> moduleAt(llvm::StringRef path) const;
  std::vector<size_t> at(size_t module, unsigned line, std::optional<unsigned> character) const;
  size_t symbolAt(size_t module, unsigned line, unsigned character,
                  const SourcePlace **place) const;
  size_t localAt(size_t module, unsigned line, unsigned character, const SourcePlace **place) const;
  const KnownType *typeAt(size_t module, unsigned line, unsigned character) const;

  const std::vector<size_t> &symbolsOf(size_t module) const;

private:
  void index() const;

  mutable size_t indexed_ = 0;
  mutable std::vector<std::string> moduleKeys_;
  mutable std::vector<size_t> counts_;
  mutable std::vector<std::vector<size_t>> to_;
  mutable std::vector<std::vector<size_t>> from_;
  mutable std::vector<std::vector<size_t>> children_;
  mutable std::vector<std::vector<size_t>> byModule_;
  mutable std::vector<std::vector<size_t>> importers_;
};

}
