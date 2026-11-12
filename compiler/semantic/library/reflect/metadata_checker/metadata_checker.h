#pragma once

#include "compiler/program/library/reflect/builtin_constructor_table/builtin_constructor_table.h"
#include "compiler/program/library/reflect/design_key_table/design_key_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library/reflect/metadata_checker/metadata_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

struct ScopeEntry;
class SemanticParts;
class Scopes;
class ClassDeclarer;
class TypeRelations;
class ReturnInferrer;
class ClassValueChecker;

class MetadataChecker {
public:
  struct NamedKey {
    const void *key;
    std::string label;
  };

  using ResultMetadataTyped = Result<const Type *, MetadataCheckerMetadataTypeError>;

  MetadataChecker(SemanticState &owner, SemanticParts &all);

  NamedKey metadataKey(const Expr *key) const;
  ClassDecl *builtinConstructorNamed(llvm::StringRef name);
  ResultMetadataTyped checkMetadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                                        const Type *expected);
  void writeDesignRecords(ClassDecl *classDecl, ClassField *field, FunctionDecl *method,
                          const FunctionDecl *constructor, llvm::SMLoc loc);

private:
  Expr *withoutClassRef(Expr *value);
  void walkBodyEarly(FunctionDecl *functionDecl);
  ClassDecl *plainConstructor(const Type *type);
  ClassDecl *designConstructor(const Type *type);
  const Type *designPlain(const Type *type) const;
  Expr *designClass(const Type *type, llvm::SMLoc loc);
  const void *entryKey(const ScopeEntry &entry) const;
  ClassDecl *builtinConstructor(BuiltinConstructor which);
  Expr *memberProperty(llvm::StringRef member, llvm::SMLoc loc);
  Expr *paramClasses(const FunctionDecl *functionDecl, llvm::SMLoc loc);
  std::optional<BuiltinConstructor> builtinDesign(const Type *type) const;
  const MetadataType *recordedType(const void *key, llvm::StringRef label);
  void writeFieldRecord(ClassDecl *classDecl, const ClassField *field, llvm::SMLoc loc);
  void writeMethodRecords(ClassDecl *classDecl, const FunctionDecl *method, llvm::SMLoc loc);
  void writeDesignRecord(ClassDecl *classDecl, DesignKey key, Expr *value, llvm::StringRef member,
                         llvm::SMLoc loc);

  SemanticState &state;
  Scopes *scopes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TypeRelations *typeRelations = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
