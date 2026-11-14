#pragma once

#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/declaration/files/declaration_files_registry_errors.h"
#include "compiler/semantic/names/scopes/scope_entry.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "llvm/ADT/StringSet.h"

namespace tz {

class DeclarationFilesRegistry {
public:
  explicit DeclarationFilesRegistry(SemanticState &owner);

  void startDeclaredAlias(const StartDeclaredAlias &task);
  void startDeclaredClass(const StartDeclaredClass &task);
  void startDeclaredParam(const StartDeclaredParam &task);
  void finishDeclaredAlias(const FinishDeclaredAlias &task);
  void finishDeclaredClass(const FinishDeclaredClass &task);
  void finishDeclaredField(const FinishDeclaredField &task);
  void finishDeclaredParam(const FinishDeclaredParam &task);
  void startDeclaredMember(const StartDeclaredMember &task);
  void finishDeclaredReturn(const FinishDeclaredReturn &task);
  void startDeclarationFile(const StartDeclarationFile &task);
  void finishDeclarationFile(const FinishDeclarationFile &task);
  void startDeclaredFunction(const StartDeclaredFunction &task);

private:
  struct Exported {
    llvm::StringRef local;
    llvm::SMLoc loc;
  };

  using ResultParamCountMatches = Result<size_t, DeclarationFilesRegistryParamCountError>;
  using ResultAccessWordMatches = Result<Access, DeclarationFilesRegistryAccessAgreesError>;
  using ResultFieldsAreSame = Result<const Type *, DeclarationFilesRegistryCheckAliasError>;
  using ResultPublicOrProtected = Result<Access, DeclarationFilesRegistryMemberVisibleError>;
  using ResultTypeParamsMatched = Result<size_t, DeclarationFilesRegistryTypeParamCountError>;
  using ResultModuleHasTheClass = Result<ClassDecl *, DeclarationFilesRegistryClassFoundError>;
  using ResultReturnTypeMatches = Result<const Type *, DeclarationFilesRegistrySameReturnError>;
  using ResultStoredTypeMatches = Result<const Type *, DeclarationFilesRegistryStoredTypeError>;
  using ResultParamShapeMatches = Result<const Param *, DeclarationFilesRegistryParamShapeError>;
  using ResultModuleExportsName =
      Result<const Exported *, DeclarationFilesRegistryExportFoundError>;
  using ResultReturnTypeWritten =
      Result<const TypeRef *, DeclarationFilesRegistryReturnWrittenError>;
  using ResultExportWasDeclared =
      Result<llvm::StringRef, DeclarationFilesRegistryExportDeclaredError>;
  using ResultMemberWasDeclared =
      Result<const ClassDecl *, DeclarationFilesRegistryMemberListedError>;
  using ResultAliasTypesAreSame =
      Result<const TypeAliasDecl *, DeclarationFilesRegistryCheckAliasError>;
  using ResultOptionalFlagsSame =
      Result<const ClassField *, DeclarationFilesRegistryOptionalAgreesError>;
  using ResultReadonlyFlagsSame =
      Result<const ClassField *, DeclarationFilesRegistryReadonlyAgreesError>;
  using ResultClassHasTheMember =
      Result<const DeclaredMember *, DeclarationFilesRegistryMemberFoundError>;
  using ResultModuleHasFunction =
      Result<const FunctionDecl *, DeclarationFilesRegistryFunctionFoundError>;
  using ResultDeclaredFieldForm =
      Result<const DeclaredField *, DeclarationFilesRegistryDeclaredFieldError>;
  using ResultDeclaredSignature =
      Result<DeclaredSignature *, DeclarationFilesRegistryMethodSignatureError>;
  using ResultAccessWordWritten =
      Result<const DeclaredMember *, DeclarationFilesRegistryAccessWrittenError>;
  using ResultConstructorAccess =
      Result<const FunctionDecl *, DeclarationFilesRegistryConstructorAccessError>;
  using ResultConstructorExists =
      Result<const FunctionDecl *, DeclarationFilesRegistryCallableConstructorError>;

  void pushFileItem(Stmt *item);
  void collectExportedNames(const Module *module);
  void addLocalExports(const ExportDecl &exportDecl);
  void pushReturn(const DeclaredSignatureMatch &match);
  void pushSignature(const DeclaredSignatureMatch &match);
  void pushTypeRef(std::optional<TypeRef> &ref, bool isVoidAllowed);
  void pushParamOrReturn(const DeclaredSignatureMatch &match, size_t index);
  void walkListed(const ClassDecl *classDecl, const llvm::StringSet<> &seen);
  void startDeclaredConstructor(DeclaredMember &member, const ClassDecl *classDecl);
  void walkExportsDeclared(const DeclarationFile &file, const llvm::StringSet<> &declared);
  void walkTypeParamCount(size_t here, size_t there, const std::string &what, llvm::SMLoc loc);
  void startDeclaredField(DeclaredMember &member, const ClassField &field,
                          const ClassDecl *classDecl);
  void walkListedField(const ClassField &field, const ClassDecl *classDecl,
                       const llvm::StringSet<> &seen);
  void walkListedMethod(const FunctionDecl *method, const ClassDecl *classDecl,
                        const llvm::StringSet<> &seen);
  void startDeclaredMethod(DeclaredMember &member, const FunctionDecl *functionDecl,
                           const ClassDecl *classDecl);
  void walkListedConstructor(const FunctionDecl *constructor, const ClassDecl *classDecl,
                             const llvm::StringSet<> &seen);


  bool isExportedLocal(llvm::StringRef name) const;
  bool isHiddenClass(const ClassDecl *classDecl) const;
  bool typesAgree(const Type *here, const Type *there) const;
  bool isMentioningHidden(const FunctionDecl *functionDecl) const;
  bool typesAgreeWidened(const Type *here, const Type *there) const;


  std::string fileName() const;
  std::string memberKey(llvm::StringRef name, bool isStatic) const;


  ResultFieldsAreSame checkSameFields(const Type *here, const Type *there, const std::string &what,
                                      const std::string &file) const;
  ResultFieldsAreSame checkFieldAgrees(const Field &mine, const Type *there,
                                       const std::string &what, const std::string &file) const;
  ResultFieldsAreSame checkFieldPresent(const Field &theirs, const Type *here,
                                        const std::string &what, const std::string &file) const;


  const FunctionDecl *lastCallableConstructor(const ClassDecl *classDecl) const;
  const FunctionDecl *classMethod(const ClassDecl *classDecl, const DeclaredMember &member) const;


  ResultAliasTypesAreSame checkAliasTypes(const TypeAliasDecl *declared,
                                          const TypeAliasDecl *implemented) const;
  ResultAliasTypesAreSame checkAliasFound(const TypeAliasDecl *declared,
                                          const TypeAliasDecl *implemented,
                                          const std::string &file) const;
  ResultAliasTypesAreSame checkAliasParams(const TypeAliasDecl *declared,
                                           const TypeAliasDecl *implemented,
                                           const std::string &file) const;


  const ClassDecl *paramClass(const Param &param) const;
  DeclaredSignature *memberSignature(DeclaredMember &member) const;
  const Type *writtenType(const std::optional<TypeRef> &ref) const;
  ResultModuleExportsName checkExportFound(llvm::StringRef name) const;
  ResultConstructorExists checkCallableConstructor(const ClassDecl *classDecl) const;
  ResultPublicOrProtected checkMemberVisible(Access access, const std::string &what) const;
  const ClassField *classField(const ClassDecl *classDecl, const DeclaredMember &member) const;
  ResultModuleHasFunction checkFunctionFound(const ScopeEntry &entry, llvm::StringRef name) const;
  ResultParamCountMatches checkParamCount(size_t here, size_t there, const std::string &what) const;
  ResultDeclaredSignature checkMethodSignature(DeclaredMember &member,
                                               const std::string &what) const;
  ResultTypeParamsMatched checkTypeParamCount(size_t here, size_t there,
                                              const std::string &what) const;
  ResultModuleHasTheClass checkClassFound(const DeclaredClass &declared,
                                          const Exported &exported) const;
  ResultAccessWordWritten checkAccessWritten(const DeclaredMember &member,
                                             const std::string &what) const;
  ResultDeclaredFieldForm checkDeclaredField(const DeclaredMember &member,
                                             const std::string &what) const;
  ResultReturnTypeMatches checkSameReturn(const Type *here, const Type *there,
                                          const std::string &what) const;
  ResultStoredTypeMatches checkStoredType(const Type *here, const Type *there,
                                          const std::string &what) const;
  ResultReturnTypeWritten checkReturnWritten(const std::optional<TypeRef> &ref,
                                             const std::string &what) const;
  ResultParamShapeMatches checkParamShape(const Param &mine, const Param &theirs,
                                          const std::string &param) const;
  ResultAccessWordMatches checkAccessAgrees(const DeclaredMember &member, Access access,
                                            const std::string &what) const;
  ResultOptionalFlagsSame checkOptionalAgrees(const DeclaredField &declared,
                                              const ClassField &field,
                                              const std::string &what) const;
  ResultReadonlyFlagsSame checkReadonlyAgrees(const DeclaredField &declared,
                                              const ClassField &field,
                                              const std::string &what) const;
  ResultExportWasDeclared checkExportDeclared(llvm::StringRef name,
                                              const llvm::StringSet<> &declared,
                                              llvm::StringRef declarations) const;
  ResultConstructorAccess checkConstructorAccess(const DeclaredMember &member,
                                                 const FunctionDecl *constructor,
                                                 const ClassDecl *classDecl) const;
  ResultMemberWasDeclared checkMemberListed(const llvm::StringSet<> &seen, llvm::StringRef name,
                                            bool isStatic, const ClassDecl *classDecl) const;
  ResultClassHasTheMember checkMemberFound(const DeclaredMember &member, const ClassDecl *classDecl,
                                           const ClassField *field,
                                           const FunctionDecl *method) const;

  SemanticState &state;
  Module *checkedModule = nullptr;
  llvm::StringSet<> declaredNames;
  llvm::StringMap<Exported> exportedNames;
};

}
