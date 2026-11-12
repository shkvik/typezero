#pragma once

#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/bodies/declarations/declaration_checker/declaration_checker_errors.h"
#include "compiler/semantic/names/scopes/scope_entry.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class SemanticParts;
class TypeDeclarer;
class TypeResolver;
class ClassDeclarer;
class ModuleDeclarer;
class TopLevelChecker;

class DeclarationChecker {
public:
  DeclarationChecker(SemanticState &owner, SemanticParts &all);
  void walkDeclarationFile(Module *module);

private:
  struct Exported {
    llvm::StringRef local;
    llvm::SMLoc loc;
  };

  using ResultParamCountMatches = Result<size_t, DeclarationCheckerParamCountError>;
  using ResultAccessWordMatches = Result<Access, DeclarationCheckerAccessAgreesError>;
  using ResultPublicOrProtected = Result<Access, DeclarationCheckerMemberVisibleError>;
  using ResultTypeParamsMatched = Result<size_t, DeclarationCheckerTypeParamCountError>;
  using ResultModuleHasTheClass = Result<ClassDecl *, DeclarationCheckerClassFoundError>;
  using ResultStoredTypeMatches = Result<const Type *, DeclarationCheckerStoredTypeError>;
  using ResultReturnTypeMatches = Result<const Type *, DeclarationCheckerSameReturnError>;
  using ResultParamShapeMatches = Result<const Param *, DeclarationCheckerParamShapeError>;
  using ResultModuleExportsName = Result<const Exported *, DeclarationCheckerExportFoundError>;
  using ResultReturnTypeWritten = Result<const TypeRef *, DeclarationCheckerReturnWrittenError>;
  using ResultExportWasDeclared = Result<llvm::StringRef, DeclarationCheckerExportDeclaredError>;
  using ResultMemberWasDeclared = Result<const ClassDecl *, DeclarationCheckerMemberListedError>;
  using ResultAliasTypesAreSame = Result<const TypeAliasDecl *, DeclarationCheckerCheckAliasError>;
  using ResultReadonlyFlagsSame = Result<const ClassField *, DeclarationCheckerReadonlyAgreesError>;
  using ResultOptionalFlagsSame = Result<const ClassField *, DeclarationCheckerOptionalAgreesError>;
  using ResultModuleHasFunction =
      Result<const FunctionDecl *, DeclarationCheckerFunctionFoundError>;
  using ResultClassHasTheMember =
      Result<const DeclaredMember *, DeclarationCheckerMemberFoundError>;
  using ResultDeclaredFieldForm =
      Result<const DeclaredField *, DeclarationCheckerDeclaredFieldError>;
  using ResultAccessWordWritten =
      Result<const DeclaredMember *, DeclarationCheckerAccessWrittenError>;
  using ResultConstructorAccess =
      Result<const FunctionDecl *, DeclarationCheckerConstructorAccessError>;
  using ResultConstructorExists =
      Result<const FunctionDecl *, DeclarationCheckerCallableConstructorError>;
  using ResultDeclaredSignature =
      Result<const DeclaredSignature *, DeclarationCheckerMethodSignatureError>;
  using ResultFieldsAreSame = Result<const Type *, DeclarationCheckerCheckAliasError>;

  const char *accessWord(Access access) const;
  bool typesAgree(const Type *here, const Type *there) const;
  bool typesAgreeWidened(const Type *here, const Type *there) const;
  std::string memberKey(llvm::StringRef name, bool isStatic) const;
  const ClassField *classField(const ClassDecl *classDecl, const DeclaredMember &member) const;
  const FunctionDecl *classMethod(const ClassDecl *classDecl, const DeclaredMember &member) const;
  std::string fileName() const;
  const Type *resolveWritten(const std::optional<TypeRef> &ref);
  const Type *resolveWrittenOrVoid(const std::optional<TypeRef> &ref);
  ResultModuleExportsName checkExportFound(llvm::StringRef name) const;
  ResultExportWasDeclared checkExportDeclared(llvm::StringRef name,
                                              const llvm::StringSet<> &declared,
                                              llvm::StringRef declarations) const;
  void collectExportedNames(const Module *module);
  void addLocalExports(const ExportDecl &exportDecl);
  void walkFileItem(Stmt *item, llvm::StringSet<> &declared);
  void walkFileAlias(TypeAliasDecl *alias, llvm::StringSet<> &declared);
  void walkFileFunction(FunctionDecl *functionDecl, llvm::StringSet<> &declared);
  void walkFileClass(const DeclaredClass &declaredClass, llvm::StringSet<> &declared);
  void walkExportsDeclared(const DeclarationFile &file, const llvm::StringSet<> &declared);
  ResultAliasTypesAreSame checkAlias(TypeAliasDecl *declared, const Exported &exported) const;
  ResultAliasTypesAreSame checkAliasFound(const TypeAliasDecl *declared,
                                          const TypeAliasDecl *implemented,
                                          const std::string &file) const;
  ResultAliasTypesAreSame checkAliasParams(const TypeAliasDecl *declared,
                                           const TypeAliasDecl *implemented,
                                           const std::string &file) const;
  ResultFieldsAreSame checkSameFields(const Type *here, const Type *there, const std::string &what,
                                      const std::string &file) const;
  ResultFieldsAreSame checkFieldAgrees(const Field &mine, const Type *there,
                                       const std::string &what, const std::string &file) const;
  ResultFieldsAreSame checkFieldPresent(const Field &theirs, const Type *here,
                                        const std::string &what, const std::string &file) const;
  ResultParamCountMatches checkParamCount(size_t here, size_t there, const std::string &what) const;
  ResultParamShapeMatches checkParamShape(const Param &mine, const Param &theirs,
                                          const std::string &param) const;
  ResultStoredTypeMatches checkStoredType(const Type *here, const Type *there,
                                          const std::string &what) const;
  bool reportParamsMismatch(const std::vector<Param> &declared, const FunctionDecl *functionDecl,
                            llvm::SMLoc loc, const std::string &what);
  bool reportParamMismatch(const Param &mine, const Param &theirs, const std::string &what);
  ResultModuleHasFunction checkFunctionFound(const ScopeEntry &entry, llvm::StringRef name) const;
  ResultTypeParamsMatched checkTypeParamCount(size_t here, size_t there,
                                              const std::string &what) const;
  ResultReturnTypeWritten checkReturnWritten(const std::optional<TypeRef> &ref,
                                             const std::string &what) const;
  ResultReturnTypeMatches checkSameReturn(const Type *here, const Type *there,
                                          const std::string &what) const;
  void walkDeclaredFunction(FunctionDecl *declared, const Exported &exported);
  void walkTypeParamCount(size_t here, size_t there, const std::string &what, llvm::SMLoc loc);
  bool isMentioningHidden(const FunctionDecl *functionDecl) const;
  const ClassDecl *paramClass(const Param &param) const;
  bool isHiddenClass(const ClassDecl *classDecl) const;
  bool isExportedLocal(llvm::StringRef name) const;
  ResultDeclaredFieldForm checkDeclaredField(const DeclaredMember &member,
                                             const std::string &what) const;
  ResultReadonlyFlagsSame checkReadonlyAgrees(const DeclaredField &declared,
                                              const ClassField &field,
                                              const std::string &what) const;
  ResultOptionalFlagsSame checkOptionalAgrees(const DeclaredField &declared,
                                              const ClassField &field,
                                              const std::string &what) const;
  void walkDeclaredField(const DeclaredMember &member, const ClassField &field,
                         const ClassDecl *classDecl);
  const DeclaredSignature *memberSignature(const DeclaredMember &member) const;
  ResultDeclaredSignature checkMethodSignature(const DeclaredMember &member,
                                               const std::string &what) const;
  void walkDeclaredMethod(const DeclaredMember &member, const FunctionDecl *functionDecl,
                          const ClassDecl *classDecl);
  ResultModuleHasTheClass checkClassFound(const DeclaredClass &declared,
                                          const Exported &exported) const;
  ResultAccessWordWritten checkAccessWritten(const DeclaredMember &member,
                                             const std::string &what) const;
  const FunctionDecl *lastCallableConstructor(const ClassDecl *classDecl) const;
  ResultConstructorExists checkCallableConstructor(const ClassDecl *classDecl) const;
  ResultConstructorAccess checkConstructorAccess(const DeclaredMember &member,
                                                 const FunctionDecl *constructor,
                                                 const ClassDecl *classDecl) const;
  ResultClassHasTheMember checkMemberFound(const DeclaredMember &member, const ClassDecl *classDecl,
                                           const ClassField *field,
                                           const FunctionDecl *method) const;
  ResultPublicOrProtected checkMemberVisible(Access access, const std::string &what) const;
  ResultAccessWordMatches checkAccessAgrees(const DeclaredMember &member, Access access,
                                            const std::string &what) const;
  ResultMemberWasDeclared checkMemberListed(const llvm::StringSet<> &seen, llvm::StringRef name,
                                            bool isStatic, const ClassDecl *classDecl) const;
  void walkDeclaredConstructor(const DeclaredMember &member, const ClassDecl *classDecl);
  void walkDeclaredMember(const DeclaredMember &member, const ClassDecl *classDecl);
  void walkListed(const ClassDecl *classDecl, const llvm::StringSet<> &seen);
  void walkListedField(const ClassField &field, const ClassDecl *classDecl,
                       const llvm::StringSet<> &seen);
  void walkListedMethod(const FunctionDecl *method, const ClassDecl *classDecl,
                        const llvm::StringSet<> &seen);
  void walkListedConstructor(const FunctionDecl *constructor, const ClassDecl *classDecl,
                             const llvm::StringSet<> &seen);
  void walkDeclaredClass(const DeclaredClass &declared, const Exported &exported);

  SemanticState &state;
  TypeDeclarer *typeDeclarer = nullptr;
  TypeResolver *typeResolver = nullptr;
  ClassDeclarer *classDeclarer = nullptr;
  ModuleDeclarer *moduleDeclarer = nullptr;
  TopLevelChecker *topLevelChecker = nullptr;
  Module *checkedModule = nullptr;
  llvm::StringMap<Exported> exportedNames;
};

}
