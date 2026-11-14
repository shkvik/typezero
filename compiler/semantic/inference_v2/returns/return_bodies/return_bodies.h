#pragma once

#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class ReturnBodies {
public:
  ReturnBodies(SemanticState &owner, ReturnRules &sharedReturnRules);

  void enterLoadBody();
  void leaveLoadBody();
  void leaveReturnBody();
  void openBodyCheck(const OpenBodyCheck &task);
  void closeBodyCheck(const CloseBodyCheck &task);
  void enterReturnBody(const EnterReturnBody &task);
  void finishTopLevelEntry(const FinishTopLevelEntry &task);

private:
  void walkFreshCopy(const WalkFreshCopy &task);
  void enterBody(const FunctionDecl *functionDecl);


  DiagnosticRoute bodyRoute(bool isClosure, const InstanceRecord *lateOwner) const;

  SemanticState &state;
  ReturnRules &returnRules;
};

}
