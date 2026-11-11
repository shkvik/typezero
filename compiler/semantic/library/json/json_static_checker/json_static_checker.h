#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library/json/json_static_checker/json_static_checker_errors.h"
#include "compiler/semantic/semantic_state/semantic_state.h"
#include "compiler/union/union/union.h"

namespace tz {

class SemanticParts;
class Prelude;
class CallChecker;
class ExprChecker;
class LibraryCalls;
class TypeRelations;
class ArgumentChecker;
class BufferStaticChecker;

enum class HoldStepKind { ENTER, LEAVE };

struct EnterHold {
  static constexpr HoldStepKind kind = HoldStepKind::ENTER;
  const Type *type;
};

struct LeaveHold {
  static constexpr HoldStepKind kind = HoldStepKind::LEAVE;
};

using HoldStep = Union<EnterHold, LeaveHold>;

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
  static bool mayHoldItself(const Type *type);
  static bool enterHold(const Type *type, std::vector<const Type *> &open,
                        std::vector<HoldStep> &steps);
  static std::vector<const Type *> heldParts(const Type *type);
  static std::vector<const Type *> familyFields(const ClassDecl *cls);
  static std::vector<const Type *> fieldTypes(const Type *type);
  static const ClassDecl *classWithToJson(const Type *type);
  static bool hasToJson(const ClassDecl *cls);
  static bool mayStringifyToUndefined(const Type *type);

  void checkConstantJson(const Expr *arg);
  void walkArgsLoosely(CallExpr *call);
  Expr *checkJsonParse(CallExpr *call, MemberExpr *member);
  Expr *checkStringify(CallExpr *call, MemberExpr *member);
  bool stringifiedValueFits(const Expr *value);
  bool stringifyReplacerFits(CallExpr *call);
  bool stringifySpaceFits(CallExpr *call, std::vector<Expr *> &args);

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
