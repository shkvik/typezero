#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/objects/code_objects_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>

namespace tz {

class ObjectFields {
public:
  explicit ObjectFields(SemanticState &owner);

  void checkField(const CheckField &task);
  void checkFieldCall(const CheckFieldCall &task);
  void checkEnumObject(const CheckEnumObject &task);
  void afterFieldCallee(const AfterFieldCallee &task);
  void rememberEnumObject(const RememberEnumObject &task);
  void checkObjectFieldTarget(const CheckObjectFieldTarget &task);
  void afterEnumObjectResolved(const AfterEnumObjectResolved &task);

private:
  using ResultLengthIsNotFixed = Result<const Type *, CodeObjectsRegistryFixedLengthError>;
  using ResultTargetFieldFound = Result<const Field *, CodeObjectsRegistryFieldExistsError>;
  using ResultTargetObjectType = Result<const Type *, CodeObjectsRegistryObjectDefinedError>;
  using ResultTargetIsWritable = Result<const Field *, CodeObjectsRegistryFieldWritableError>;

  void startFieldValueCall(CallExpr *call, const Type *type);
  void pushFieldValue(MemberExpr *member, const Field *field);


  bool isInherited(llvm::StringRef name) const;


  ResultLengthIsNotFixed checkFixedLength(const Type *type, const MemberExpr *member) const;
  ResultTargetFieldFound checkFieldExists(const Type *type, const MemberExpr *member) const;
  ResultTargetObjectType checkObjectDefined(const Type *type, const MemberExpr *member) const;
  ResultTargetIsWritable checkFieldWritable(const Field *field, const Type *type,
                                            const MemberExpr *member) const;


  std::string noFieldText(const Type *type, llvm::StringRef name) const;

  SemanticState &state;
};

}
