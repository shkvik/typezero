#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/types/types.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class CheckTaskKind {
  USE_MODULE,
  DECLARE_CLASS_TYPE_PARAMS,
  BIND_CLASS_TYPE_PARAMS,
  DECLARE_FUNCTION_TYPE_PARAMS,
  BIND_FUNCTION_TYPE_PARAMS,
  INSTANTIATE_OWN_ALIAS,
  SUBSTITUTE_INSTANCE_FIELDS,
  INSTANTIATE_EMITTER_PARENT,
  MARK_OVERRIDDEN,
  FIND_STREAM_HOOK_PARAMS,
  WALK_HOOK_PARAMS,
  WALK_ITERABLE_PARAM,
  WALK_GENERATOR_RETURN,
  WALK_ASYNC_RETURN,
  WALK_STMT,
  REPORT_UNLISTENED_ERRORS,
  ENTER_LOAD_BODY,
  LEAVE_LOAD_BODY,
  IMPORT_BUILTIN_MODULE,
  INSTANTIATE_CLASS_TYPE,
  INSTANTIATE_ALIAS,
  SUBSTITUTE_TYPE,
  FIND_INTL_TYPE,
  RESOLVE_RESULT_TYPE,
  RESOLVE_ITERATOR_TYPE,
  RESOLVE_DICT_TYPE,
  RESOLVE_PROMISE_TYPE,
  MAKE_METHOD_DESCRIPTOR,
  RESOLVE_CONSTRUCTOR_TYPE,
  CHECK_CONSTRUCTOR_ARGS,
  CLASS_VALUE_TYPE,
  LITERAL_SHAPE,
  CREATED_TYPE,
  INFER_RETURN_TYPE,
  RETURN_TYPE_OF,
  RESOLVE_TYPE_REF,
  RESOLVE_INNER_TYPE_REF,
  RESOLVE_OBJECT_FIELDS,
  WRITE_RESOLVED_TYPE,
  RESOLVE_TYPE_ANSWER,
  FINISH_TYPE_USE,
  FINISH_ARRAY_TYPE,
  NEXT_UNION_MEMBER,
  AFTER_UNION_MEMBER,
  FINISH_ALIAS_TYPE,
  FINISH_ENUM_TYPE,
  FINISH_ENUM_MEMBER,
  FINISH_INTL_TYPE,
  NEXT_TYPE_ARGUMENT,
  AFTER_TYPE_ARGUMENT,
  FINISH_DESCRIPTOR,
  FINISH_INSTANCE_TYPE,
  NEXT_UTILITY_OPERAND,
  AFTER_UTILITY_OPERAND,
  AFTER_UTILITY_SIGNATURE,
  NEXT_FUNCTION_PARAM,
  AFTER_FUNCTION_PARAM,
  FINISH_FUNCTION_TYPE,
  FINISH_ASSERTS_TYPE,
  FINISH_TUPLE_TYPE,
  NEXT_OBJECT_FIELD,
  AFTER_OBJECT_FIELD,
  AFTER_OBJECT_SIGNATURE,
  EVAL_TYPE_OP,
  EVAL_KEY_OF,
  EVAL_INDEXED,
  EVAL_SPREAD,
  START_KEY_OF,
  AFTER_KEY_OF_OPERAND,
  AFTER_KEY_OF_EVAL,
  START_INDEXED,
  AFTER_INDEXED_OPERANDS,
  AFTER_INDEXED_EVAL,
  EVAL_MAPPED,
  NEXT_MAPPED_KEY,
  AFTER_MAPPED_VALUE,
  AFTER_MAPPED_RENAME,
  START_MAPPED,
  AFTER_MAPPED_KEYS,
  AFTER_MAPPED_SOURCE,
  AFTER_MAPPED_PARTS,
  START_INTERSECTION,
  NEXT_INTERSECTION_PART,
  AFTER_INTERSECTION_PART,
  START_CONDITIONAL,
  AFTER_CONDITIONAL_CHECK,
  AFTER_INFER_CONSTRAINT,
  AFTER_CONDITIONAL_PATTERN,
  CHOOSE_BRANCH,
  CLOSE_CONDITIONAL_SCOPE,
  AFTER_DISTRIBUTED_MEMBER,
  EVAL_CONDITIONAL,
  AFTER_EVAL_CONDITIONAL,
  EVAL_FILTERED,
  FINISH_FILTERED,
  PREPARE_EXPECTATION,
  FINISH_BRANCHES,
  START_TEMPLATE,
  AFTER_TEMPLATE_HOLE,
  FINISH_TEMPLATE,
  EVAL_TEMPLATE,
  EVAL_STRING_CASE,
  EVAL_FUNCTION_PART,
  START_TYPE_OF,
  FIND_TYPE_OF_CANDIDATES,
  AFTER_TYPE_OF_CANDIDATE,
  AFTER_TYPE_OF_MEMBER,
  AFTER_FUNCTION_VALUE,
  FINISH_DECLARED_FUNCTION_VALUE,
  SIGNATURE_FOR_UTILITY,
  CONTINUE_SIGNATURE,
  AFTER_ERASED_RETURN,
  RESOLVE_IMPORT,
  START_TOP_LEVEL,
  NEXT_TOP_LEVEL_ITEM,
  FINISH_TOP_LEVEL,
  CLOSE_TOP_LEVEL,
  START_LOAD_EARLY,
  LOAD_EARLY_BODY,
  FINISH_LOAD_EARLY,
  BIND_LOADED_NAMES,
  RESOLVE_ALIAS,
  START_ALIAS,
  ENTER_ALIAS,
  FINISH_ALIAS,
  RESOLVE_ENUM,
  ENTER_ENUM,
  NEXT_ENUM_MEMBER,
  START_FUNCTION_SYMBOL,
  FINISH_FUNCTION_SYMBOL,
  START_FUNCTION_PARAMS,
  START_FUNCTION_PARAM,
  FINISH_FUNCTION_PARAM,
  START_FUNCTION_RETURN,
  FINISH_FUNCTION_RETURN,
  START_RETURN_TYPE,
  FINISH_ASSERTS_RETURN,
  FINISH_PREDICATE_RETURN,
  RESOLVE_CLASS,
  OPEN_CLASS_SCOPE,
  CLOSE_CLASS_SCOPE,
  RESOLVE_CLASS_MEMBERS,
  START_CLASS_FIELD,
  FINISH_CLASS_FIELD,
  START_CLASS_BASES,
  FINISH_CLASS_BASES,
  FINISH_PARENT_CLASS,
  FINISH_LINK_PARENT,
  FINISH_IMPLEMENTED,
  ADD_BASE,
  DECLARE_METHODS,
  FINISH_CONSTRUCTOR,
  FINISH_CLASS_METHOD,
  START_FIELD_METHOD,
  FINISH_FIELD_METHOD,
  START_TYPED_METHOD,
  FINISH_TYPED_PARAM,
  FINISH_TYPED_METHOD,
  FINISH_METHODS,
  NEXT_FIELD_DEFAULT,
  CONTINUE_FIELD_DEFAULT,
  MAKE_FIELD_INIT,
  FINISH_FIELD_INIT,
  FINISH_VARIANTS,
  FINISH_ITEM_METHODS,
  COMPLETE_CLASS,
  FINISH_COMPLETION,
  START_INTERFACE_FIELDS,
  START_INTERFACE_BASE,
  FINISH_INTERFACE_BASE,
  FINISH_INTERFACE_FIELDS,
  REPORT_VARIANT_PAIR,
  START_LINK_CLASSES,
  FINISH_LINK_CLASSES,
  START_LINK_OVERRIDES,
  FINISH_LINK_OVERRIDES,
  LINK_OVERRIDE,
  START_DECLARATION_FILE,
  FINISH_DECLARATION_FILE,
  START_DECLARED_ALIAS,
  FINISH_DECLARED_ALIAS,
  START_DECLARED_FUNCTION,
  START_DECLARED_CLASS,
  FINISH_DECLARED_CLASS,
  START_DECLARED_MEMBER,
  FINISH_DECLARED_FIELD,
  START_DECLARED_PARAM,
  FINISH_DECLARED_PARAM,
  FINISH_DECLARED_RETURN,
};

struct DeclaredSignatureMatch {
  std::vector<Param> *declared;
  const FunctionDecl *implemented;
  FuncSymbol *returned;
  std::optional<TypeRef> *returnType;
  llvm::SMLoc loc;
  std::string what;
  bool isConstructor;
};

struct FunctionSymbolDraft {
  FunctionDecl *functionDecl;
  FuncSymbol *function;
  VarSymbol *variable;
  bool isNamed;
  bool isTypeScoped;
};

struct EnumCount {
  double next;
  bool isCounting;
  bool hasNumbers;
  bool hasStrings;
};

struct EnumFrame {
  EnumDecl *enumDecl;
  std::optional<Module *> previousModule;
  size_t nextMember;
  EnumCount count;
};

struct InterfaceDraft {
  TypeAliasDecl *alias;
  std::vector<Field> fields;
  std::vector<std::string> origins;
  std::vector<Overload> overloads;
  std::vector<const Type *> bases;
};

enum class DefaultStepKind {
  VALUE,
  ENTER_EMPTY,
  RESOLVE_FAMILY,
  LEAVE_EMPTY,
  MAKE_NEW,
  FILL_OBJECT
};

struct DefaultFor {
  static constexpr DefaultStepKind kind = DefaultStepKind::VALUE;
  const Type *type;
  const TypeRef *ref;
};

struct EnterEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::ENTER_EMPTY;
  ClassDecl *classDecl;
};

struct ResolveFamily {
  static constexpr DefaultStepKind kind = DefaultStepKind::RESOLVE_FAMILY;
  ClassDecl *classDecl;
};

struct LeaveEmpty {
  static constexpr DefaultStepKind kind = DefaultStepKind::LEAVE_EMPTY;
  ClassDecl *classDecl;
};

struct MakeNew {
  static constexpr DefaultStepKind kind = DefaultStepKind::MAKE_NEW;
  ClassDecl *classDecl;
};

struct FillObject {
  static constexpr DefaultStepKind kind = DefaultStepKind::FILL_OBJECT;
  ObjectLitExpr *object;
  std::vector<const Field *> fields;
};

using DefaultStep = Union<DefaultFor, EnterEmpty, ResolveFamily, LeaveEmpty, MakeNew, FillObject>;

struct DefaultRun {
  llvm::SmallPtrSet<const ClassDecl *, 4> visiting;
  llvm::SMLoc loc;
  std::vector<DefaultStep> steps;
  std::vector<Expr *> values;
  bool hasNoDefault;
};

enum class TypeUse {
  INNER,
  STANDALONE,
  STANDALONE_OR_VOID,
};

enum class TypeArgumentsUse {
  CLASS,
  ALIAS,
  TAIL_CALL,
};

enum class BranchCall {
  PLAIN,
  TAIL,
};

struct InferRef {
  const TypeRef *ref;
  bool isList;
};

struct UnionDraft {
  const TypeRef *ref;
  size_t next;
  bool isWithNull;
  std::vector<const Type *> members;
};

struct TypeArgumentsDraft {
  const TypeRef *ref;
  Stmt *decl;
  TypeArgumentsUse use;
  size_t next;
  std::vector<const Type *> args;
};

struct UtilityDraft {
  const TypeRef *ref;
  const UtilityTypeName *utility;
  size_t next;
  std::vector<const Type *> operands;
};

struct FunctionTypeDraft {
  const TypeRef *ref;
  size_t next;
  std::vector<const Type *> types;
  size_t minArgs;
  bool hasOptional;
  bool isValid;
};

struct ObjectDraft {
  const TypeRef *ref;
  TypeRef *target;
  size_t next;
  std::vector<Field> fields;
  std::vector<Overload> overloads;
};

struct MappedOperands {
  const Type *key;
  const Type *keys;
  const Type *value;
  const Type *source;
  const Type *rename;
  int optional;
  int readonly;
};

struct MappedDraft {
  MappedOperands mapped;
  std::vector<const Type *> keys;
  size_t next;
  std::vector<Field> fields;
  std::vector<std::string> fieldNames;
  const Type *type;
  bool isReadonly;
};

struct MappedPartsDraft {
  const TypeRef *ref;
  const Type *keys;
  const Type *source;
  const Type *key;
};

struct IntersectionDraft {
  const TypeRef *ref;
  size_t next;
  std::vector<const Type *> parts;
};

struct ConditionalDraft {
  const TypeRef *ref;
  bool isMember;
  bool isTail;
  const Type *check;
  const Type *pattern;
  std::vector<InferRef> inferRefs;
  std::vector<const Type *> infers;
  size_t next;
};

struct DistributionDraft {
  const TypeRef *ref;
  std::vector<const Type *> members;
  size_t next;
  std::vector<const Type *> results;
};

struct TemplateDraft {
  const TypeRef *ref;
  size_t next;
  std::vector<const Type *> operands;
};

enum class TypeOfSource {
  KNOWN,
  RESOLVED,
  LITERAL_SHAPE,
  CREATED,
  FUNCTION_VALUE,
  CLASS_VALUE,
  DECLARED_FUNCTION,
};

struct TypeOfCandidate {
  TypeOfSource source;
  const Type *known;
  const TypeRef *ref;
  const Expr *init;
  const NewExpr *create;
  FuncSymbol *function;
  ClassDecl *classDecl;
  const FunctionDecl *functionDecl;
};

struct TypeOfDraft {
  const TypeRef *ref;
  std::vector<TypeOfCandidate> candidates;
  size_t next;
  const Type *type;
  size_t part;
};

struct UseModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::USE_MODULE;
  Module *module;
};

struct DeclareClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_CLASS_TYPE_PARAMS;
  ClassDecl *classDecl;
};

struct BindClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_CLASS_TYPE_PARAMS;
  const ClassDecl *classDecl;
};

struct DeclareFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_FUNCTION_TYPE_PARAMS;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct BindFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_FUNCTION_TYPE_PARAMS;
  const FunctionDecl *functionDecl;
};

struct InstantiateOwnAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_OWN_ALIAS;
  TypeAliasDecl *alias;
};

struct SubstituteInstanceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_INSTANCE_FIELDS;
  ClassDecl *instance;
};

struct InstantiateEmitterParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_EMITTER_PARENT;
  ClassDecl *classDecl;
  ClassDecl *emitter;
  TypeRef *ref;
};

struct MarkOverridden {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_OVERRIDDEN;
  FunctionDecl *functionDecl;
};

struct FindStreamHookParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_STREAM_HOOK_PARAMS;
  FunctionDecl *functionDecl;
};

struct WalkHookParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_HOOK_PARAMS;
  const FunctionDecl *functionDecl;
  std::vector<FunctionDecl *> inherited;
};

struct WalkIterableParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_ITERABLE_PARAM;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
  TypeRef *ref;
};

struct WalkGeneratorReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_GENERATOR_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct WalkAsyncReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_ASYNC_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct WalkStmt {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_STMT;
  Stmt *stmt;
};

struct ReportUnlistenedErrors {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_UNLISTENED_ERRORS;
};

struct EnterLoadBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_LOAD_BODY;
};

struct LeaveLoadBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_LOAD_BODY;
};

struct ImportBuiltinModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::IMPORT_BUILTIN_MODULE;
  Module *module;
  const ImportDecl *importDecl;
};

struct InstantiateClassType {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_CLASS_TYPE;
  ClassDecl *classDecl;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
};

struct InstantiateAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_ALIAS;
  TypeAliasDecl *alias;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
};

struct SubstituteType {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_TYPE;
  const Type *type;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
};

struct FindIntlType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_INTL_TYPE;
  llvm::StringRef name;
};

struct ResolveResultType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_RESULT_TYPE;
  const TypeRef *ref;
  TypeName typeName;
};

struct ResolveIteratorType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ITERATOR_TYPE;
  const TypeRef *ref;
};

struct ResolveDictType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_DICT_TYPE;
  const TypeRef *ref;
};

struct ResolvePromiseType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_PROMISE_TYPE;
  const TypeRef *ref;
};

struct MakeMethodDescriptor {
  static constexpr CheckTaskKind kind = CheckTaskKind::MAKE_METHOD_DESCRIPTOR;
  const Type *params;
  const Type *returned;
};

struct ResolveConstructorType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_CONSTRUCTOR_TYPE;
  const TypeRef *ref;
};

struct CheckConstructorArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONSTRUCTOR_ARGS;
  const TypeRef *ref;
};

struct ClassValueType {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLASS_VALUE_TYPE;
  ClassDecl *classDecl;
};

struct LiteralShape {
  static constexpr CheckTaskKind kind = CheckTaskKind::LITERAL_SHAPE;
  const Expr *init;
};

struct CreatedType {
  static constexpr CheckTaskKind kind = CheckTaskKind::CREATED_TYPE;
  const NewExpr *create;
};

struct InferReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_RETURN_TYPE;
  FuncSymbol *function;
};

struct ReturnTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::RETURN_TYPE_OF;
  FuncSymbol *function;
};

struct ResolveTypeRef {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_TYPE_REF;
  TypeRef *ref;
  bool isVoidAllowed;
};

struct ResolveInnerTypeRef {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_INNER_TYPE_REF;
  TypeRef *ref;
};

struct ResolveObjectFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_OBJECT_FIELDS;
  TypeRef *ref;
};

struct WriteResolvedType {
  static constexpr CheckTaskKind kind = CheckTaskKind::WRITE_RESOLVED_TYPE;
  TypeRef *ref;
};

struct ResolveTypeAnswer {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_TYPE_ANSWER;
  const TypeRef *ref;
  TypeUse use;
};

struct FinishTypeUse {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPE_USE;
  const TypeRef *ref;
  TypeUse use;
};

struct FinishArrayType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ARRAY_TYPE;
  const TypeRef *ref;
};

struct NextUnionMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_UNION_MEMBER;
  UnionDraft draft;
};

struct AfterUnionMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UNION_MEMBER;
  UnionDraft draft;
};

struct FinishAliasType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ALIAS_TYPE;
  TypeAliasDecl *alias;
};

struct FinishEnumType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ENUM_TYPE;
  EnumDecl *enumDecl;
};

struct FinishEnumMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ENUM_MEMBER;
  const TypeRef *ref;
  EnumDecl *enumDecl;
};

struct FinishIntlType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTL_TYPE;
  const TypeRef *ref;
};

struct NextTypeArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TYPE_ARGUMENT;
  TypeArgumentsDraft draft;
};

struct AfterTypeArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_ARGUMENT;
  TypeArgumentsDraft draft;
};

struct FinishDescriptor {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DESCRIPTOR;
  const TypeRef *ref;
};

struct FinishInstanceType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INSTANCE_TYPE;
  const TypeRef *ref;
};

struct NextUtilityOperand {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_UTILITY_OPERAND;
  UtilityDraft draft;
};

struct AfterUtilityOperand {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UTILITY_OPERAND;
  UtilityDraft draft;
};

struct AfterUtilitySignature {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UTILITY_SIGNATURE;
  UtilityDraft draft;
};

struct NextFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_FUNCTION_PARAM;
  FunctionTypeDraft draft;
};

struct AfterFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FUNCTION_PARAM;
  FunctionTypeDraft draft;
};

struct FinishFunctionType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_TYPE;
  FunctionTypeDraft draft;
};

struct FinishAssertsType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ASSERTS_TYPE;
  const TypeRef *returned;
};

struct FinishTupleType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TUPLE_TYPE;
  const TypeRef *ref;
};

struct NextObjectField {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_OBJECT_FIELD;
  ObjectDraft draft;
};

struct AfterObjectField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OBJECT_FIELD;
  ObjectDraft draft;
};

struct AfterObjectSignature {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OBJECT_SIGNATURE;
  ObjectDraft draft;
};

struct EvalTypeOp {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_TYPE_OP;
  const Type *op;
  std::vector<const Type *> operands;
};

struct EvalKeyOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_KEY_OF;
  const Type *operand;
};

struct EvalIndexed {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_INDEXED;
  const Type *object;
  const Type *key;
};

struct EvalSpread {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_SPREAD;
  std::vector<const Type *> operands;
};

struct StartKeyOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_KEY_OF;
  const TypeRef *ref;
};

struct AfterKeyOfOperand {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_KEY_OF_OPERAND;
  const TypeRef *ref;
};

struct AfterKeyOfEval {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_KEY_OF_EVAL;
  const TypeRef *ref;
  const Type *operand;
};

struct StartIndexed {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INDEXED;
  const TypeRef *ref;
};

struct AfterIndexedOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEXED_OPERANDS;
  const TypeRef *ref;
};

struct AfterIndexedEval {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEXED_EVAL;
  const TypeRef *ref;
  const Type *object;
  const Type *key;
};

struct EvalMapped {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_MAPPED;
  const Type *op;
  std::vector<const Type *> operands;
};

struct NextMappedKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_MAPPED_KEY;
  MappedDraft draft;
};

struct AfterMappedValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MAPPED_VALUE;
  MappedDraft draft;
};

struct AfterMappedRename {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MAPPED_RENAME;
  MappedDraft draft;
};

struct StartMapped {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_MAPPED;
  const TypeRef *ref;
};

struct AfterMappedKeys {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MAPPED_KEYS;
  const TypeRef *ref;
};

struct AfterMappedSource {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MAPPED_SOURCE;
  const TypeRef *ref;
  const Type *keys;
};

struct AfterMappedParts {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MAPPED_PARTS;
  MappedPartsDraft draft;
};

struct StartIntersection {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INTERSECTION;
  const TypeRef *ref;
};

struct NextIntersectionPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INTERSECTION_PART;
  IntersectionDraft draft;
};

struct AfterIntersectionPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INTERSECTION_PART;
  IntersectionDraft draft;
};

struct StartConditional {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CONDITIONAL;
  const TypeRef *ref;
  bool isMember;
};

struct AfterConditionalCheck {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONDITIONAL_CHECK;
  ConditionalDraft draft;
};

struct AfterInferConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INFER_CONSTRAINT;
  ConditionalDraft draft;
};

struct AfterConditionalPattern {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONDITIONAL_PATTERN;
  ConditionalDraft draft;
};

struct ChooseBranch {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHOOSE_BRANCH;
  ConditionalDraft draft;
  BranchCall call;
};

struct CloseConditionalScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_CONDITIONAL_SCOPE;
};

struct AfterDistributedMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DISTRIBUTED_MEMBER;
  DistributionDraft draft;
};

struct EvalConditional {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_CONDITIONAL;
  std::vector<const Type *> operands;
};

struct AfterEvalConditional {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EVAL_CONDITIONAL;
  bool isTail;
};

struct EvalFiltered {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_FILTERED;
  TypeOp kind;
  const Type *members;
  const Type *filter;
};

struct FinishFiltered {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FILTERED;
  TypeOp kind;
  const Type *members;
  const Type *filter;
};

struct PrepareExpectation {
  static constexpr CheckTaskKind kind = CheckTaskKind::PREPARE_EXPECTATION;
  const Type *type;
  unsigned depth;
};

struct FinishBranches {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_BRANCHES;
  const Type *conditional;
  unsigned depth;
};

struct StartTemplate {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TEMPLATE;
  const TypeRef *ref;
};

struct AfterTemplateHole {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TEMPLATE_HOLE;
  TemplateDraft draft;
};

struct FinishTemplate {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TEMPLATE;
};

struct EvalTemplate {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_TEMPLATE;
  std::vector<const Type *> operands;
};

struct EvalStringCase {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_STRING_CASE;
  TypeOp kind;
  const Type *operand;
};

struct EvalFunctionPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVAL_FUNCTION_PART;
  TypeOp kind;
  const Type *operand;
};

struct StartTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TYPE_OF;
  const TypeRef *ref;
};

struct FindTypeOfCandidates {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_TYPE_OF_CANDIDATES;
  const TypeRef *ref;
};

struct AfterTypeOfCandidate {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_OF_CANDIDATE;
  TypeOfDraft draft;
};

struct AfterTypeOfMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_OF_MEMBER;
  TypeOfDraft draft;
  FuncSymbol *function;
};

struct AfterFunctionValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FUNCTION_VALUE;
  FuncSymbol *function;
};

struct FinishDeclaredFunctionValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_FUNCTION_VALUE;
  const FunctionDecl *functionDecl;
  size_t typedCount;
};

struct SignatureForUtility {
  static constexpr CheckTaskKind kind = CheckTaskKind::SIGNATURE_FOR_UTILITY;
  const TypeRef *ref;
};

struct ContinueSignature {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONTINUE_SIGNATURE;
  const TypeRef *ref;
};

struct AfterErasedReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ERASED_RETURN;
  FuncSymbol *function;
};

struct ResolveImport {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_IMPORT;
  Module *module;
  const ImportDecl *importDecl;
};

struct StartTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TOP_LEVEL;
  Module *module;
  llvm::StringRef until;
};

struct NextTopLevelItem {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TOP_LEVEL_ITEM;
  Module *module;
  llvm::StringRef until;
};

struct FinishTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TOP_LEVEL;
  Module *module;
  Module *outer;
};

struct CloseTopLevel {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_TOP_LEVEL;
  Module *module;
  Module *outer;
};

struct StartLoadEarly {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LOAD_EARLY;
  Module *source;
  llvm::StringRef name;
};

struct LoadEarlyBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LOAD_EARLY_BODY;
  Module *source;
  llvm::StringRef name;
};

struct FinishLoadEarly {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LOAD_EARLY;
  Module *source;
};

struct BindLoadedNames {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_LOADED_NAMES;
  Module *source;
};

struct ResolveAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ALIAS;
  TypeAliasDecl *alias;
};

struct StartAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_ALIAS;
  TypeAliasDecl *alias;
};

struct EnterAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_ALIAS;
  TypeAliasDecl *alias;
  std::optional<Module *> previousModule;
};

struct FinishAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ALIAS;
  TypeAliasDecl *alias;
  std::optional<Module *> previousModule;
  bool isBuiltinModule;
};

struct ResolveEnum {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ENUM;
  EnumDecl *enumDecl;
};

struct EnterEnum {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_ENUM;
  EnumDecl *enumDecl;
  std::optional<Module *> previousModule;
};

struct NextEnumMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_ENUM_MEMBER;
  EnumFrame frame;
};

struct StartFunctionSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_SYMBOL;
  FunctionDecl *functionDecl;
  VarSymbol *variable;
  bool isNamed;
};

struct FinishFunctionSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_SYMBOL;
  FunctionSymbolDraft draft;
};

struct StartFunctionParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_PARAMS;
  FunctionSymbolDraft draft;
};

struct StartFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_PARAM;
  FunctionSymbolDraft draft;
  size_t index;
};

struct FinishFunctionParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_PARAM;
  FunctionSymbolDraft draft;
  size_t index;
  bool isListContext;
};

struct StartFunctionReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FUNCTION_RETURN;
  FunctionSymbolDraft draft;
};

struct FinishFunctionReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_RETURN;
  FunctionSymbolDraft draft;
};

struct StartReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_RETURN_TYPE;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct FinishAssertsReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ASSERTS_RETURN;
  const FunctionDecl *functionDecl;
};

struct FinishPredicateReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PREDICATE_RETURN;
  const FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct ResolveClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_CLASS;
  ClassDecl *classDecl;
};

struct OpenClassScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::OPEN_CLASS_SCOPE;
  ClassDecl *classDecl;
};

struct CloseClassScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_CLASS_SCOPE;
};

struct ResolveClassMembers {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_CLASS_MEMBERS;
  ClassDecl *classDecl;
};

struct StartClassField {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLASS_FIELD;
  ClassDecl *classDecl;
  size_t index;
};

struct FinishClassField {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_FIELD;
  ClassDecl *classDecl;
  size_t index;
  TypeRef *written;
};

struct StartClassBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLASS_BASES;
  ClassDecl *classDecl;
};

struct FinishClassBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_BASES;
  ClassDecl *classDecl;
};

struct FinishParentClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PARENT_CLASS;
  ClassDecl *classDecl;
  Stmt *found;
};

struct FinishLinkParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_PARENT;
  ClassDecl *classDecl;
  ClassDecl *parent;
};

struct FinishImplemented {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_IMPLEMENTED;
  ClassDecl *classDecl;
  TypeRef *ref;
};

struct AddBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_BASE;
  const Type *type;
  const Type *base;
};

struct DeclareMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_METHODS;
  ClassDecl *classDecl;
};

struct FinishConstructor {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONSTRUCTOR;
  FunctionDecl *constructor;
};

struct FinishClassMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_METHOD;
  const ClassDecl *classDecl;
  FunctionDecl *method;
};

struct StartFieldMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_FIELD_METHOD;
  ClassDecl *classDecl;
  size_t index;
};

struct FinishFieldMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FIELD_METHOD;
  ClassDecl *classDecl;
  size_t index;
};

struct StartTypedMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TYPED_METHOD;
  FunctionDecl *functionDecl;
  const Type *declared;
};

struct FinishTypedParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPED_PARAM;
  FunctionDecl *functionDecl;
  const Type *declared;
  size_t index;
};

struct FinishTypedMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPED_METHOD;
  FunctionDecl *functionDecl;
  const Type *declared;
};

struct FinishMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_METHODS;
  ClassDecl *classDecl;
};

struct NextFieldDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_FIELD_DEFAULT;
  ClassDecl *classDecl;
  size_t fieldIndex;
};

struct ContinueFieldDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONTINUE_FIELD_DEFAULT;
  ClassDecl *classDecl;
  size_t fieldIndex;
  DefaultRun run;
};

struct MakeFieldInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::MAKE_FIELD_INIT;
  ClassDecl *classDecl;
};

struct FinishFieldInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FIELD_INIT;
  ClassDecl *classDecl;
  FunctionDecl *init;
};

struct FinishVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VARIANTS;
  ClassDecl *classDecl;
};

struct FinishItemMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITEM_METHODS;
  ClassDecl *classDecl;
  Module *module;
};

struct CompleteClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::COMPLETE_CLASS;
  ClassDecl *classDecl;
};

struct FinishCompletion {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_COMPLETION;
  ClassDecl *classDecl;
};

struct StartInterfaceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INTERFACE_FIELDS;
  TypeAliasDecl *alias;
};

struct StartInterfaceBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_INTERFACE_BASE;
  InterfaceDraft draft;
  size_t index;
};

struct FinishInterfaceBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTERFACE_BASE;
  InterfaceDraft draft;
  size_t index;
};

struct FinishInterfaceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTERFACE_FIELDS;
  InterfaceDraft draft;
};

struct ReportVariantPair {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_VARIANT_PAIR;
  FunctionDecl *first;
  FunctionDecl *second;
};

struct StartLinkClasses {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LINK_CLASSES;
};

struct FinishLinkClasses {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_CLASSES;
  std::vector<ClassDecl *> order;
};

struct StartLinkOverrides {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LINK_OVERRIDES;
  ClassDecl *classDecl;
};

struct FinishLinkOverrides {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LINK_OVERRIDES;
  const ClassDecl *classDecl;
};

struct LinkOverride {
  static constexpr CheckTaskKind kind = CheckTaskKind::LINK_OVERRIDE;
  ClassDecl *classDecl;
  FunctionDecl *functionDecl;
};

struct StartDeclarationFile {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARATION_FILE;
  Module *module;
};

struct FinishDeclarationFile {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARATION_FILE;
  Module *module;
};

struct StartDeclaredAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_ALIAS;
  TypeAliasDecl *declared;
};

struct FinishDeclaredAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_ALIAS;
  TypeAliasDecl *declared;
  TypeAliasDecl *implemented;
};

struct StartDeclaredFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_FUNCTION;
  FunctionDecl *declared;
};

struct StartDeclaredClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_CLASS;
  DeclaredClass *declared;
};

struct FinishDeclaredClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_CLASS;
  const DeclaredClass *declared;
  const ClassDecl *classDecl;
};

struct StartDeclaredMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_MEMBER;
  DeclaredMember *member;
  const ClassDecl *classDecl;
};

struct FinishDeclaredField {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_FIELD;
  const DeclaredMember *member;
  const ClassField *field;
  const ClassDecl *classDecl;
};

struct StartDeclaredParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_DECLARED_PARAM;
  DeclaredSignatureMatch match;
  size_t index;
};

struct FinishDeclaredParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_PARAM;
  DeclaredSignatureMatch match;
  size_t index;
};

struct FinishDeclaredReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DECLARED_RETURN;
  DeclaredSignatureMatch match;
};

using CheckTask = Union<
    UseModule, DeclareClassTypeParams, BindClassTypeParams, DeclareFunctionTypeParams,
    BindFunctionTypeParams, InstantiateOwnAlias, SubstituteInstanceFields, InstantiateEmitterParent,
    MarkOverridden, FindStreamHookParams, WalkHookParams, WalkIterableParam, WalkGeneratorReturn,
    WalkAsyncReturn, WalkStmt, ReportUnlistenedErrors, EnterLoadBody, LeaveLoadBody,
    ImportBuiltinModule, InstantiateClassType, InstantiateAlias, SubstituteType, FindIntlType,
    ResolveResultType, ResolveIteratorType, ResolveDictType, ResolvePromiseType,
    MakeMethodDescriptor, ResolveConstructorType, CheckConstructorArgs, ClassValueType,
    LiteralShape, CreatedType, InferReturnType, ReturnTypeOf, ResolveTypeRef, ResolveInnerTypeRef,
    ResolveObjectFields, WriteResolvedType, ResolveTypeAnswer, FinishTypeUse, FinishArrayType,
    NextUnionMember, AfterUnionMember, FinishAliasType, FinishEnumType, FinishEnumMember,
    FinishIntlType, NextTypeArgument, AfterTypeArgument, FinishDescriptor, FinishInstanceType,
    NextUtilityOperand, AfterUtilityOperand, AfterUtilitySignature, NextFunctionParam,
    AfterFunctionParam, FinishFunctionType, FinishAssertsType, FinishTupleType, NextObjectField,
    AfterObjectField, AfterObjectSignature, EvalTypeOp, EvalKeyOf, EvalIndexed, EvalSpread,
    StartKeyOf, AfterKeyOfOperand, AfterKeyOfEval, StartIndexed, AfterIndexedOperands,
    AfterIndexedEval, EvalMapped, NextMappedKey, AfterMappedValue, AfterMappedRename, StartMapped,
    AfterMappedKeys, AfterMappedSource, AfterMappedParts, StartIntersection, NextIntersectionPart,
    AfterIntersectionPart, StartConditional, AfterConditionalCheck, AfterInferConstraint,
    AfterConditionalPattern, ChooseBranch, CloseConditionalScope, AfterDistributedMember,
    EvalConditional, AfterEvalConditional, EvalFiltered, FinishFiltered, PrepareExpectation,
    FinishBranches, StartTemplate, AfterTemplateHole, FinishTemplate, EvalTemplate, EvalStringCase,
    EvalFunctionPart, StartTypeOf, FindTypeOfCandidates, AfterTypeOfCandidate, AfterTypeOfMember,
    AfterFunctionValue, FinishDeclaredFunctionValue, SignatureForUtility, ContinueSignature,
    AfterErasedReturn, ResolveImport, StartTopLevel, NextTopLevelItem, FinishTopLevel,
    CloseTopLevel, StartLoadEarly, LoadEarlyBody, FinishLoadEarly, BindLoadedNames, ResolveAlias,
    StartAlias, EnterAlias, FinishAlias, ResolveEnum, EnterEnum, NextEnumMember,
    StartFunctionSymbol, FinishFunctionSymbol, StartFunctionParams, StartFunctionParam,
    FinishFunctionParam, StartFunctionReturn, FinishFunctionReturn, StartReturnType,
    FinishAssertsReturn, FinishPredicateReturn, ResolveClass, OpenClassScope, CloseClassScope,
    ResolveClassMembers, StartClassField, FinishClassField, StartClassBases, FinishClassBases,
    FinishParentClass, FinishLinkParent, FinishImplemented, AddBase, DeclareMethods,
    FinishConstructor, FinishClassMethod, StartFieldMethod, FinishFieldMethod, StartTypedMethod,
    FinishTypedParam, FinishTypedMethod, FinishMethods, NextFieldDefault, ContinueFieldDefault,
    MakeFieldInit, FinishFieldInit, FinishVariants, FinishItemMethods, CompleteClass,
    FinishCompletion, StartInterfaceFields, StartInterfaceBase, FinishInterfaceBase,
    FinishInterfaceFields, ReportVariantPair, StartLinkClasses, FinishLinkClasses,
    StartLinkOverrides, FinishLinkOverrides, LinkOverride, StartDeclarationFile,
    FinishDeclarationFile, StartDeclaredAlias, FinishDeclaredAlias, StartDeclaredFunction,
    StartDeclaredClass, FinishDeclaredClass, StartDeclaredMember, FinishDeclaredField,
    StartDeclaredParam, FinishDeclaredParam, FinishDeclaredReturn>;

}
