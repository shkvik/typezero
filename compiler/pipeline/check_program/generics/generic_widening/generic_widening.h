#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/generics/generic_widening/generic_widening_errors.h"

namespace tz {

class GenericWidening {
public:
  explicit GenericWidening(CheckState &owner);

  static bool widens(const Type *from, const Type *to);
  static bool covariant(const ClassDecl *generic, size_t index);
  static const ClassDecl *instanceOf(const ClassDecl *cls, const ClassDecl *generic);
  static std::string whyNot(const Type *from, const Type *to);
  Expr *joined(Expr *expr);

private:
  static bool mentions(const TypeRef &ref, llvm::StringRef name);
  static std::map<std::pair<const ClassDecl *, size_t>, bool> &knownVariance();
  static std::string writerOf(const ClassDecl *generic, llvm::StringRef name);
  static bool writtenFromOutside(const ClassDecl *generic, llvm::StringRef name);
  static bool passedOnSafely(const ClassDecl *generic, llvm::StringRef name);
  static bool referenceFits(const Type *own, const Type *wide);

  CheckState &state;
};

}
