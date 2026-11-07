#pragma once

#include "compiler/check/checker/checker.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"

namespace tz {

class DeclarationChecker {
public:
  explicit DeclarationChecker(Checker &owner);
  void check(Module *module);

private:
  static const char *accessWord(Access access);

  struct Exported {
    llvm::StringRef local;
    llvm::SMLoc loc;
  };
  const Type *resolve(const std::optional<TypeRef> &ref, bool allowVoid);
  bool sameParams(const std::vector<Param> &declared, const FunctionDecl *fn, llvm::SMLoc loc,
                  const std::string &what);
  void checkAlias(TypeAliasDecl *declared, const Exported &exported);
  void checkFunction(FunctionDecl *declared, const Exported &exported);
  void checkClass(const DeclaredClass &declared, const Exported &exported);
  void checkField(const DeclaredMember &member, const ClassField &field, const ClassDecl *cls);
  void checkMethod(const DeclaredMember &member, const FunctionDecl *fn, const ClassDecl *cls);
  bool mentionsHidden(const FunctionDecl *fn) const;
  void mismatch(llvm::SMLoc loc, const std::string &what, const Type *here, const Type *there);
  std::string fileName() const;
  Checker &checker;
  Module *checkedModule = nullptr;
  llvm::StringMap<Exported> exportedNames;
};

}
