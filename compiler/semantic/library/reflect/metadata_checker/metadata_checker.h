#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Scopes;
class ClassDeclarer;
class TypeRelations;
class ReturnInferrer;
class FunctionChecker;
class ClassValueChecker;

class MetadataChecker {
public:
  using ResultMetadataTyped = Result<const Type *, MetadataCheckerMetadataTypeError>;

  MetadataChecker(SemanticState &owner, SemanticParts &all);

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
  static const Type *designPlain(const Type *type);
  ClassDecl *plainConstructor(const Type *type);
  SemanticState &state;
  Scopes *scopes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  FunctionChecker *functionChecker = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
