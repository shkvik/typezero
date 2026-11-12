#pragma once

#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

class GenericCheckHelpers {
public:
  static bool mentionsAnyOf(const Type *type, llvm::ArrayRef<const Type *> params);
  static llvm::StringRef recordName(const InstanceRecord *record);
  static std::string typeNames(llvm::ArrayRef<const Type *> types, llvm::StringRef separator);
  static std::string bindingNames(llvm::ArrayRef<const Type *> params,
                                  llvm::ArrayRef<const Type *> args);
  static size_t shownArgumentCount(llvm::ArrayRef<const Type *> defaults,
                                   llvm::ArrayRef<const Type *> args);
  static std::string constraintSpelling(const Type *param,
                                        const TypeParamDecl *typeParam = nullptr);

private:
  static bool isUnionOfNames(const TypeRef &ref);
  static std::string refNames(llvm::ArrayRef<TypeRef> refs);
  static bool isParamOf(const Type *type, llvm::ArrayRef<const Type *> params);
  static bool isDefaultArgument(llvm::ArrayRef<const Type *> defaults,
                                llvm::ArrayRef<const Type *> args, size_t index);
};

}
