#pragma once

#include "compiler/semantic/generics_v2/classes/generics_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class InstanceMembers {
public:
  explicit InstanceMembers(SemanticState &owner);

  void nextMemberType(const NextMemberType &task);
  void afterMemberType(const AfterMemberType &task);
  void afterInstanceInit(const AfterInstanceInit &task);
  void nextIteratedParam(const NextIteratedParam &task);
  void afterIteratedParam(const AfterIteratedParam &task);
  void nextInstanceMember(const NextInstanceMember &task);
  void afterMemberSignature(const AfterMemberSignature &task);
  void walkInstanceVariants(const WalkInstanceVariants &task);
  void afterInstanceVariants(const AfterInstanceVariants &task);
  void declareInstanceMembers(const DeclareInstanceMembers &task);
  void afterMemberInstanceType(const AfterMemberInstanceType &task);

private:
  void startMemberSignature(InstanceMembersDraft draft);
  void addMemberType(MemberTypesDraft &types, const Type *type, bool isValue) const;


  FuncSymbol *makeInstanceSymbol(FunctionDecl *functionDecl, FunctionDecl *generic);

  SemanticState &state;
};

}
