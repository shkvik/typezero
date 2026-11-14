#pragma once

#include "compiler/semantic/generics_v2/copies/generics_copies_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class GenericsCopiesRegistry {
public:
  explicit GenericsCopiesRegistry(SemanticState &owner);

  void pushInstanceTasks();
  void walkInstance(const WalkInstance &task);
  void nextClassBody(const NextClassBody &task);
  void startCopyBody(const StartCopyBody &task);
  void finishCopyWalk(const FinishCopyWalk &task);
  void reportInstance(const ReportInstance &task);
  void startClassBody(const StartClassBody &task);
  void forceConversion(const ForceConversion &task);
  void walkPreciseBody(const WalkPreciseBody &task);
  void classBodyEntered(const ClassBodyEntered &task);
  void reportInstanceAt(const ReportInstanceAt &task);
  void walkClassInstance(const WalkClassInstance &task);
  void popCopyDiagnostics(const PopCopyDiagnostics &task);
  void nextPendingInstance(const NextPendingInstance &task);
  void reportClassInstance(const ReportClassInstance &task);
  void reportFailedPending(const ReportFailedPending &task);
  void walkTemplateInstance(const WalkTemplateInstance &task);
  void popClassBodyDiagnostics(const PopClassBodyDiagnostics &task);

private:
  enum class CopyChain {
    CALL,
    CLASS,
  };

  void noteFailedChain(const InstanceRecord *start, CopyChain chain);
  void startCopyWalk(InstanceRecord *instance, FunctionDecl *copy, CopyWalk walk);
  void finishClassWalk(InstanceRecord *instance, const std::vector<Diagnostic> &diagnostics);
  void finishInstanceWalk(InstanceRecord *instance, const std::vector<Diagnostic> &diagnostics);


  bool isWalkedBody(const FunctionDecl *functionDecl) const;


  std::string failedCallNote(const InstanceRecord *at, const InstanceRecord *next) const;
  std::string failedClassCallNote(const InstanceRecord *at, const InstanceRecord *next) const;


  std::vector<FunctionDecl *> instanceBodies(const ClassDecl *classDecl) const;

  SemanticState &state;
};

}
