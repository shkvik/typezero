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

  static const char *accessWord(Access access);
  static bool typesAgree(const Type *here, const Type *there);
  static std::string memberKey(llvm::StringRef name, bool isStatic);
  static bool typesAgreeWidened(const Type *here, const Type *there);
  static const ClassField *classField(const ClassDecl *cls, const DeclaredMember &member);
  static const FunctionDecl *classMethod(const ClassDecl *cls, const DeclaredMember &member);
  static ResultAccessWordWritten accessWritten(const DeclaredMember &member,
                                               const std::string &what);
  static ResultReturnTypeWritten returnWritten(const std::optional<TypeRef> &ref,
                                               const std::string &what);
  static ResultExportWasDeclared exportDeclared(llvm::StringRef name,
                                                const llvm::StringSet<> &declared,
                                                llvm::StringRef declarations);

  std::string fileName() const;
  bool mentionsHidden(const FunctionDecl *fn) const;
  ResultModuleExportsName exportFound(llvm::StringRef name) const;
  ResultConstructorExists callableConstructor(const ClassDecl *cls) const;
  ResultPublicOrProtected memberVisible(Access access, const std::string &what) const;
  ResultModuleHasFunction functionFound(const ScopeEntry &entry, llvm::StringRef name) const;
  ResultAliasTypesAreSame checkAlias(TypeAliasDecl *declared, const Exported &exported) const;
  ResultParamCountMatches paramCount(size_t here, size_t there, const std::string &what) const;
  ResultTypeParamsMatched typeParamCount(size_t here, size_t there, const std::string &what) const;
  ResultModuleHasTheClass classFound(const DeclaredClass &declared, const Exported &exported) const;
  ResultDeclaredFieldForm declaredField(const DeclaredMember &member,
                                        const std::string &what) const;
  ResultDeclaredSignature methodSignature(const DeclaredMember &member,
                                          const std::string &what) const;
  ResultStoredTypeMatches storedType(const Type *here, const Type *there,
                                     const std::string &what) const;
  ResultReturnTypeMatches sameReturn(const Type *here, const Type *there,
                                     const std::string &what) const;
  ResultParamShapeMatches paramShape(const Param &mine, const Param &theirs,
                                     const std::string &param) const;
  ResultAccessWordMatches accessAgrees(const DeclaredMember &member, Access access,
                                       const std::string &what) const;
  ResultReadonlyFlagsSame readonlyAgrees(const DeclaredField &declared, const ClassField &field,
                                         const std::string &what) const;
  ResultOptionalFlagsSame optionalAgrees(const DeclaredField &declared, const ClassField &field,
                                         const std::string &what) const;
  ResultMemberWasDeclared memberListed(const llvm::StringSet<> &seen, llvm::StringRef name,
                                       bool isStatic, const ClassDecl *cls) const;
  ResultConstructorAccess constructorAccess(const DeclaredMember &member,
                                            const FunctionDecl *constructor,
                                            const ClassDecl *cls) const;
  ResultClassHasTheMember memberFound(const DeclaredMember &member, const ClassDecl *cls,
                                      const ClassField *field, const FunctionDecl *method) const;

  void walkDeclaredMember(const DeclaredMember &member, const ClassDecl *cls);
  void walkDeclaredFunction(FunctionDecl *declared, const Exported &exported);
  void walkListed(const ClassDecl *cls, const llvm::StringSet<> &seen);
  const Type *resolve(const std::optional<TypeRef> &ref, bool allowVoid);
  void walkDeclaredClass(const DeclaredClass &declared, const Exported &exported);
  void walkDeclaredConstructor(const DeclaredMember &member, const ClassDecl *cls);
  void walkDeclaredField(const DeclaredMember &member, const ClassField &field,
                         const ClassDecl *cls);
  void walkDeclaredMethod(const DeclaredMember &member, const FunctionDecl *fn,
                          const ClassDecl *cls);
  bool sameParams(const std::vector<Param> &declared, const FunctionDecl *fn, llvm::SMLoc loc,
                  const std::string &what);

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
