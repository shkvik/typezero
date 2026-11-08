#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/conditional_types/conditional_types_errors.h"

namespace tz {

class ConditionalTypes {
public:
  static bool mentionsParamBesides(const Type *type, llvm::ArrayRef<const Type *> allowed);

  using ResultIntersectionJoined = Result<const Type *, ConditionalTypesResolveIntersectionError>;

  explicit ConditionalTypes(SemanticState &owner);

  const Type *resolveConditional(const TypeRef &ref);
  const Type *deferConditional(const TypeRef &ref);
  const Type *evalConditional(llvm::ArrayRef<const Type *> operands);
  std::vector<const Type *> conditionalBranches(const Type *conditional);
  bool fitsConditional(const Type *from, const Type *to, unsigned depth = 0);
  void bindType(llvm::StringRef name, const Type *type, llvm::SMLoc loc);
  ResultIntersectionJoined resolveIntersection(const TypeRef &ref);
  const Type *resolveTypeArgument(const TypeRef &ref);
  const Type *signatureForUtility(const TypeRef &ref);

private:
  static std::vector<const Type *> distributed(const Type *type);
  static void collectNames(const TypeRef &ref, std::vector<llvm::StringRef> &names);
  static void collectInfers(const TypeRef &ref, std::vector<const TypeRef *> &infers,
                            std::vector<bool> &lists, bool spread = false);
  static bool mentionsOtherParams(const Type *type, llvm::ArrayRef<const Type *> allowed,
                                  llvm::SmallPtrSetImpl<const Type *> &seen);
  static void collectParams(const Type *type, std::vector<const Type *> &params,
                            llvm::SmallPtrSetImpl<const Type *> &seen);
  static bool isBinding(const Stmt *decl);
  SemanticState &state;
};

}
