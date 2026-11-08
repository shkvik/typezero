#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/reflect/metadata_checker/metadata_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class MetadataChecker {
public:
  using ResultMetadataTyped = Result<const Type *, MetadataCheckerMetadataTypeError>;

  explicit MetadataChecker(CheckState &owner);

  const void *metadataKey(const Expr *key, std::string &label);
  ResultMetadataTyped metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                                   const Type *expected);
  void checkBodyEarly(FunctionDecl *fn);
  Expr *designClass(const Type *type, llvm::SMLoc loc);
  ClassDecl *designConstructor(const Type *type);
  ClassDecl *builtinConstructor(llvm::StringRef name);
  void writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                          const FunctionDecl *ctor, llvm::SMLoc loc);
  void writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value, llvm::StringRef member,
                         llvm::SMLoc loc);

private:
  CheckState &state;
};

}
