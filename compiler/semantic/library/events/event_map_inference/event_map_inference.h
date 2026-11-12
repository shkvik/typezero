#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallSet.h"

namespace tz {

class SemanticParts;
class Scopes;
class StmtChecker;
class ReturnInferrer;
class FunctionChecker;

class EventMapInference {
public:
  EventMapInference(SemanticState &owner, SemanticParts &all);

  EventRecord *findEvent(EventSite *site, llvm::StringRef name) const;
  bool isFrozen(const EventRecord *record) const;

  Type *classEventMap(const ClassDecl *classDecl);
  Type *newEventMap(llvm::SMLoc loc);
  std::string placeOf(llvm::SMLoc loc) const;
  std::string shortPlaceOf(llvm::SMLoc loc) const;
  Module *findModuleAt(llvm::SMLoc loc) const;
  EventRecord *addEvent(EventSite *site, llvm::StringRef name);
  void rememberEventArgs(EventSite *site, EventRecord *record, const Type *args, llvm::SMLoc loc);
  EventRecord *completeEvent(EventSite *site, llvm::StringRef name);
  void completeSite(EventSite *site);

private:
  std::string fileOfPlace(const std::string &place) const;
  std::vector<FunctionDecl *> classMethods(const ClassDecl *classDecl) const;
  bool isHoldingClassExpr(const Stmt *where) const;
  bool canTryFunction(const FunctionDecl *functionDecl) const;
  void addCandidates(const Stmt *where, Module *module, FunctionDecl *functionDecl, size_t index,
                     std::vector<EmitCandidate> &found) const;
  void defineMapFields(EventSite *site) const;
  bool isSubclassEmit(const EventSite *site, const EmitCandidate &candidate) const;

  Type *siteMap(const std::string &key, const std::string &name, Module *owner,
                const ClassDecl *classDecl);
  const std::vector<EmitCandidate> &getModuleCandidates(Module *module);
  std::vector<EmitCandidate> getSiteCandidates(const EventSite *site);
  void addModuleCandidates(const EventSite *site, Module *module,
                           std::vector<EmitCandidate> &found);
  std::optional<EmitCandidate> emitCandidate(const Expr *expr) const;
  bool isClassExpr(const Expr *expr) const;
  void tryCandidate(const EmitCandidate &candidate,
                    llvm::SmallPtrSetImpl<const FunctionDecl *> &functions,
                    llvm::SmallSet<size_t, 4> &statements);
  std::vector<EmitCandidate> getLateCandidates();
  void tryCandidates(EventSite *site, EventRecord *record);
  bool canTryStatement(const EmitCandidate &candidate);
  void tryFunction(FunctionDecl *functionDecl);
  void tryStatement(Stmt *statement);

  SemanticState &state;
  Scopes *scopes = nullptr;
  StmtChecker *stmtChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
};

}
