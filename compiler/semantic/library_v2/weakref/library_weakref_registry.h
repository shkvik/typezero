#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/weakref/library_weakref_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"

namespace tz {

class LibraryWeakrefRegistry {
public:
  explicit LibraryWeakrefRegistry(SemanticState &owner);

  void checkWeakRefNew(const CheckWeakRefNew &task);
  void finishWeakRefNew(const FinishWeakRefNew &task);
  void afterWeakRefValue(const AfterWeakRefValue &task);
  void afterWeakRefTarget(const AfterWeakRefTarget &task);
  void checkWeakRefMethod(const CheckWeakRefMethod &task);
  void resolveWeakRefType(const ResolveWeakRefType &task);
  void afterWeakRefTypeArg(const AfterWeakRefTypeArg &task);

private:
  using ResultWeakTargetValid = Result<const Type *, LibraryWeakrefRegistryWeakTargetError>;
  using ResultNewTakesOneArgs = Result<const NewExpr *, LibraryWeakrefRegistryNewArgsError>;
  using ResultNewTypeArgsFits = Result<const NewExpr *, LibraryWeakrefRegistryNewTypeArgsError>;
  using ResultTargetIsWritten = Result<const TypeRef *, LibraryWeakrefRegistryTargetWrittenError>;
  using ResultMethodIsDerefed = Result<const MemberExpr *, LibraryWeakrefRegistryMethodNameError>;

  void failWeakRefNew(NewExpr *expr);
  void checkWeakRefValue(NewExpr *expr, const Type *written);


  ResultNewTakesOneArgs checkNewArgs(const NewExpr *expr) const;
  ResultWeakTargetValid checkWeakTarget(const Type *target) const;
  ResultNewTypeArgsFits checkNewTypeArgs(const NewExpr *expr) const;
  ResultTargetIsWritten checkTargetWritten(const TypeRef &ref) const;
  ResultMethodIsDerefed checkMethodName(const MemberExpr *member, const Type *type) const;

  SemanticState &state;
};

}
