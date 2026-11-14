#pragma once

#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/result/result/result.h"
#include "compiler/semantic/code/dictionaries/code_dictionaries_registry_errors.h"
#include "compiler/semantic/code/dictionaries/dictionary_rules/dictionary_rules.h"
#include "compiler/semantic/semantic_state/v2/semantic_state.h"
#include <string>
#include <vector>

namespace tz {

class DictionaryConstruction {
public:
  DictionaryConstruction(SemanticState &owner, DictionaryRules &sharedDictionaryRules);

  void checkDictNew(const CheckDictNew &task);
  void resolveDictType(const ResolveDictType &task);
  void finishDictTypeAnswer(const FinishDictTypeAnswer &task);
  void afterDictNewTypeArgs(const AfterDictNewTypeArgs &task);

private:
  using ResultTypesStated = Result<const Type *, CodeDictionariesRegistryTypesGivenError>;
  using ResultOneArgument = Result<const NewExpr *, CodeDictionariesRegistryArgumentCountError>;
  using ResultDictTypeResolved = Result<const Type *, CodeDictionariesRegistryResolveDictTypeError>;

  void markPairs(Expr *arg);
  void startDictNew(NewExpr *expr, const Type *type, bool isMap);
  void startDictSource(NewExpr *expr, const Type *type, bool isMap);
  void startEmptyDictNew(NewExpr *expr, const Type *type, bool isMap);
  void pushTypeArguments(const std::vector<TypeRef> &args, Collection collection);
  void pushBlockStatements(const BlockStmt *block, std::vector<const Stmt *> &stack) const;


  bool isLiteralKeys(const Type *key) const;
  bool isPairReturn(const ReturnStmt *returnStmt) const;
  bool isPairElement(const ArrayLitExpr::Element &element) const;
  bool isKeyArgument(CodeDictionariesRegistryResolveDictTypeErrorCode code) const;


  const Type *wantedDict(const Type *expected, bool isMap) const;
  const Type *onlyDictMember(const Type *expected, bool isMap) const;
  const Type *literalKeysObject(const Type *key, const Type *value) const;


  ResultOneArgument checkArgumentCount(const NewExpr *expr, bool isMap) const;
  ResultTypesStated checkTypesGiven(const NewExpr *expr, const Type *type, bool isMap,
                                    bool isUndefinedArg) const;


  ResultDictTypeShape checkDictTypeShape(const TypeRef &ref) const;
  ResultDictTypeShape checkTypeArgumentCount(llvm::StringRef name, size_t count) const;


  ResultDictTypeResolved checkRecordType(const Type *key, const Type *value) const;
  ResultDictTypeResolved checkCollectionType(Collection collection, const Type *key,
                                             const Type *value);


  std::vector<const ReturnStmt *> branchReturns(const Stmt *body) const;
  std::string typeArgumentsText(llvm::StringRef name, Collection collection) const;

  SemanticState &state;
  DictionaryRules &dictionaryRules;
};

}
