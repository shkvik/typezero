#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/library/builtin_modules/node_option_table/node_option_table.h"
#include "compiler/program/library/process/process_member_table/process_member_table.h"
#include "compiler/program/types/types.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/common/library_common_registry_errors.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

class SemanticState;

class LibraryCommon {
public:
  using ResultArityMatches = Result<const CallExpr *, LibraryCommonArityError>;
  using ResultSpreadIsFree = Result<const CallExpr *, LibraryCommonSpreadFreeError>;
  using ResultKnownEncoding = Result<int, LibraryCommonKnownEncodingError>;

  explicit LibraryCommon(SemanticState &owner);

  bool hasSpread(const CallExpr *call) const;
  bool isProcessMember(const Expr *expr, ProcessMember which) const;
  ResultSpreadIsFree checkSpreadFree(const CallExpr *call, size_t index) const;
  ResultArityMatches checkArity(const CallExpr *call, unsigned min, unsigned max) const;
  ResultKnownEncoding checkKnownEncoding(llvm::StringRef name) const;
  Expr *findNodeOption(const CallExpr *call, size_t index, NodeOption option) const;
  std::string placeOf(llvm::SMLoc loc) const;
  std::optional<std::vector<std::string>> knownTexts(const Expr *arg) const;
  std::vector<FunctionDecl *> ownMethods(const ClassDecl *classDecl) const;
  FunctionDecl *soleImplementation(const ClassDecl *classDecl, FunctionDecl *variant) const;
  FunctionDecl *freshCopy(const FunctionDecl *functionDecl) const;
  Expr *builtinCall(llvm::SMLoc loc, Expr *callee, Builtin builtin, std::vector<Expr *> args,
                    const Type *type);

private:
  struct OwnedMethod {
    FunctionDecl *method;
    const ClassDecl *owner;
  };

  struct KnownTextLookup {
    std::optional<std::vector<std::string>> texts;
    const ConditionalExpr *choice;
  };

  void copyParams(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void copyMemberRole(const FunctionDecl *functionDecl, FunctionDecl *copy) const;
  void addImplementations(const ClassDecl *child, const FunctionDecl *variant,
                          std::vector<OwnedMethod> &implementations) const;


  bool isOverridingTransitively(const FunctionDecl *functionDecl, const FunctionDecl *base) const;


  const Expr *writtenValue(const Expr *bare) const;
  std::vector<OwnedMethod> implementationsBelow(const ClassDecl *classDecl,
                                                const FunctionDecl *variant) const;
  const VarSymbol *namedVariable(const Expr *expr) const;
  KnownTextLookup lookupTexts(const Expr *arg) const;
  std::optional<std::u16string> writtenText(const Expr *expr) const;
  std::optional<std::vector<std::string>> typeTexts(const Type *type) const;
  std::optional<std::vector<std::string>> typedTexts(const Expr *arg) const;
  std::optional<std::vector<std::string>> narrowedTexts(const Expr *bare) const;

  SemanticState &state;
};

}
