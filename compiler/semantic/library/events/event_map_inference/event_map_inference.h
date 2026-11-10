#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class StmtChecker;
class ReturnInferrer;
class FunctionChecker;

class EventMapInference {
public:
  EventMapInference(SemanticState &owner, SemanticParts &all);

  static EventRecord *findEvent(EventSite *site, llvm::StringRef name);
  static bool isFrozen(const EventRecord *record);

  Type *classEventMap(const ClassDecl *cls);
  Type *newEventMap(llvm::SMLoc loc);
  std::string placeOf(llvm::SMLoc loc) const;
  std::string shortPlaceOf(llvm::SMLoc loc) const;
  Module *findModuleAt(llvm::SMLoc loc) const;
  EventRecord *addEvent(EventSite *site, llvm::StringRef name);
  void rememberEventArgs(EventSite *site, EventRecord *record, const Type *args, llvm::SMLoc loc);
  EventRecord *completeEvent(EventSite *site, llvm::StringRef name);
  void completeSite(EventSite *site);

private:
  static std::string fileOfPlace(const std::string &place);
  static std::vector<FunctionDecl *> classMethods(const ClassDecl *cls);
  static bool holdsClassExpr(const Stmt *where);
  static bool canTryFunction(const FunctionDecl *fn);
  static void addCandidates(const Stmt *where, Module *module, FunctionDecl *fn, size_t index,
                            std::vector<EmitCandidate> &found);
  static void defineMapFields(EventSite *site);
  static bool isSubclassEmit(const EventSite *site, const EmitCandidate &candidate);

  Type *siteMap(const std::string &key, const std::string &name, Module *owner,
                const ClassDecl *cls);
  const std::vector<EmitCandidate> &getModuleCandidates(Module *module);
  std::vector<EmitCandidate> getSiteCandidates(const EventSite *site);
  std::vector<EmitCandidate> getLateCandidates();
  void tryCandidates(EventSite *site, EventRecord *record);
  bool canTryStatement(const EmitCandidate &candidate);
  void tryFunction(FunctionDecl *fn);
  void tryStatement(Stmt *statement);

  SemanticState &state;
  Scopes *scopes = nullptr;
  StmtChecker *stmtChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
};

}
