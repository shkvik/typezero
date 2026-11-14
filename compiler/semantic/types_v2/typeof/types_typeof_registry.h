#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/semantic/types_v2/typeof/types_typeof_registry_errors.h"
#include "compiler/union/union/union.h"

namespace tz {

class TypesTypeofRegistry {
public:
  explicit TypesTypeofRegistry(SemanticState &owner);

  void startTypeOf(const StartTypeOf &task);
  void evalFunctionPart(const EvalFunctionPart &task);
  void afterErasedReturn(const AfterErasedReturn &task);
  void afterTypeOfMember(const AfterTypeOfMember &task);
  void continueSignature(const ContinueSignature &task);
  void afterFunctionValue(const AfterFunctionValue &task);
  void signatureForUtility(const SignatureForUtility &task);
  void afterTypeOfCandidate(const AfterTypeOfCandidate &task);
  void findTypeOfCandidates(const FindTypeOfCandidates &task);
  void finishDeclaredFunctionValue(const FinishDeclaredFunctionValue &task);

private:
  void nextTypeOfCandidate(TypeOfDraft draft);
  void pushFunctionValue(FuncSymbol *function);
  void pushErasedSignature(FuncSymbol *function);
  void pushSingleMethod(const TypeOfDraft &draft);
  void reportUnknownTypeOf(const TypeOfDraft &draft);
  void pushCandidateType(const TypeOfCandidate &candidate);
  void nextTypeOfMember(TypeOfDraft draft, const Type *type);
  void pushDeclaredFunction(const FunctionDecl *functionDecl);
  void addScopedCandidates(llvm::StringRef name, std::vector<TypeOfCandidate> &candidates) const;
  void addItemCandidates(Stmt *item, llvm::StringRef name,
                         std::vector<TypeOfCandidate> &candidates) const;
  void addImportedCandidate(Module *from, llvm::StringRef name,
                            std::vector<TypeOfCandidate> &candidates) const;
  void addVarCandidates(const VarDeclStmt *varDecl, llvm::StringRef name,
                        std::vector<TypeOfCandidate> &candidates) const;
  void addImportCandidates(const ImportDecl &importDecl, llvm::StringRef name,
                           std::vector<TypeOfCandidate> &candidates) const;


  bool isDeclaringFunctionType(const FunctionDecl *functionDecl, llvm::StringRef name) const;


  FuncSymbol *singleMethod(const Type *type, llvm::StringRef name) const;
  FuncSymbol *classMethod(const Type *object, llvm::StringRef methodName) const;


  const Type *erasedParam(const Type *param) const;
  const Type *functionTypeWith(const FuncSymbol *function, const Type *returnType) const;


  TypeOfCandidate shapeCandidate(const Expr *init) const;
  TypeOfCandidate knownCandidate(const Type *known) const;
  TypeOfCandidate classCandidate(ClassDecl *classDecl) const;
  TypeOfCandidate resolvedCandidate(const TypeRef *ref) const;
  TypeOfCandidate createdCandidate(const NewExpr *create) const;
  TypeOfCandidate functionCandidate(FuncSymbol *function) const;
  TypeOfCandidate declaredFunctionCandidate(const FunctionDecl *functionDecl) const;


  size_t typedParamCount(const FunctionDecl *functionDecl) const;
  std::vector<TypeOfCandidate> typeOfCandidates(llvm::StringRef name) const;
  const VarDeclarator *declaratorNamed(const VarDeclStmt *varDecl, llvm::StringRef name) const;

  SemanticState &state;
};

}
