#pragma once

#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/types/declaration_types_registry_errors.h"
#include "compiler/semantic/flow/children_first/children_first.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/APInt.h"

namespace tz {

class DeclarationTypesRegistry {
public:
  explicit DeclarationTypesRegistry(SemanticState &owner);

  void enterEnum(const EnterEnum &task);
  void enterAlias(const EnterAlias &task);
  void startAlias(const StartAlias &task);
  void finishAlias(const FinishAlias &task);
  void resolveEnum(const ResolveEnum &task);
  void resolveAlias(const ResolveAlias &task);
  void nextEnumMember(const NextEnumMember &task);
  void resolveTypes(llvm::ArrayRef<Stmt *> declared);


  std::vector<Stmt *> declareModuleTypes(Module *module);
  std::vector<Stmt *> declareTypes(llvm::ArrayRef<Stmt *> stmts);
  std::vector<Stmt *> declareTypeNames(llvm::ArrayRef<Stmt *> stmts);


  const Type *enumType(const EnumDecl *enumDecl) const;

private:
  using ResultMemberIntValue = Result<double, DeclarationTypesRegistryWholeMemberError>;
  using ResultEnumIsResolved = Result<const EnumDecl *, DeclarationTypesRegistryEnumCycleError>;
  using ResultEnumKindIsSame = Result<const EnumDecl *, DeclarationTypesRegistryEnumKindsError>;
  using ResultUnionRecursion = Result<const Type *, DeclarationTypesRegistryRecursiveUnionError>;
  using ResultMemberConstant = Result<ConstantValue, DeclarationTypesRegistryMemberConstantError>;
  using ResultTypeNameIsFree = Result<llvm::StringRef, DeclarationTypesRegistryFreeTypeNameError>;
  using ResultMemberIsUnique =
      Result<const EnumMember *, DeclarationTypesRegistryUniqueMemberError>;
  using ResultAliasNotCyclic =
      Result<const TypeAliasDecl *, DeclarationTypesRegistryAliasCycleError>;
  using ResultMemberHasValue =
      Result<const EnumMember *, DeclarationTypesRegistryCountedMemberError>;
  using ResultAliasIsOwnType =
      Result<const TypeAliasDecl *, DeclarationTypesRegistryBuiltinAliasError>;

  void pushDeclaredType(Stmt *stmt);
  void countMember(EnumFrame &frame);
  void beginAlias(TypeAliasDecl *alias);
  void declareClass(ClassDecl *classDecl);
  void finishEnum(const EnumFrame &frame);
  void enterMember(const EnumFrame &frame);
  void pushAliasBody(TypeAliasDecl *alias);
  void pushDeclaredAlias(TypeAliasDecl *alias);
  void walkResolvingAlias(TypeAliasDecl *alias);
  void reportEnumCycle(const EnumDecl *enumDecl);
  void reportNeededCycles(const EnumFrame &frame);
  void markTextsAssigned(const EnumDecl *enumDecl);
  void nameBuiltinUnion(const TypeAliasDecl *alias);
  void pushAliasBases(const TypeAliasDecl *alias, const Type *type);
  void numberMember(EnumFrame &frame, EnumMember &member, double value);
  void openEnum(EnumDecl *enumDecl, std::optional<Module *> previousModule);
  void openAlias(TypeAliasDecl *alias, std::optional<Module *> previousModule);
  void addNamedType(llvm::StringRef name, Stmt *stmt, std::vector<Stmt *> &declared);


  bool isMentioningTypeOf(const TypeRef &ref) const;


  EnumDecl *enumNamedBy(const Expr *expr) const;
  EnumDecl *unresolvedEnum(const EnumFrame &frame) const;


  const Type *aliasBodyType(TypeAliasDecl *alias);
  const Type *unionAliasType(TypeAliasDecl *alias);
  const Type *objectAliasType(const TypeAliasDecl *alias) const;
  const Type *definedObject(const TypeAliasDecl *alias, std::vector<Field> fields,
                            std::vector<Overload> overloads) const;


  ConstantValue numberValue(double number) const;
  ConstantValue textValue(std::u16string text) const;
  ConstantValue memberValue(const EnumMember &member) const;
  ConstantValue int32Result(BinaryOp op, int32_t left, int32_t right) const;


  std::optional<ConstantValue> numberResult(BinaryOp op, double left, double right) const;
  std::optional<ConstantValue> evalConstant(const Expr *expr, const EnumDecl *self, size_t defined);
  std::optional<ConstantValue> leafConstant(const Expr *expr, const EnumDecl *self,
                                            size_t defined) const;
  std::optional<ConstantValue> unaryConstant(UnaryOp op,
                                             const std::optional<ConstantValue> &operand) const;
  std::optional<ConstantValue> ownMemberConstant(const EnumDecl *self, size_t defined,
                                                 llvm::StringRef name) const;
  std::optional<ConstantValue> joinedText(BinaryOp op, const ConstantValue &left,
                                          const ConstantValue &right) const;
  std::optional<ConstantValue> enumMemberConstant(const MemberExpr *member, const EnumDecl *self,
                                                  size_t defined) const;
  std::optional<ConstantValue> binaryConstant(BinaryOp op, const std::optional<ConstantValue> &left,
                                              const std::optional<ConstantValue> &right) const;
  std::optional<ConstantValue> nodeConstant(const Expr *expr, const EnumDecl *self, size_t defined,
                                            std::vector<std::optional<ConstantValue>> &answers);


  std::optional<llvm::StringRef> declaredTypeName(Stmt *stmt);
  std::optional<llvm::StringRef> ownAliasName(const TypeAliasDecl *alias);


  int32_t jsToInt32(double number) const;
  std::string formatNumber(double value) const;
  double power(double base, double exponent) const;
  Module *elsewhereHome(const Stmt *declared) const;
  Type *declareSelf(const TypeAliasDecl *alias) const;
  llvm::StringRef declareEnumName(EnumDecl *enumDecl);
  TypeAliasDecl *plainAliasNamed(const TypeRef &part) const;
  ResultTypeNameIsFree freeTypeName(llvm::StringRef name) const;
  ResultEnumIsResolved enumCycle(const EnumDecl *enumDecl) const;
  NodeChildren<const Expr *> constantParts(const Expr *expr) const;
  ResultAliasNotCyclic aliasCycle(const TypeAliasDecl *alias) const;
  ResultAliasIsOwnType builtinAlias(const TypeAliasDecl *alias) const;
  std::optional<double> memberNumber(EnumFrame &frame, EnumMember &member);
  std::vector<TypeAliasDecl *> neededAliases(const TypeAliasDecl *alias) const;
  ResultMemberIntValue wholeMember(const EnumMember &member, double value) const;
  ResultMemberConstant memberConstant(std::optional<ConstantValue> constant) const;
  std::vector<EnumDecl *> enumsNeeded(const Expr *init, const EnumDecl *self) const;
  ResultMemberHasValue countedMember(const EnumMember &member, bool isCounting) const;
  ResultEnumKindIsSame enumKinds(const EnumDecl *enumDecl, const EnumCount &count) const;
  ResultUnionRecursion recursiveUnion(const TypeAliasDecl *alias, const Type *type) const;
  ResultMemberIsUnique uniqueMember(const EnumMember &member, const EnumMember &other) const;

  SemanticState &state;
};

}
