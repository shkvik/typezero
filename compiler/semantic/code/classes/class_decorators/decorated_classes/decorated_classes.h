#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/classes/class_decorators/decorator_rules/decorator_rules.h"
#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class DecoratedClasses {
public:
  DecoratedClasses(SemanticState &owner, DecoratorRules &sharedDecoratorRules);

  void leaveClassContext();
  void decorateClass(const DecorateClass &task);

private:
  struct DecoratedName {
    llvm::StringRef name;
    llvm::SMLoc loc;
    bool isStatic;
    std::vector<std::string> decorators;
  };
  struct DecoratedMember {
    ClassField *field = nullptr;
    FunctionDecl *method = nullptr;
    bool isStatic = false;
    const char *at = nullptr;
  };

  using ResultHasDecorators = Result<const ClassDecl *, CodeClassesRegistryHasDecoratorsError>;
  using ResultFirstConstructor =
      Result<const ClassDecl *, CodeClassesRegistrySecondConstructorError>;
  using ResultNamesAreDistinct =
      Result<const DecoratedName *, CodeClassesRegistryNamesDistinctError>;

  void startDecorateClass(ClassDecl *classDecl);
  void walkDecoratedNames(ClassDecl *classDecl);
  void walkDecoratedConstructors(const ClassDecl *classDecl);
  void addParamUses(std::vector<DecoratorUse> &uses, const FunctionDecl *functionDecl) const;
  void walkNameClash(llvm::ArrayRef<DecoratedName> members, size_t index,
                     llvm::StringRef className);
  void addDecoratorNames(std::vector<std::string> &names,
                         llvm::ArrayRef<Decorator> decorators) const;
  void addParamUse(std::vector<DecoratorUse> &uses, const FunctionDecl *functionDecl,
                   size_t index) const;


  bool isDecoratedField(const ClassField &field) const;
  bool isDecoratedFunction(const FunctionDecl *functionDecl) const;
  bool isMemberBefore(const DecoratedMember &first, const DecoratedMember &second) const;


  std::string sharedDecorator(const DecoratedName &earlier, const DecoratedName &later) const;
  DecorateRun decorateRun(ClassDecl *classDecl, ClassField *field, FunctionDecl *method,
                          std::vector<DecoratorUse> uses) const;


  ResultNamesAreDistinct checkNamesDistinct(const DecoratedName &earlier,
                                            const DecoratedName &later,
                                            llvm::StringRef className) const;
  ResultFirstConstructor
  checkOneDecoratedConstructor(const ClassDecl *classDecl,
                               llvm::ArrayRef<const FunctionDecl *> decorated) const;


  std::vector<std::string> decoratorNames(llvm::ArrayRef<Decorator> decorators) const;
  std::vector<std::string> methodDecoratorNames(const FunctionDecl *functionDecl) const;


  std::vector<DecoratorUse> classUses(const ClassDecl *classDecl) const;
  std::vector<DecoratorUse> memberUses(const DecoratedMember &member) const;
  std::vector<DecoratorUse> ownUses(llvm::ArrayRef<Decorator> decorators) const;


  ResultHasDecorators hasDecorators(const ClassDecl *classDecl) const;
  std::vector<DecoratedMember> decoratedMembers(ClassDecl *classDecl) const;
  std::vector<DecoratedName> decoratedNames(const ClassDecl *classDecl) const;
  DecoratorPlace placeOfMember(const ClassField *field, const FunctionDecl *method) const;
  llvm::StringRef nameOfMember(const ClassField *field, const FunctionDecl *method) const;

  SemanticState &state;
  DecoratorRules &decoratorRules;
};

}
