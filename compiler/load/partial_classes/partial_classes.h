#pragma once

#include "compiler/load/partial_classes/partial_classes_errors.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include "compiler/program/module/module.h"
#include "compiler/result/result/result.h"
#include <string>
#include <vector>

namespace tz {

struct PartTarget {
  ClassDecl *classDecl = nullptr;
  Module *module = nullptr;
};

class PartialClasses {
public:
  explicit PartialClasses(Diagnostics &owner);

  void joinParts(const std::vector<Module *> &modules);

private:
  using ResultListHasPart = Result<const ImportDecl *, PartialClassesListHasPartError>;
  using ResultClassOfPart = Result<PartTarget, PartialClassesPartClassError>;
  using ResultPartShapeOk = Result<const ClassDecl *, PartialClassesPartShapeError>;
  using ResultMethodIsNew = Result<const FunctionDecl *, PartialClassesMethodIsNewError>;

  void separateParts(Module *module);
  void walkPartLists(Module *module);
  void joinModuleParts(Module *module);
  void walkPartShape(const ClassDecl *part);
  void joinPart(Module *module, ClassDecl *part);
  void joinMethods(Module *module, const ClassDecl *part, const PartTarget &target);
  void joinMethod(Module *module, FunctionDecl *method, ClassDecl *classDecl);
  void addMethod(Module *module, FunctionDecl *method, ClassDecl *classDecl);


  bool isListingPart(const Module *classModule, const Module *module) const;
  bool isPartOf(const ClassDecl *part, const Module *module, const Module *classModule) const;
  bool hasSameTypeParams(const ClassDecl *part, const ClassDecl *classDecl) const;


  ClassDecl *findDeclaredClass(const Module *module, llvm::StringRef name) const;
  const ImportDecl *findImportOf(const Module *module, llvm::StringRef name) const;
  const ModuleName *findImportedName(const ImportDecl &importDecl, llvm::StringRef name) const;
  const FunctionDecl *findMethodNamed(const ClassDecl *classDecl, llvm::StringRef name) const;
  const Module *findMethodFile(const FunctionDecl *method, const Module *classModule) const;


  std::string fileName(const Module *module) const;
  std::string typeParamsText(const ClassDecl *classDecl) const;
  std::string importPath(const Module *from, const Module *to) const;


  ResultPartShapeOk checkNoFields(const ClassDecl *part) const;
  ResultPartShapeOk checkNoHeader(const ClassDecl *part) const;
  ResultPartShapeOk checkNoDecorators(const ClassDecl *part) const;
  ResultPartShapeOk checkNoConstructors(const ClassDecl *part) const;
  ResultClassOfPart findPartClass(const Module *module, const ClassDecl *part) const;
  ResultListHasPart checkListHasPart(const Module *module, const ImportDecl &importDecl) const;
  ResultPartShapeOk checkTypeParams(const ClassDecl *part, const ClassDecl *classDecl) const;
  ResultMethodIsNew checkMethodIsNew(const PartTarget &target, const Module *module,
                                     const FunctionDecl *method) const;

  Diagnostics &diagnostics;
};

}
