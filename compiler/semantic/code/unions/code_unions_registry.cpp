#include "compiler/semantic/code/unions/code_unions_registry.h"

namespace tz {

CodeUnionsRegistry::CodeUnionsRegistry(SemanticState &owner)
    : unionRules(owner),
      unionMembers(owner, this->unionRules),
      unionMethodCalls(owner, this->unionRules) {}

void CodeUnionsRegistry::checkUnionMember(const CheckUnionMember &task) {
  return this->unionMembers.checkUnionMember(task);
}

void CodeUnionsRegistry::checkUnionFieldTarget(const CheckUnionFieldTarget &task) {
  return this->unionMembers.checkUnionFieldTarget(task);
}

void CodeUnionsRegistry::checkUnionMethodCall(const CheckUnionMethodCall &task) {
  return this->unionMethodCalls.checkUnionMethodCall(task);
}

void CodeUnionsRegistry::resolveUnionMethodCall(const ResolveUnionMethodCall &task) {
  return this->unionMethodCalls.resolveUnionMethodCall(task);
}

void CodeUnionsRegistry::reportUnionMethodCall(const ReportUnionMethodCall &task) {
  return this->unionMethodCalls.reportUnionMethodCall(task);
}

}
