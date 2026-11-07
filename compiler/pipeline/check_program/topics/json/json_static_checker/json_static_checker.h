#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"
#include "compiler/pipeline/check_program/topics/json/json_static_checker/json_static_checker_errors.h"
#include "compiler/result/result/result.h"

namespace tz {

class JsonStaticChecker {
public:
  explicit JsonStaticChecker(CheckState &owner);

  Expr *checkJsonStatic(CallExpr *call, MemberExpr *member);
  Expr *checkJsonGlobal(CallExpr *call, NameExpr *name);
  bool readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                    std::string &full);

private:
  using ResultEmbeddedPath = Result<llvm::StringRef, JsonStaticCheckerEmbeddedPathError>;
  using ResultConstantJson = Result<llvm::StringRef, JsonStaticCheckerConstantJsonError>;

  static ResultConstantJson constantJson(llvm::StringRef text);
  static ResultEmbeddedPath embeddedPath(llvm::StringRef fn, llvm::StringRef shown);
  static bool mayHoldItself(const Type *type, std::vector<const Type *> &open);
  static const ClassDecl *classWithToJson(const Type *type,
                                          llvm::SmallPtrSetImpl<const Type *> &seen);
  static bool mayStringifyToUndefined(const Type *type);

  void checkConstantJson(const Expr *arg);

  CheckState &state;
};

}
