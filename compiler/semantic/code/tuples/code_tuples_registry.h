#pragma once

#include "compiler/semantic/code/tuples/code_tuples_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeTuplesRegistry {
public:
  explicit CodeTuplesRegistry(SemanticState &owner);

  void finishTupleJoin(const FinishTupleJoin &task);
  void reportTupleJoin(const ReportTupleJoin &task);
  void checkTupleMember(const CheckTupleMember &task);
  void checkTupleMethod(const CheckTupleMethod &task);
  void finishTupleMethod(const FinishTupleMethod &task);

private:
  void startTupleJoin(CallExpr *call, MemberExpr *member);


  bool isJoinableElement(const Type *element) const;


  const Type *tupleElement(const Type *tuple) const;
  std::string tupleText(const MemberExpr *member) const;

  SemanticState &state;
};

}
