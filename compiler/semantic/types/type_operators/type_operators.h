#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_operators/type_operators_errors.h"

namespace tz {

class ClassBases;
class SemanticParts;
class EventMaps;
class TypeParams;
class TypeResolver;
class UtilityTypes;
class ClassDeclarer;
class TemplateTypes;
class ArgListChecker;
class ReturnInferrer;
class GenericChecker;
class ExtendsRelation;
class TopLevelChecker;
class ConditionalTypes;
class ClassValueChecker;

class TypeOperators {
public:
  using ResultTypeOpResolved = Result<const Type *, TypeOperatorsResolveTypeOpError>;

  TypeOperators(SemanticState &owner, SemanticParts &all);

  const Type *keysOf(const Type *type);
  ResultTypeOpResolved resolveTypeOp(const TypeRef &ref);
  const Type *evalTypeOp(const Type *op, llvm::ArrayRef<const Type *> operands);

private:
  struct MappedOperands {
    const Type *key;
    const Type *keys;
    const Type *value;
    const Type *source;
    const Type *rename;
    int optional;
    int readonly;
  };

  struct MappedParts {
    Type *key;
    const Type *value;
    const Type *rename;
  };

  using ResultTypeOpAllowed = Result<void, TypeOperatorsResolveTypeOpError>;

  ResultTypeOpResolved resolveKeyOf(const TypeRef &ref);
  ResultTypeOpResolved resolveTypeOf(const TypeRef &ref);
  ResultTypeOpResolved resolveMapped(const TypeRef &ref);
  ResultTypeOpResolved resolveIndexed(const TypeRef &ref);
  ResultTypeOpAllowed checkKeysAreStrings(const Type *keys) const;
  ResultTypeOpAllowed checkRenameIsString(const Type *rename) const;

  const Type *ownKeys(const Type *type);
  bool isLengthKey(const Type *key) const;
  const Type *evalKeyOf(const Type *operand);
  const Type *typeOfName(llvm::StringRef name);
  const Type *widerKeys(const Type *keys) const;
  const Type *writtenRename(const TypeRef &ref);
  const Type *lengthType(const Type *object) const;
  const Type *nonStringKey(const Type *keys) const;
  const Type *scopedValueType(llvm::StringRef name);
  bool isOpenMapped(const MappedOperands &mapped) const;
  const Type *nonStringRename(const Type *rename) const;
  const Type *evalIndexed(const Type *object, const Type *key);
  const Type *mappedSource(const TypeRef &ref, const Type *keys);
  const Type *declaredValueType(Stmt *item, llvm::StringRef name);
  llvm::ArrayRef<const Type *> membersOf(const Type *&type) const;
  const Type *memberValueType(const Type *type, llvm::StringRef name);
  const Type *firstError(llvm::ArrayRef<const Type *> operands) const;
  const Type *evalSpread(llvm::ArrayRef<const Type *> operands) const;
  const Type *evalFunctionPart(TypeOp kind, const Type *operand) const;
  const Type *singleMethodType(const Type *type, llvm::StringRef name);
  MappedParts resolveMappedParts(const TypeRef &ref, const Type *keys);
  bool isMentioningAnyParam(llvm::ArrayRef<const Type *> operands) const;
  bool isPublicKey(const ClassDecl *classDecl, llvm::StringRef name) const;
  const Type *importedFrom(const ImportDecl &importDecl, llvm::StringRef name);
  const Type *evalMapped(const Type *op, llvm::ArrayRef<const Type *> operands);
  const Type *declaredVarType(const VarDeclStmt *varDecl, llvm::StringRef name);
  const Type *evalFiltered(TypeOp kind, const Type *members, const Type *filter);
  bool isMappedReadonly(const MappedOperands &mapped, llvm::StringRef name) const;
  const Type *evalReadyTypeOp(TypeOp kind, llvm::ArrayRef<const Type *> operands);
  const Type *writtenVarType(const VarDeclarator &declarator, llvm::StringRef name);
  const Type *mappedFieldType(const MappedOperands &mapped, const Type *value) const;
  const Type *declaredFunctionType(const FunctionDecl *functionDecl, llvm::StringRef name);
  const Type *importedNameType(const std::vector<ImportDecl> &imports, llvm::StringRef name);
  const Type *addMappedField(const MappedOperands &mapped, const Type *name,
                             std::vector<Field> &fields);
  const Type *renamedFields(const MappedOperands &mapped, const Type *name,
                            std::vector<std::string> &names);
  std::vector<const Type *> sharedKeys(llvm::ArrayRef<const Type *> common, const Type *keys,
                                       bool isFirst) const;
  void mergeMappedField(std::vector<Field> &fields, const std::string &name, const Type *type,
                        bool isReadonly) const;

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  TypeParams *typeParams = nullptr;
  ClassBases *classBases = nullptr;
  TypeResolver *typeResolver = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  GenericChecker *genericChecker = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
