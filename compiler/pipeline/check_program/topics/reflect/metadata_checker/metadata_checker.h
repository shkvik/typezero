#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class MetadataChecker {
public:
  explicit MetadataChecker(CheckState &owner);

  const void *metadataKey(const Expr *key, std::string &label);
  const Type *metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                           const Type *expected);
  void findMetadataWriters(SourceFile &program);
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
