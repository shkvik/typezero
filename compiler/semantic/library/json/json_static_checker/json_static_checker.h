#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/json/json_static_checker/json_static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class LibraryCalls;
class TypeRelations;
class ArgumentChecker;
class BufferStaticChecker;

class JsonStaticChecker {
public:
  JsonStaticChecker(SemanticState &owner, SemanticParts &all);

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

  SemanticState &state;
  Prelude *prelude = nullptr;
  CallChecker *callChecker = nullptr;
  ExprChecker *exprChecker = nullptr;
  LibraryCalls *libraryCalls = nullptr;
  TypeRelations *typeRelations = nullptr;
  ArgumentChecker *argumentChecker = nullptr;
  BufferStaticChecker *bufferStaticChecker = nullptr;
};

}
