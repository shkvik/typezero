#pragma once

#include "compiler/knowledge/knowledge/knowledge.h"
#include "compiler/program/source_places/source_places.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace tz {

struct ClassDecl;
struct FunctionDecl;
struct Module;
struct Program;
struct VarSymbol;
class Type;

class KnowledgePass {
public:
  KnowledgePass(const Program &program, llvm::StringRef entry, llvm::StringRef root);

  Knowledge run();

  size_t functionSymbol(const FunctionDecl *fn) const;
  size_t classSymbol(const ClassDecl *cls) const;
  size_t variableSymbol(const VarSymbol *var) const;
  size_t fieldSymbol(const ClassDecl *cls, llvm::StringRef name, bool isStatic) const;
  size_t typeSymbol(size_t module, llvm::StringRef name) const;
  bool reference(size_t symbol, size_t from, size_t module, llvm::SMLoc loc, bool call);
  void typed(size_t module, llvm::SMLoc loc, llvm::StringRef name, const Type *type);
  void local(const VarSymbol *var, size_t module, size_t owner, llvm::SMLoc use);
  const SourcePlaces &places() const { return places_; }

private:
  void addModules();
  void declare(size_t module, const Module &source);
  size_t add(size_t module, std::string kind, llvm::StringRef name, llvm::SMLoc loc,
             const char *begin, const char *end, size_t parent);
  void declareClass(size_t module, const ClassDecl &cls, size_t parent);
  void bindImports(size_t module, const Module &source);
  void walk();
  void addMembers();
  std::string moduleKey(const Module &module) const;

  const Program &program_;
  SourcePlaces places_;
  Knowledge out_;
  llvm::DenseMap<const Module *, size_t> moduleIndex_;
  llvm::DenseMap<const FunctionDecl *, size_t> functions_;
  llvm::DenseMap<const char *, size_t> declaredAt_;
  llvm::DenseMap<const ClassDecl *, size_t> classes_;
  llvm::DenseMap<const VarSymbol *, size_t> variables_;
  std::map<std::tuple<const ClassDecl *, std::string, bool>, size_t> fields_;
  std::vector<llvm::StringMap<size_t>> scopes_;
  llvm::DenseSet<std::pair<size_t, const char *>> seenReferences_;
  llvm::DenseSet<const char *> seenTypes_;
  llvm::DenseMap<const char *, size_t> locals_;
  llvm::DenseSet<const char *> seenLocalUses_;
  std::map<std::string, const Type *> memberTypes_;
  std::set<std::string> keys_;
};

}
