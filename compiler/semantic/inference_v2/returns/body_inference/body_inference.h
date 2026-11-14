#pragma once

#include "compiler/semantic/inference_v2/returns/inference_returns_registry_errors.h"
#include "compiler/semantic/inference_v2/returns/return_rules/return_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class BodyInference {
public:
  BodyInference(SemanticState &owner, ReturnRules &sharedReturnRules);

  void inferFromBody(const InferFromBody &task);
  void finishInferFromBody(const FinishInferFromBody &task);

private:
  void convertReturns(const FunctionDecl *functionDecl, const CollectedReturns &collected);


  bool isSettled(const Type *type) const;
  bool isConvertibleYield(const YieldExpr *yield, bool isAsync) const;
  bool isConvertibleReturn(const ReturnStmt *returnStmt, bool isAsync) const;

  SemanticState &state;
  ReturnRules &returnRules;
};

}
