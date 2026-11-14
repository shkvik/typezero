#pragma once

#include "compiler/semantic/code/unions/union_members/union_members.h"
#include "compiler/semantic/code/unions/union_method_calls/union_method_calls.h"
#include "compiler/semantic/code/unions/union_rules/union_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class CodeUnionsRegistry {
public:
  explicit CodeUnionsRegistry(SemanticState &owner);

  void checkUnionMember(const CheckUnionMember &task);
  void checkUnionMethodCall(const CheckUnionMethodCall &task);
  void checkUnionFieldTarget(const CheckUnionFieldTarget &task);
  void reportUnionMethodCall(const ReportUnionMethodCall &task);
  void resolveUnionMethodCall(const ResolveUnionMethodCall &task);

private:
  UnionRules unionRules;
  UnionMembers unionMembers;
  UnionMethodCalls unionMethodCalls;
};

}
