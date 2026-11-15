#pragma once

#include "compiler/result/result/result.h"
#include "compiler/semantic/library_v2/json/library_json_registry_errors.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include "compiler/union/union/union.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class HeldTypeStepKind { ENTER, LEAVE };

struct EnterHeldType {
  static constexpr HeldTypeStepKind kind = HeldTypeStepKind::ENTER;
  const Type *type;
};

struct LeaveHeldType {
  static constexpr HeldTypeStepKind kind = HeldTypeStepKind::LEAVE;
};

using HeldTypeStep = Union<EnterHeldType, LeaveHeldType>;

class LibraryJsonRegistry {
public:
  explicit LibraryJsonRegistry(SemanticState &owner);

  void afterJsonArrayLit();
  void afterJsonTargetMember();
  void afterJsonKey(const AfterJsonKey &task);
  void checkJsonIndex(const CheckJsonIndex &task);
  void checkJsonGlobal(const CheckJsonGlobal &task);
  void checkJsonMember(const CheckJsonMember &task);
  void checkJsonTarget(const CheckJsonTarget &task);
  void checkJsonMethod(const CheckJsonMethod &task);
  void checkJsonStatic(const CheckJsonStatic &task);
  void finishJsonParse(const FinishJsonParse &task);
  void checkJsonIsJson(const CheckJsonIsJson &task);
  void finishStringify(const FinishStringify &task);
  void checkJsonArrayLit(const CheckJsonArrayLit &task);
  void afterJsonIndexKey(const AfterJsonIndexKey &task);
  void checkNullEquality(const CheckNullEquality &task);
  void afterNullEquality(const AfterNullEquality &task);
  void checkJsonEquality(const CheckJsonEquality &task);
  void checkArrayIsArray(const CheckArrayIsArray &task);
  void nextJsonProperty(const NextJsonProperty &task);
  void checkJsonObjectLit(const CheckJsonObjectLit &task);
  void walkRedundantTests(const WalkRedundantTests &task);
  void afterJsonIsJsonArg(const AfterJsonIsJsonArg &task);
  void finishJsonObjectLit(const FinishJsonObjectLit &task);
  void afterStringifyValue(const AfterStringifyValue &task);
  void afterStringifySpace(const AfterStringifySpace &task);
  void afterJsonTargetIndex(const AfterJsonTargetIndex &task);
  void afterArrayIsArrayArg(const AfterArrayIsArrayArg &task);
  void afterStringifyFailure(const AfterStringifyFailure &task);
  void afterJsonPropertyValue(const AfterJsonPropertyValue &task);
  void afterStringifyReplacer(const AfterStringifyReplacer &task);

private:
  struct JsonTest {
    const Expr *tested;
    bool isNullTest;
  };

  struct TypeOfPair {
    const UnaryExpr *typeOf;
    const StringLitExpr *text;
  };

  struct EmbeddedFile {
    std::string bytes;
    std::string shown;
    std::string full;
  };

  using ResultEmbeddedFile = Result<EmbeddedFile, LibraryJsonRegistryEmbeddedFileError>;
  using ResultTypeOfAgrees = Result<const Expr *, LibraryJsonRegistryTypeOfAgreesError>;
  using ResultNullPossible = Result<const Type *, LibraryJsonRegistryNullPossibleError>;
  using ResultTestsUnknown = Result<const Type *, LibraryJsonRegistryTestsUnknownError>;
  using ResultConstantJson = Result<llvm::StringRef, LibraryJsonRegistryConstantJsonError>;

  void walkNonNullArgs(CallExpr *call);
  void startJsonIndex(IndexExpr *index);
  void walkConstantJson(const Expr *arg);
  void startJsonMember(MemberExpr *member);
  void checkStringifySpace(CallExpr *call, bool isOk);
  void checkJsonParse(CallExpr *call, const MemberExpr *member);
  void checkStringify(CallExpr *call, const MemberExpr *member);
  void reportEmbedFailure(CallExpr *call, const LibraryJsonRegistryEmbeddedFileError &error);


  bool hasToJson(const ClassDecl *classDecl) const;
  bool canHoldItself(const Type *type) const;
  bool isGivenOption(const Expr *option) const;
  bool matchStringifiedValue(const Expr *value);
  bool canStringifyToUndefined(const Type *type) const;
  bool isNullComparison(const BinaryExpr *equality) const;
  bool isSameRef(const Expr *first, const Expr *second) const;
  bool reportRedundantTest(const Expr *term, const JsonTest &test, bool isAnd, const Expr *rhs);
  bool enterHeldType(const Type *type, std::vector<const Type *> &open,
                     std::vector<HeldTypeStep> &steps) const;


  Expr *embedText(CallExpr *call, const EmbeddedFile &embedded);
  Expr *embedBytes(CallExpr *call, const EmbeddedFile &embedded);


  const Expr *bare(const Expr *expr) const;
  const Expr *withoutNot(const Expr *expr) const;


  std::string refText(const Expr *expr) const;
  std::string embeddedFullPath(const CallExpr *call, llvm::StringRef shown) const;


  std::optional<JsonTest> jsonTest(const Expr *rhs) const;
  std::optional<JsonTest> testedSide(const Expr *rhs) const;


  std::vector<const Type *> heldParts(const Type *type) const;
  std::vector<const Type *> fieldTypes(const Type *type) const;
  std::vector<const Type *> familyFields(const ClassDecl *classDecl) const;


  ResultTestsUnknown checkTestsUnknown(const Type *type);
  ResultConstantJson checkConstantJson(llvm::StringRef text) const;
  ResultNullPossible checkNullPossible(const Type *type, llvm::StringRef op) const;
  ResultEmbeddedFile checkEmbeddedFile(const CallExpr *call, llvm::StringRef functionName) const;
  ResultTypeOfAgrees checkTypeOfAgrees(const Expr *tested, llvm::StringRef name, bool nullTest,
                                       bool isAnd) const;


  TypeOfPair typeOfPair(const BinaryExpr *comparison) const;
  const ClassDecl *classWithToJson(const Type *type) const;
  std::optional<std::u16string> constantPath(const Expr *arg) const;
  const StringLitExpr *typeOfTextFor(const Expr *term, const Expr *tested, bool isAnd) const;

  SemanticState &state;
};

}
