#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/semantic/types/type_operators/type_operators_errors.h"

namespace tz {

class SemanticParts;
class EventMaps;
class TypeParams;
class TypeResolver;
class UtilityTypes;
class ClassDeclarer;
class TemplateTypes;
class ArgListChecker;
class ReturnInferrer;
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

  static bool mentionsAnyParam(llvm::ArrayRef<const Type *> operands);
  static const Type *firstError(llvm::ArrayRef<const Type *> operands);
  const Type *evalReadyTypeOp(TypeOp kind, llvm::ArrayRef<const Type *> operands);
  const Type *evalKeyOf(const Type *operand);
  const Type *evalIndexed(const Type *object, const Type *key);
  static const Type *lengthType(const Type *object);
  const Type *evalFiltered(TypeOp kind, const Type *members, const Type *filter);
  static const Type *evalSpread(llvm::ArrayRef<const Type *> operands);
  static const Type *evalFunctionPart(TypeOp kind, const Type *operand);
  const Type *evalMapped(const Type *op, llvm::ArrayRef<const Type *> operands);
  static bool isOpenMapped(const MappedOperands &mapped);
  const Type *addMappedField(const MappedOperands &mapped, const Type *name,
                             std::vector<Field> &fields);
  const Type *renamedFields(const MappedOperands &mapped, const Type *name,
                            std::vector<std::string> &names);
  static void mergeMappedField(std::vector<Field> &fields, const std::string &field,
                               const Type *type, bool readonly);

  struct MappedParts {
    Type *key;
    const Type *value;
    const Type *rename;
  };

  const Type *ownKeys(const Type *type);
  ResultTypeOpResolved resolveKeyOf(const TypeRef &ref);
  ResultTypeOpResolved resolveIndexed(const TypeRef &ref);
  ResultTypeOpResolved resolveTypeOf(const TypeRef &ref);
  const Type *typeOfName(llvm::StringRef name);
  const Type *scopedValueType(llvm::StringRef name);
  const Type *declaredValueType(Stmt *item, llvm::StringRef name);
  const Type *declaredVarType(const VarDeclStmt *decl, llvm::StringRef name);
  const Type *declaredFunctionType(const FunctionDecl *fn, llvm::StringRef name);
  const Type *importedNameType(const std::vector<ImportDecl> &imports, llvm::StringRef name);
  const Type *singleMethodType(const Type *type, llvm::StringRef name);
  ResultTypeOpResolved resolveMapped(const TypeRef &ref);
  const Type *mappedSource(const TypeRef &ref, const Type *keys);
  MappedParts resolveMappedParts(const TypeRef &ref, const Type *keys);
  static const Type *nonStringRename(const Type *rename);
  static const Type *nonStringKey(const Type *keys);
  static std::u16string utf16(llvm::StringRef text);
  static llvm::ArrayRef<const Type *> membersOf(const Type *&type);

  SemanticState &state;
  EventMaps *eventMaps = nullptr;
  TypeParams *typeParams = nullptr;
  TypeResolver *typeResolver = nullptr;
  UtilityTypes *utilityTypes = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  TemplateTypes *templateTypes = nullptr;
  ArgListChecker *argListChecker = nullptr;
  ReturnInferrer *returnInferrer = nullptr;
  ExtendsRelation *extendsRelation = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  ConditionalTypes *conditionalTypes = nullptr;
  ClassValueChecker *classValueChecker = nullptr;
};

}
