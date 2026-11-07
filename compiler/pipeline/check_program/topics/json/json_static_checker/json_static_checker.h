#pragma once

#include "compiler/pipeline/check_program/check_state/check_state.h"

namespace tz {

class JsonStaticChecker {
public:
  explicit JsonStaticChecker(CheckState &owner);

  Expr *checkJsonStatic(CallExpr *call, MemberExpr *member);
  Expr *checkJsonGlobal(CallExpr *call, NameExpr *name);
  bool readEmbedded(CallExpr *call, llvm::StringRef fn, std::string &bytes, std::string &shown,
                    std::string &full);

private:
  static std::string toUtf8(const std::u16string &text);
  static bool mayHoldItself(const Type *type, std::vector<const Type *> &open);
  static const ClassDecl *classWithToJson(const Type *type,
                                          llvm::SmallPtrSetImpl<const Type *> &seen);
  static bool mayStringifyToUndefined(const Type *type);

  void checkConstantJson(const Expr *arg);

  CheckState &state;
};

}
