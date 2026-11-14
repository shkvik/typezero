#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/code/logic/code_logic_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class LogicRules {
public:
  using ResultTruthIsTested = Result<const Expr *, CodeLogicRegistryCheckTruthyError>;

  explicit LogicRules(SemanticState &owner);

  void pushValueFacts(const Expr *value);


  std::string hintText(const Expr *expr, const char *placeholder) const;
  ResultTruthIsTested checkTruthy(const Expr *expr, TruthUse use, const Expr *other) const;

private:
  struct ObjectTruth {
    const NameExpr *narrowed = nullptr;
    bool isArray = false;
    bool isPromise = false;
    bool isClass = false;
    bool isObject = false;
    bool isClassName = false;
  };

  bool isLength(const Expr *expr) const;
  bool hasFalsyMember(const Type *base) const;
  bool isFalsyMember(const Type *member) const;
  bool isNumberMember(const Type *member) const;
  bool isTestable(const Type *type, const Type *base) const;
  bool areAllBoolOrString(llvm::ArrayRef<const Type *> types) const;


  const Expr *wrappedExpr(const Expr *expr) const;
  const Type *unionNumber(const Type *base) const;
  const Expr *withoutWrappers(const Expr *expr) const;


  std::string sourceText(const Expr *expr) const;
  std::string useConsequence(TruthUse use) const;
  std::string stringSource(const StringLitExpr *literal) const;
  ObjectTruth objectTruth(const Expr *expr, const Type *base) const;
  std::string objectHead(const ObjectTruth &truth, TruthUse use) const;
  std::string objectSubject(const ObjectTruth &truth, llvm::StringRef what) const;
  std::string nodeSource(const Expr *expr, std::vector<std::string> &answers) const;
  std::string callSource(const CallExpr *call, std::vector<std::string> &answers) const;
  std::string indexSource(const IndexExpr *index, std::vector<std::string> &answers) const;
  std::string memberSource(const MemberExpr *member, std::vector<std::string> &answers) const;
  std::string unionNumberMessage(const Type *type, const Type *base, llvm::StringRef value) const;
  std::string jsonMessage(TruthUse use, llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string unknownMessage(TruthUse use, llvm::StringRef valueText,
                             llvm::StringRef otherText) const;
  std::string optionalNumberAdvice(TruthUse use, bool isLength, llvm::StringRef valueText,
                                   llvm::StringRef otherText) const;
  std::string paramMessage(const Type *type, const Type *base, TruthUse use,
                           llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string objectMessage(const Expr *expr, const Type *base, TruthUse use,
                            llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string objectAdvice(const ObjectTruth &truth, const Type *base, TruthUse use,
                           llvm::StringRef valueText, llvm::StringRef otherText) const;
  std::string truthMessage(CodeLogicRegistryCheckTruthyErrorCode code, const Expr *expr,
                           TruthUse use, const Expr *other, const Type *base) const;
  std::string numberMessage(const Expr *expr, const Type *type, const Type *base, TruthUse use,
                            llvm::StringRef valueText, llvm::StringRef otherText) const;


  std::optional<CodeLogicRegistryCheckTruthyErrorCode> truthProblem(const Type *type,
                                                                    const Type *base) const;
  std::optional<CodeLogicRegistryCheckTruthyErrorCode> unionProblem(const Type *type,
                                                                    const Type *base) const;


  llvm::StringRef useOperator(TruthUse use) const;
  const NameExpr *narrowedVariable(const Expr *expr) const;
  std::vector<const Type *> unionMembers(const Type *base) const;
  NodeChildren<const Expr *> sourceParts(const Expr *expr) const;

  SemanticState &state;
};

}
