#pragma once

#include "compiler/program/ast/ast.h"
#include "compiler/program/ast/declaration_file/declaration_file.h"
#include "compiler/program/diagnostic/diagnostic.h"
#include "compiler/program/library/array/array_method_table/array_method_table.h"
#include "compiler/program/library/builtin_names/type_name_table/type_name_table.h"
#include "compiler/program/types/types.h"
#include "compiler/semantic/bodies/expr/target_checker/target_checker_types.h"
#include "compiler/semantic/generics/type_arg_inferrer/type_arg_inferrer_types.h"
#include "compiler/semantic/generics_v2/inference/candidate_inference/candidate_inference.h"
#include "compiler/semantic/semantic_state/flow_state/flow_state_types.h"
#include "compiler/semantic/semantic_state/function_stack/function_stack_types.h"
#include "compiler/semantic/semantic_state/generic_store/generic_store_types.h"
#include "compiler/semantic/semantic_state/v2/library/event_context/event_context.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <optional>
#include <string>
#include <vector>

namespace tz {

enum class CheckTaskKind {
  USE_MODULE,
  INSTANTIATE_EMITTER_PARENT,
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
  CHECK_EXPR,
  CHECK_EXPR_SLOT,
  WRITE_CHECKED_SLOT,
  REPORT_ERROR,
  CHECK_EXPR_KIND,
  AFTER_CONDITION_TEST,
  AFTER_ELSE_FIRST,
  AFTER_THEN_SECOND,
  AFTER_THEN_FIRST,
  AFTER_ELSE_SECOND,
  FINISH_CONDITIONAL,
  JOIN_CONDITIONAL_BRANCHES,
  AFTER_CAST_TARGET,
  FINISH_CAST,
  NEXT_SEQUENCE_ITEM,
  AFTER_SEQUENCE_ITEM,
  FINISH_SEQUENCE,
  FINISH_OPTIONAL_CHAIN,
  CHECK_INT_LITERAL,
  CHECK_TEMPLATE,
  CHECK_ARRAY_LITERAL,
  CHECK_OBJECT_LITERAL,
  NEXT_TEMPLATE_PART,
  AFTER_TEMPLATE_PART,
  AFTER_TEMPLATE_TYPE,
  AFTER_COMPUTED_KEY,
  AFTER_UNION_NEEDS,
  NEXT_INFERRED_KEY,
  AFTER_INFERRED_KEY,
  NEXT_KEYED_PROPERTY,
  NEXT_NAMED_PROPERTY,
  AFTER_UNTARGETED_FIELD,
  NEXT_RECORD_PROPERTY,
  AFTER_RECORD_SPREAD,
  NEXT_SPREAD_PROPERTY,
  AFTER_NAMED_PROP,
  AFTER_SPREAD_PROP,
  NEXT_LITERAL_MEMBER,
  AFTER_LITERAL_FIELD,
  AFTER_LITERAL_METHODS,
  WALK_LITERAL_METHODS,
  WALK_LITERAL_METHOD,
  AFTER_LITERAL_METHOD_RETURN,
  MATCH_LITERAL_TARGET,
  FINISH_PAIR_LITERAL,
  NEXT_ARRAY_ELEMENT,
  AFTER_LITERAL_ELEMENT,
  AFTER_SPREAD_ELEMENT,
  AFTER_SPREAD_MATCH,
  CHECK_ENUM_OBJECT,
  AFTER_ENUM_OBJECT_RESOLVED,
  REMEMBER_ENUM_OBJECT,
  CHECK_FIELD,
  CHECK_FIELD_CALL,
  AFTER_FIELD_CALLEE,
  CHECK_OBJECT_FIELD_TARGET,
  CHECK_KEYED_FIELD,
  AFTER_TUPLE_KEY,
  AFTER_OBJECT_KEY,
  OBJECT_KEY_READY,
  CHECK_NAME,
  LOOKUP_NAME,
  VARIABLE_TYPE_KNOWN,
  FINISH_VARIABLE_NAME,
  FINISH_FUNCTION_NAME,
  CONST_TEXT,
  TO_INT32,
  NOTE_UNSIGNED_VAR,
  CHECK_SHIFT,
  CHECK_ARITHMETIC,
  AFTER_ARITHMETIC_RIGHT,
  AFTER_ARITHMETIC_LEFT,
  AFTER_ARITHMETIC_OPERANDS,
  FINISH_ARITHMETIC,
  RESOLVE_ARITHMETIC,
  ARITHMETIC_RESULT,
  FINISH_INTEGER_ARITHMETIC,
  CHECK_WRAPPING_DIVISOR,
  START_BITS,
  FINISH_BITS,
  FOLD_BINARY_NODE,
  CHECK_SIGN,
  FINISH_SIGN,
  SIGN_OPERAND_READY,
  CHECK_BIT_NOT,
  FINISH_BIT_NOT,
  CHECK_CONCATENATION,
  TRACK_MIXED_CONCAT,
  TO_STRING_SLOT,
  TO_STRING_OPERAND,
  MARK_TEXT_CAST,
  CHECK_MEMBER,
  AFTER_LIST_PART,
  AFTER_NODE_MEMBER,
  AFTER_PROCESS_MEMBER,
  AFTER_BYTES_STATIC_MEMBER,
  AFTER_MEMBER_OBJECT,
  MEMBER_OF_COMMON_BASE,
  AFTER_ENUM_RESOLVED,
  CHECK_TUPLE_MEMBER,
  CHECK_TUPLE_METHOD,
  FINISH_TUPLE_METHOD,
  REPORT_TUPLE_JOIN,
  FINISH_TUPLE_JOIN,
  CHECK_UNION_MEMBER,
  CHECK_UNION_FIELD_TARGET,
  CHECK_UNION_METHOD_CALL,
  RESOLVE_UNION_METHOD_CALL,
  REPORT_UNION_METHOD_CALL,
  CHECK_UNKNOWN_MEMBER,
  CHECK_UNKNOWN_METHOD,
  FINISH_UNKNOWN_STATIC_CALL,
  FINISH_UNKNOWN_METHOD,
  CHECK_INDEX,
  CHECK_POSITION,
  FINISH_POSITION,
  AFTER_INDEX_LIST_PART,
  AFTER_PROCESS_ENV_INDEX,
  AFTER_INDEX_OBJECT,
  FINISH_GENERIC_INDEX,
  WRITE_INDEX_TYPE,
  FINISH_UNKNOWN_INDEX,
  REPORT_UNDEFINED_INDEX,
  FINISH_POSITION_INDEX,
  AFTER_ENUM_INDEX_RESOLVED,
  FINISH_ENUM_INDEX,
  ENUM_INDEX_VALUE,
  CHECK_UNARY,
  CHECK_BINARY,
  FINISH_TYPE_OF,
  FINISH_VOID,
  FINISH_NON_NULL,
  CHECK_IN_OPERATOR,
  AFTER_IN_OPERANDS,
  CHECK_EQUALITY,
  AFTER_EQUALITY_OPERANDS,
  FINISH_EQUAL_OPERANDS,
  CHECK_COMPARISON,
  AFTER_COMPARISON_OPERANDS,
  FINISH_COMPARISON,
  COMPARE_OPERANDS,
  FINISH_NUMBER_COMPARISON,
  WALK_TYPE_OF_NAME,
  CHECK_ASSIGN,
  CHECK_UPDATE,
  CHECK_DELETE_VALUE,
  AFTER_TARGET_ALIAS,
  AFTER_PROCESS_ASSIGN,
  AFTER_LOGICAL_ASSIGN,
  AFTER_NULLISH_HOLDER,
  AFTER_RECORD_FIELD_NULLISH,
  AFTER_INDEX_NULLISH,
  AFTER_FIELD_NULLISH,
  AFTER_NULLISH_VARIABLE,
  FINISH_NULLISH_VARIABLE,
  FINISH_VAR_TARGET,
  VAR_TARGET_TYPE_KNOWN,
  ANSWER_VAR_TARGET,
  DISCARD_CHECKED,
  AFTER_INDEX_TARGET_OBJECT,
  AFTER_GENERIC_TARGET,
  AFTER_TYPED_ARRAY_POSITION,
  AFTER_KEYED_TARGET,
  AFTER_ARRAY_POSITION,
  AFTER_MEMBER_TARGET_OBJECT,
  AFTER_ASSIGN_TARGET,
  AFTER_INFERRED_FIELD,
  AFTER_PLAIN_VALUE,
  AFTER_STRING_COMPOUND,
  AFTER_NUMERIC_COMPOUND,
  CHECK_ASSIGN_DIVISOR,
  COMPOUND_FACTS,
  AFTER_ASSIGN_VALUE,
  ASSIGNED_LENGTH,
  LENGTH_FACTS,
  ASSIGNED_FIELD,
  AFTER_UPDATE_TARGET,
  AFTER_MEMBER_DELETE,
  AFTER_OTHER_DELETE,
  AFTER_INDEX_DELETE_OBJECT,
  AFTER_INDEX_DELETE_KEY,
  CHECK_CALL,
  WALK_ARGS_LOOSELY,
  CHECK_ARG_SLOT,
  CHECK_ARGUMENTS,
  CHECK_NEW,
  CHECK_VALUE_CALL,
  CHECK_DIRECT_CALL,
  AFTER_NODE_CALL_TRY,
  AFTER_RESULT_STATIC_TRY,
  AFTER_CONSOLE_TRY,
  AFTER_PROCESS_TRY,
  AFTER_BYTES_STATIC_TRY,
  AFTER_RECEIVER_OBJECT,
  AFTER_RESULT_METHOD_TRY,
  AFTER_DATE_METHOD_TRY,
  AFTER_MATCH_RECEIVER,
  AFTER_COMMON_RECEIVER,
  AFTER_SUBTLE_TRY,
  AFTER_CLASS_METHOD_CALL,
  AFTER_NAMED_CALL_TRY,
  SCOPED_CALL,
  AFTER_KNOWN_FUNCTION_TYPE,
  AFTER_FUNCTION_NAME_TRY,
  AFTER_NOT_CALLABLE_NAME,
  AFTER_CALLEE_VALUE,
  AFTER_DIRECT_RETURN_TYPE,
  AFTER_SPREAD_REST,
  AFTER_NEW_LOAD,
  AFTER_CLASS_NEW,
  AFTER_ARRAY_ELEMENT,
  AFTER_ARRAY_LENGTH,
  CHECK_CLASS_EXPR,
  CHECK_CLASS_STMT,
  CHECK_FIELDS_SET,
  CHECK_FIELD_TARGET,
  CHECK_STATIC_TARGET,
  CHECK_METHOD_CALL,
  CHECK_INSTANCE_OF_OPERATOR,
  CHECK_THIS,
  CHECK_STATIC_MEMBER,
  CHECK_CLASS_MEMBER,
  CHECK_OVERLOAD_MEMBER,
  CHECK_CLASS_REF_MEMBER,
  CHECK_CLASS_STATIC_CALL,
  CHECK_SUPER_CALL,
  CHECK_SUPER_METHOD_CALL,
  CHECK_VALUE_NEW,
  CHECK_CLASS_NEW,
  CHECK_OVERLOAD_CALL,
  CHECK_CLASS_VALUE,
  BIND_OUTER_TYPE_PARAMS,
  DECLARE_THIS,
  WALK_CLASS_BODIES,
  AFTER_LATE_CLASS_EXPR,
  FINISH_CLASS_EXPR,
  WALK_STATIC_FIELDS,
  LEAVE_STATIC_FIELDS,
  WALK_STATIC_FIELD,
  AFTER_STATIC_FIELD,
  DECORATE_CLASS,
  LEAVE_CLASS_CONTEXT,
  AFTER_LATE_BASE,
  LEAVE_DECLARING_CLASS,
  FINISH_LATE_CLASS,
  WALK_CLASS_FUNCTION,
  WALK_FIELD_STARTS,
  WALK_CLASS_METHOD,
  AFTER_CLASS_METHOD_RETURN,
  FINISH_CLASS_BODIES,
  NEXT_LATE_CLASS,
  WALK_LATE_CLASS,
  FINISH_LATE_CLASS_WALK,
  AFTER_CONSTRUCTOR_TYPE_ARGUMENT,
  AFTER_METHOD_VARIANTS,
  AFTER_MEMBER_FUNCTION_TYPE,
  AFTER_STATIC_VARIANTS,
  AFTER_NAME_FUNCTION_TYPE,
  AFTER_CLASS_REF_STATIC,
  AFTER_OVERLOAD_VARIANTS,
  AFTER_INSTANCE_OF_LEFT,
  INSTANCE_OF_RIGHT,
  AFTER_INSTANCE_OF_RIGHT,
  AFTER_METHOD_CALL_TRY,
  AFTER_CLASS_FIELD_CALLEE,
  AFTER_METHOD_CHOICE,
  AFTER_PLAIN_METHOD_TYPE,
  AFTER_GENERIC_METHOD_CALL,
  SET_CALL_TYPE,
  START_GENERIC_CALL_TYPES,
  AFTER_BOUND_TYPES,
  AFTER_GENERIC_CALLEE,
  CHOOSE_VARIANT,
  RANK_VARIANTS,
  NEXT_RANKED_VARIANT,
  AFTER_INFERRED_ARGUMENTS,
  AFTER_GENERIC_PARAMS,
  FINISH_SUPER_CALL,
  AFTER_PARENT_CONSTRUCTOR_CHOICE,
  AFTER_SUPER_OBJECT,
  AFTER_SUPER_EMITTER_TRY,
  AFTER_SUPER_METHOD_CHOICE,
  AFTER_VALUE_NEW_CALLEE,
  NEXT_REFLECTED_ARG,
  AFTER_REFLECTED_ARG,
  AFTER_CONSTRUCTOR_CHOICE,
  FINISH_CLASS_NEW,
  AFTER_OVERLOAD_CHOICE,
  AFTER_EVENTS_FUNCTION_TRY,
  AFTER_STATIC_FIELD_CALLEE,
  AFTER_STATIC_METHOD_CHOICE,
  DECORATE,
  NEXT_HOISTED_DECORATOR,
  AFTER_FACTORY_METHOD_RETURN,
  AFTER_DESCRIPTOR_METHOD_RETURN,
  DECLARE_HOISTED_DECORATOR,
  AFTER_DESCRIPTOR_DECLARED,
  NEXT_APPLIED_DECORATOR,
  AFTER_DECORATOR_APPLIED,
  RELINK_CLASS,
  DROP_DIAGNOSTIC_LIST,
  WALK_INFERRED_BODY,
  CHECK_THIS_CALL,
  CHECK_LIST_METHOD,
  CHECK_LIST_ARGUMENTS,
  WALK_STATEMENTS,
  NOTE_USE,
  FIND_LIST_PART,
  CHECK_FUNCTION_EXPR,
  CHECK_FUNCTION_VALUE,
  WALK_FUNCTION_BODY,
  AFTER_BODY_TYPE_PARAMS,
  AFTER_BODY_THIS,
  NEXT_BODY_PARAM,
  AFTER_DEFAULTED_PARAM,
  AFTER_BODY_STATEMENTS,
  ENTER_ERROR_WATCH,
  LEAVE_ERROR_WATCH,
  AFTER_PREDICATE_END,
  FINISH_FUNCTION_BODY,
  WALK_FUNCTION_FROM_SCRATCH,
  DECLARE_ARGUMENT_LIST,
  REMEMBER_ARGUMENT_LIST,
  AFTER_BLOCK_TYPES,
  ENTER_STATEMENTS,
  NEXT_STATEMENT,
  AFTER_STATEMENT,
  GROUP_MEMBER,
  AFTER_GROUP_MEMBER_RETURN,
  GROUP_USE_ORDER,
  LEAVE_GROUP,
  NEXT_CLOSURE_PARAM,
  AFTER_CLOSURE_PARAM_TYPE,
  AFTER_CLOSURE_RETURN_TYPE,
  AFTER_CLOSURE_RETURN,
  START_CLOSURE_BODY,
  LEAVE_INITIALIZER_CLOSURE,
  KEEP_CONSTANT_FLOW,
  DECLARE_OWN_NAME,
  AFTER_CLOSURE_TYPE,
  CHECK_FUNCTION_ARG_SLOT,
  AFTER_LITERAL_CALLBACK,
  AFTER_CALLBACK_VALUE,
  LEAVE_CALLBACK,
  CHECK_CALLBACK_METHOD,
  AFTER_EXTRA_CALLBACK_ARGS,
  AFTER_REDUCE_INIT,
  FINISH_CALLBACK_METHOD,
  CHECK_ARRAY_CALLBACK_RESULT,
  NEXT_LIST_ARGUMENT,
  AFTER_LIST_ARGUMENT,
  START_MODULE_BODIES,
  WALK_TOP_FUNCTION,
  AFTER_TOP_RETURN_TYPE,
  START_LIFTED_FUNCTION,
  CHECK_YIELD,
  FIND_ITERATOR,
  ASYNC_USER_ITERATOR,
  ITERABLE_OF_SLOT,
  ELEMENT_TYPE_OF,
  DESTRUCTURED_ITERATOR_SLOT,
  FINISH_ITERATOR_RESULT,
  FINISH_ITERATOR_ARGS,
  FINISH_ITERABLE_PARAM,
  AFTER_YIELD_VALUE,
  AFTER_YIELD_DELEGATE,
  FINISH_YIELD,
  AFTER_ITERABLE_USER,
  AFTER_BUILTIN_ITERATOR,
  USER_ITERATOR,
  AFTER_SYMBOL_ITERATOR,
  AFTER_RESULT_ITERATOR,
  AFTER_ASYNC_SYMBOL_ITERATOR,
  ITERATOR_OF,
  AFTER_ITERATOR_VALUE,
  BUILD_ITERATE_CALL,
  FINISH_ITERATE_SYMBOL,
  FINISH_ITERATE_CALL,
  NEXT_VALUE_OF,
  AFTER_NEXT_METHODS,
  AFTER_SOLE_ITERATOR_RETURN,
  CHECK_CONDITION,
  FINISH_CONDITION,
  START_BRANCH,
  AFTER_BRANCH_LEAF,
  FINISH_BRANCH_LEAF,
  AFTER_ASSIGNED_LEAF,
  AFTER_BRANCH_LEFT,
  AFTER_BRANCH_RIGHT,
  AFTER_BRANCH_NOT,
  CHECK_NOT,
  CHECK_LOGICAL_OPERATOR,
  FINISH_BRANCH_JOIN,
  START_TESTED_VALUE,
  AFTER_TESTED_LEAF,
  START_LOGICAL,
  AFTER_LOGICAL_LEFT,
  START_LOGICAL_RIGHT,
  FINISH_TESTED_LOGICAL,
  FINISH_VALUE_LOGICAL,
  FINISH_LOGICAL_TYPE,
  START_NULLISH,
  AFTER_NULLISH_LEFT,
  START_NULLISH_RIGHT,
  FINISH_NULLISH,
  CONVERT,
  COERCE,
  COERCE_SLOT,
  COERCE_ITERATOR_FOUND,
  COERCE_ASYNC_ITERATOR_FOUND,
  APPARENT_SLOT,
  CONVERT_SLOT,
  ADAPT_LITERAL_SLOT,
  CHECK_DECIMAL_LITERAL_RANGE,
  CHECK_REGEX_LITERAL,
  CHECK_AWAIT,
  CHECK_USED_VALUE,
  WALK_REDUNDANT_TESTS,
  MATCH_ITEMS,
  WALK_COMPARISON_OPERANDS,
  NEVER_OPERANDS,
  CHECK_DATE_OPERANDS,
  CHECK_DECIMAL_COMPARISON,
  CHECK_NULL_EQUALITY,
  CHECK_JSON_EQUALITY,
  REPORT_DECIMAL_MIX,
  CHECK_DECIMAL_ARITHMETIC,
  DATE_TO_TIME,
  REGEX_TO_STRING,
  DATE_TO_STRING,
  BYTES_TO_STRING,
  NOTE_REFLECTED_CONVERSION,
  NOTE_CONSTRUCTOR_FLOW,
  CHECK_NODE_NAME,
  FUNCTION_TYPE_OF,
  NOTE_HTTPS_VALUE,
  CHECK_JSON_MEMBER,
  CHECK_RESULT_MEMBER,
  CHECK_REGEX_MEMBER,
  CHECK_MATCH_MEMBER,
  CHECK_DATE_MEMBER,
  CHECK_BYTES_MEMBER,
  CHECK_CRYPTO_MEMBER,
  CHECK_RUNTIME_MEMBER,
  CHECK_REG_EXP_STATIC_MEMBER,
  CHECK_WEB_CRYPTO_MEMBER,
  CHECK_NODE_MEMBER,
  CHECK_PROCESS_MEMBER,
  CHECK_BYTES_STATIC_MEMBER,
  CHECK_INTL_MEMBER,
  CHECK_MATCH_INDEX,
  CHECK_JSON_INDEX,
  CHECK_BYTES_INDEX,
  CHECK_PROCESS_ENV_INDEX,
  CHECK_BYTES_UNION_MEMBER,
  PROCESS_ALIAS_SLOT,
  MARK_FUSED,
  RESOLVE_WEAK_REF_TYPE,
  SEGMENTS_ARRAY_SLOT,
  NOTE_INHERITED_RETURN,
  CHECK_USED_CALLBACK_VALUE,
  WALK_FOR_AWAIT,
  NOTE_ERRORS_HANDLED,
  FIND_CALL_CYCLES,
  CHECK_PROCESS_ASSIGN,
  CHECK_JSON_TARGET,
  CHECK_RUNTIME_TARGET,
  WALK_DECIMAL_ASSIGN,
  WALK_BYTES_STORE,
  CHECK_NODE_CALL,
  CHECK_PROMISE_STATIC,
  CHECK_RESULT_STATIC,
  CHECK_JSON_STATIC,
  CHECK_ARRAY_IS_ARRAY,
  CHECK_JSON_IS_JSON,
  CHECK_REFLECT_CALL,
  CHECK_CONSOLE_CALL,
  CHECK_PROCESS_CALL,
  CHECK_BIT_CALL,
  CHECK_MATH_CALL,
  CHECK_REG_EXP_STATIC,
  CHECK_INTL_CALL,
  CHECK_LOCALE_STATIC,
  CHECK_INTL_STATIC,
  CHECK_DECIMAL_STATIC,
  CHECK_X509_PARSE,
  CHECK_WEB_CRYPTO_CALL,
  CHECK_BYTES_STATIC,
  CHECK_NUMBER_STATIC,
  CHECK_OBJECT_CALL,
  CHECK_ITERATOR_FROM,
  CHECK_ARRAY_FROM,
  CHECK_MAP_STATIC,
  CHECK_RUNTIME_METHOD,
  CHECK_RESULT_METHOD,
  CHECK_JSON_METHOD,
  CHECK_REGEX_METHOD,
  CHECK_DATE_METHOD,
  CHECK_DATE_FORMAT_METHOD,
  CHECK_INTL_METHOD,
  CHECK_CRYPTO_METHOD,
  CHECK_WEAK_REF_METHOD,
  CHECK_BYTES_METHOD,
  CHECK_PROMISE_METHOD,
  CHECK_TIMER_METHOD,
  CHECK_SUBTLE_CALL,
  WALK_HEADERS_CALL,
  CHECK_STRING_LOCALE,
  CHECK_NUMBER_LOCALE,
  CHECK_DECIMAL_METHOD,
  CHECK_ITERATOR_METHOD,
  CHECK_DICT_METHOD,
  CHECK_STRING_METHOD,
  CHECK_ARRAY_METHOD,
  CHECK_NUMBER_METHOD,
  CHECK_LIB_CALL,
  CHECK_RESULT_CALL,
  CHECK_JSON_GLOBAL,
  CHECK_ASYNC_GLOBAL,
  CHECK_GLOBAL_CALL,
  CHECK_EVENTS_LIBRARY_CALL,
  CHECK_STREAM_LIBRARY_CALL,
  NOTE_HTTPS_CALL,
  CHECK_GENERIC_CALL,
  CHECK_CRYPTO_NEW,
  CHECK_LOCALE_NEW,
  CHECK_DATE_FORMAT_NEW,
  CHECK_INTL_NEW,
  NOTE_LIBRARY_INSTANCE,
  CHECK_REG_EXP_CONSTRUCTOR,
  CHECK_DATE_NEW,
  CHECK_PROMISE_NEW,
  CHECK_WEAK_REF_NEW,
  CHECK_BYTES_NEW,
  FIND_METHOD_CYCLES,
  CHECK_JSON_OBJECT_LIT,
  CHECK_JSON_ARRAY_LIT,
  WALK_EXTERNAL,
  ENTER_RETURN_BODY,
  LEAVE_RETURN_BODY,
  INFER_FROM_BODY,
  INFER_CYCLE,
  ENTER_HOME_MODULE,
  NOTE_TLS_METHOD,
  CHECK_STATS_DATE,
  REPORT_EMITTER_METHOD_VALUE,
  CHECK_FS_CLASS_METHOD,
  CHECK_STREAM_CALL,
  CHECK_EMITTER_CALL,
  CHECK_EVENT_TARGET_CALL,
  CHECK_EVENTS_FUNCTION,
  CHECK_STREAM_FROM,
  WRITE_DESIGN_RECORDS,
  BUILD_VTABLE,
  NOTE_REGEX_VARIABLE,
  LEAVE_BRANCH,
  MARK_UNREACHABLE,
  AFTER_EXPR_STMT,
  FINISH_EXPR_STMT,
  WALK_BODY,
  WALK_VAR_DECL,
  CLOSE_SCOPE,
  NEXT_DECLARATOR,
  AFTER_DECLARED_TYPE,
  AFTER_CONST_FUNCTION_SYMBOL,
  FINISH_CONST_FUNCTION,
  AFTER_EARLY_TIMER,
  FIND_DESTINATION,
  AFTER_INIT,
  DESTRUCTURE_VALUE,
  AFTER_DESTRUCTURED_ITERATOR,
  REMEMBER_VALUE_DECLARATOR,
  WALK_IF,
  WALK_SWITCH,
  AFTER_IF_CONDITION,
  AFTER_THEN_BRANCH,
  FINISH_IF,
  AFTER_SWITCH_VALUE,
  NEXT_CASE_TEST,
  AFTER_CASE_TEST,
  ADD_CASE_FACTS,
  FINISH_CONSTANT_CASE,
  FINISH_CASE_TEST,
  NEXT_CASE_BODY,
  WALK_CASE_STATEMENT,
  AFTER_CASE_BODY,
  FINISH_SWITCH,
  DECLARE_BLOCK_TYPES,
  DECLARE_METHOD_RETURN,
  FINISH_METHOD_RETURN,
  DECLARE_VARIABLE_SLOT,
  DECLARE_VARIABLE,
  WALK_WHILE,
  WALK_DO_WHILE,
  WALK_FOR,
  WALK_FOR_OF,
  WALK_LABELED,
  CHECK_JUMP,
  FINISH_LABELED_BLOCK,
  LEAVE_LOOP_BODY,
  RESTORE_LOOP_FLOW,
  AFTER_WHILE_CONDITION,
  AFTER_DO_WHILE_BODY,
  AFTER_DO_WHILE_CONDITION,
  AFTER_FOR_INIT,
  AFTER_FOR_CONDITION,
  AFTER_FOR_BODY,
  AFTER_FOR_STEP,
  FINISH_FOR,
  AFTER_FOR_AWAIT,
  AFTER_FOR_OF_SOURCE,
  AFTER_ITERABLE,
  AFTER_FOR_IN_OBJECT,
  BIND_FOR_OF,
  AFTER_FOR_OF_BINDING,
  WALK_RETURN,
  AFTER_UNLISTENED_ERRORS,
  AFTER_RETURN_VALUE,
  AFTER_TESTED_RETURN,
  AFTER_PREDICATE_RETURN,
  AFTER_PREDICATE_FACTS,
  AFTER_VALUE_COERCE,
  AFTER_CONSTRUCTOR_VALUE,
  AFTER_INFERRED_RETURN,
  AFTER_ASYNC_GENERATOR_RETURN,
  CHECK_DICT_IN,
  CHECK_DICT_MEMBER,
  CHECK_DICT_INDEX,
  CHECK_RECORD_KEY_SLOT,
  FINISH_RECORD_KEY,
  FINISH_DICT_INDEX_READ,
  FINISH_DICT_INDEX_ERROR,
  CHECK_DICT_TARGET,
  FINISH_DICT_TARGET,
  CHECK_RECORD_NULLISH_ASSIGN,
  AFTER_NULLISH_RECORD_OBJECT,
  AFTER_NULLISH_RECORD_KEY,
  FINISH_NULLISH_RECORD,
  CHECK_DICT_DELETE,
  FINISH_DICT_DELETE,
  FINISH_DICT_TYPE_ANSWER,
  CHECK_DICT_NEW,
  AFTER_DICT_NEW_TYPE_ARGS,
  NEXT_PAIR_PART,
  AFTER_PAIR_PART,
  FINISH_PAIRS_DICT,
  AFTER_DICT_SOURCE,
  FINISH_DICT_SOURCE,
  CALLED_FUNCTION,
  NOTE_ASSIGNED,
  NOTE_INCREMENTED,
  NOTE_ARRAY_RESIZED,
  BEGIN_PREDICATE,
  END_PREDICATE,
  NOTE_PREDICATE_EXIT,
  BUILD_PREDICATE,
  START_PREDICATE_COPY,
  ADD_BYTE_CALLBACK_FACTS,
  ADD_INDEX_CALLBACK_FACTS,
  CHECK_BYTE_CALLBACK_RESULT,
  ADD_CONDITION_FACTS,
  FINISH_CONDITION_FACTS,
  WALK_FRESH_COPY,
  WALK_BODY_NOW,
  FINISH_FILTER_RESULT,
  FINISH_FILTER_DEFINED,
  SUBSTITUTE_TYPE,
  SUBSTITUTE_VALUE_TYPE,
  SUBSTITUTE_NODE,
  BUILD_SUBSTITUTED_TYPE,
  INSTANCE_TYPE,
  INSTANCE_VALUE_TYPE,
  SUBSTITUTE_INSTANCE_FIELDS,
  NEXT_INSTANCE_FIELD,
  AFTER_INSTANCE_FIELD,
  SUBSTITUTE_TYPE_LIST,
  AFTER_TYPE_LIST_ITEM,
  START_GENERIC_VALUE,
  CHECK_GENERIC_VALUE,
  NEXT_VALUE_TYPE_ARG,
  AFTER_VALUE_TYPE_ARG,
  AFTER_GENERIC_VALUE_ARGS,
  AFTER_GENERIC_VALUE_TYPE,
  INSTANTIATE_CALL_TARGET,
  AFTER_REUSED_WALK,
  NEXT_MEANINGFUL_PARAM,
  AFTER_MEANINGFUL_PARAM,
  AFTER_INSTANCE_TYPES,
  AFTER_NEW_INSTANCE_WALK,
  WALK_PRECISE_COPY,
  CHECK_CONSTRAINT,
  AFTER_CONSTRAINT_SUBSTITUTION,
  EVALUATE_CONSTRAINT,
  AFTER_CONSTRAINT_METHOD,
  INSTANTIATE_OWN_ALIAS,
  AFTER_OWN_ALIAS_PARAMS,
  DROP_OWN_ALIAS_ANSWER,
  INSTANTIATE_ALIAS,
  AFTER_ALIAS_PARAMS,
  AFTER_ALIAS_DEFAULT,
  AFTER_ALIAS_MATCH,
  AFTER_ALIAS_OBJECT,
  AFTER_ALIAS_INNER,
  CLOSE_ALIAS_BODY,
  DECLARE_CLASS_TYPE_PARAMS,
  AFTER_CLASS_TYPE_PARAMS,
  LEAVE_CLASS_TYPE_PARAMS,
  BIND_CLASS_TYPE_PARAMS,
  DECLARE_FUNCTION_TYPE_PARAMS,
  AFTER_FUNCTION_TYPE_PARAMS,
  BIND_FUNCTION_TYPE_PARAMS,
  DECLARE_ALIAS_TYPE_PARAMS,
  AFTER_ALIAS_TYPE_PARAMS,
  START_TYPE_PARAMS,
  NEXT_TYPE_PARAM,
  AFTER_CONSTRAINT_TYPE,
  NEXT_CONSTRAINT_MEMBER,
  AFTER_CONSTRAINT_MEMBER,
  AFTER_DEFAULT_TYPE,
  AFTER_DEFAULT_CONSTRAINT,
  CHECK_GENERIC_NEW,
  AFTER_EMITTER_NEW,
  NEXT_EXPLICIT_NEW_ARGUMENT,
  AFTER_EXPLICIT_NEW_ARGUMENT,
  AFTER_EXPLICIT_INSTANCE,
  AFTER_GENERIC_NEW_BINDING,
  AFTER_GENERIC_NEW_INSTANCE,
  INSTANTIATE_CLASS_TYPE,
  AFTER_INSTANCE_TYPE_PARAMS,
  NEXT_INSTANCE_DEFAULT,
  AFTER_INSTANCE_DEFAULT,
  NEXT_INSTANCE_ARGUMENT,
  AFTER_INSTANCE_ARGUMENT,
  FINISH_CLASS_INSTANCE,
  WALK_OR_POSTPONE,
  AFTER_INSTANCE_WALK,
  FILL_INSTANCE,
  LEAVE_FILLING,
  LINK_INSTANCE_BASES,
  AFTER_INSTANCE_PARENT,
  NEXT_INSTANCE_INTERFACE,
  AFTER_INSTANCE_INTERFACE,
  NEXT_FILLED_FIELD,
  AFTER_FILLED_FIELD,
  DECLARE_INSTANCE_MEMBERS,
  NEXT_INSTANCE_MEMBER,
  AFTER_INSTANCE_INIT,
  NEXT_ITERATED_PARAM,
  AFTER_ITERATED_PARAM,
  AFTER_MEMBER_SIGNATURE,
  NEXT_MEMBER_TYPE,
  AFTER_MEMBER_INSTANCE_TYPE,
  AFTER_MEMBER_TYPE,
  WALK_INSTANCE_VARIANTS,
  AFTER_INSTANCE_VARIANTS,
  LINK_INSTANCE,
  MARK_OVERRIDDEN,
  FORCE_CONVERSION,
  WALK_INSTANCE,
  WALK_PRECISE_BODY,
  WALK_TEMPLATE_INSTANCE,
  START_COPY_BODY,
  POP_COPY_DIAGNOSTICS,
  FINISH_COPY_WALK,
  REPORT_INSTANCE,
  REPORT_INSTANCE_AT,
  NEXT_PENDING_INSTANCE,
  REPORT_FAILED_PENDING,
  WALK_CLASS_INSTANCE,
  NEXT_CLASS_BODY,
  START_CLASS_BODY,
  CLASS_BODY_ENTERED,
  POP_CLASS_BODY_DIAGNOSTICS,
  REPORT_CLASS_INSTANCE,
  FINISH_TYPE_ARGS,
  BIND_GENERIC_CALL,
  START_BINDING,
  INFER_FROM_ARGUMENTS,
  NEXT_BINDING_ARGUMENT,
  INFER_FROM_ARGUMENT,
  NEXT_PRECHECKED_FIELD,
  AFTER_PRECHECKED_FIELD,
  INFER_PRECHECKED_FIELD,
  INFER_FROM_LIST_REST,
  RETRY_DEFERRED,
  NEXT_CONTEXTUAL_ARGUMENT,
  AFTER_CONTEXTUAL_PARTIAL,
  AFTER_DEFAULTED_HINT,
  AFTER_CONTEXTUAL_ARGUMENT,
  AFTER_EXPLICIT_TYPE_ARGS,
  AFTER_BINDING_TYPE_ARGS,
  AFTER_BOUND_PARAMS,
  NEXT_LITERAL_ARGUMENT,
  AFTER_LITERAL_HINT,
  AFTER_BOUND_RESULT,
  AFTER_BOUND_ARGUMENTS,
  NEXT_TYPE_ARG_CHECK,
  AFTER_TYPE_ARG_DEFAULT,
  AFTER_ITERATED_ARG_FIT,
  AFTER_TYPE_ARG_CONSTRAINT,
  INFER_ARGUMENT_PATTERNS,
  NEXT_FITTING_ARGUMENT,
  AFTER_FITTING_DEFAULT,
  AFTER_FITTING_CONSTRAINT,
  JOIN_INSTANCES,
  AFTER_JOINED_INSTANCE,
  CHECK_EMITTER_NEW,
  CHECK_STREAM_NEW,
  ADD_STREAM_BASES,
  WALK_VARIANTS,
  BUILD_INSTANCE_INIT,
  CHECK_CONSTRUCTOR_ACCESS,
  CHECK_BUILTIN_CALLBACK_SLOT,
  ASYNC_ITERABLE_TYPE_OF,
  AFTER_ASYNC_ITERABLE_RETURN,
  ANSWER_RETURN_TYPE,
  ANSWER_FUNCTION_TYPE,
  AFTER_TEMPLATE_RETURN,
  AFTER_TEMPLATE_INSTANCE,
  SET_INFERRED_RETURN,
  AFTER_INHERITED_RETURN,
  OPEN_BODY_CHECK,
  CLOSE_BODY_CHECK,
  FINISH_TOP_LEVEL_ENTRY,
  FINISH_INFER_FROM_BODY,
  NEXT_CYCLE_TRIAL,
  AFTER_CYCLE_TRIAL,
  FINISH_TRIAL_RETURN,
  AFTER_BODY_NOW_INFERENCE,
  FINISH_ASYNC_RETURNS,
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

struct ConditionFlows {
  Flow whenTrue;
  Flow whenFalse;
  std::optional<bool> constant;
};

enum class LiteralSign {
  PLAIN,
  NEGATED,
};

enum class KeyedAccess {
  READ,
  WRITE,
};

enum class RightSide {
  TESTED,
  VALUE,
};

struct ArithmeticDraft {
  BinaryExpr *binary;
  const Type *hint;
  const Type *rhsHint;
  bool isBitwise;
  bool canHintLiterals;
  BinaryExpr *pending;
  bool isChainEnd;
  const BinaryExpr *outerMixed;
  bool isForgettingChain;
};

struct YieldShape {
  FuncSymbol *function;
  bool isAsync;
  const Type *yielded;
  const Type *sent;
  bool isLoose;
};

struct SwitchDraft {
  SwitchStmt *switchStmt;
  const Type *type;
  const Type *hint;
  Flow tests;
  std::vector<Flow> matched;
  Flow fallthrough;
  bool hasDefault;
  size_t index;
  size_t position;
};

struct ForDraft {
  ForStmt *forStmt;
  llvm::SmallVector<llvm::StringRef, 1> labels;
  LoopScan scan;
  Flow head;
  Flow whenTrue;
  Flow whenFalse;
};

struct DeclaratorDraft {
  VarDeclStmt *decl;
  size_t number;
  const Type *declared;
  llvm::ArrayRef<Stmt *> block;
  size_t blockIndex;
  const Type *type;
  const Type *hint;
  Expr *value;
  bool isEarly;
  bool isUntyped;
  std::vector<std::pair<const Type *, llvm::SMLoc>> places;
};

struct AssignPlace {
  IndexExpr *index;
  MemberExpr *member;
  NameExpr *name;
  const VarSymbol *variable;
  AssignForm form;
  bool hasReportedRange;
  bool isDefinite;
};

enum class NamedCallStage {
  LIBRARY,
  RESULT,
  JSON,
  TIMER,
  GLOBAL,
  SCOPED,
};

struct NamedCallDraft {
  CallExpr *call;
  NameExpr *name;
  const Type *expected;
  NamedCallStage stage;
};

enum class FunctionNameStage {
  EVENTS,
  STREAM,
  DIRECT,
};

struct FunctionNameDraft {
  CallExpr *call;
  NameExpr *name;
  FuncSymbol *function;
  const Type *expected;
  FunctionNameStage stage;
};

struct TemplateDraft {
  TemplateExpr *templateExpr;
  const Type *expected;
  std::vector<const Type *> parts;
  bool isConstant;
  size_t index;
};

struct ObjectLitDraft {
  ObjectLitExpr *literal;
  const Type *wanted;
  const Type *target;
  std::vector<bool> named;
  bool isTyped;
  bool isInferred;
  std::vector<Field> fields;
  size_t index;
};

struct RecordDraft {
  ObjectLitExpr *literal;
  const Type *record;
  bool hasFailed;
  size_t index;
};

struct SpreadDraft {
  ObjectLitExpr *literal;
  const Type *target;
  std::vector<bool> named;
  std::vector<Field> fields;
  bool hasRecords;
  bool isTyped;
  size_t index;
};

struct MethodLiteralDraft {
  ObjectLitExpr *literal;
  const Type *wanted;
  Type *type;
  std::vector<Field> fields;
  bool hasFieldsTyped;
  size_t index;
};

struct ArrayDraft {
  ArrayLitExpr *literal;
  const Type *expectedElement;
  std::vector<const Type *> types;
  bool hasDecimal;
  const Type *seen;
  const Type *literalHint;
  bool isUnseen;
  size_t index;
};

struct PairsDraft {
  NewExpr *newExpr;
  ArrayLitExpr *literal;
  size_t element;
  bool isValue;
  const Type *key;
  const Type *value;
  bool isTyped;
};

struct LogicalDraft {
  BinaryExpr *binary;
  const Type *expected;
  RightSide side;
  TruthUse use;
  Flow lhsTrue;
  Flow lhsFalse;
  const Type *leftType;
  bool isTruthy;
};

struct BodyDraft {
  FunctionDecl *functionDecl;
  FuncSymbol *outer;
  bool isInitializer;
  size_t index;
};

struct StatementsDraft {
  BlockStmt *block;
  NestedGroup *group;
  size_t index;
};

struct ClosureDraft {
  FunctionExpr *functionExpr;
  FuncSymbol *function;
  std::optional<Signature> signature;
  bool isGeneric;
  bool isThisFromContext;
  size_t index;
};

struct ArrayCallbackShape {
  CallExpr *call;
  MemberExpr *member;
  const Type *arrayType;
  const Type *element;
  ArrayCallback kind;
  bool isTyped;
  size_t count;
  const Type *accumulator;
  bool hasSomeValue;
  bool isBoolean;
};

struct ListArgumentsDraft {
  CallExpr *call;
  const Type *list;
  std::vector<const Type *> parts;
  llvm::SMLoc loc;
  std::vector<bool> checked;
  bool isTyped;
  size_t used;
  size_t index;
};

struct DecoratorUse {
  const Decorator *decorator;
  int param = -1;
  const FunctionDecl *owner = nullptr;
};

enum class DecoratorPlace {
  CLASS,
  FIELD,
  METHOD,
  PARAMETER,
};

struct DecorateRun {
  ClassDecl *classDecl;
  ClassField *field;
  FunctionDecl *method;
  std::vector<DecoratorUse> uses;
  DecoratorPlace ownPlace;
  llvm::StringRef memberName;
  llvm::SMLoc at;
  const Type *earlyDescriptor;
  const Type *methodReturn;
  bool isMethodReturnKnown;
  std::vector<Expr *> callees;
  std::vector<size_t> takes;
  bool hasDescriptor;
  VarSymbol *descriptorVar;
  const Type *descriptorType;
  ClassDecl *current;
  size_t index;
};

enum class GenericVariants {
  ALLOWED,
  REJECTED,
};

enum class GenericCallee {
  METHOD,
  FUNCTION,
};

struct VariantCandidate {
  FunctionDecl *functionDecl;
  std::vector<int> ranks;
  bool isLeavingOut;
  std::vector<const Type *> typeArgs;
  bool isAdapted;
};

struct VariantRanking {
  std::vector<VariantCandidate> applicable;
  std::string why;
  std::string numberHint;
};

struct ChoiceDraft {
  std::vector<FunctionDecl *> fit;
  std::vector<Expr *> *args;
  std::string what;
  GenericVariants generics;
  VariantRanking ranking;
  size_t index;
};

enum class MethodCallStage {
  FILE_SYSTEM,
  STREAM,
  EMITTER,
  EVENT_TARGET,
  CLASS,
};

struct MethodCallDraft {
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
  const Type *expected;
  MethodCallStage stage;
};

struct ReflectNewDraft {
  NewExpr *newExpr;
  const Type *type;
  std::vector<const Type *> argTypes;
  size_t index;
};

enum class Resize {
  GROWS,
  SHRINKS,
  CHANGES,
};

enum class VoidAs {
  VOID,
  UNDEFINED,
};

enum class SubstituteForm {
  TYPE_OPERATOR,
  OPTIONAL,
  ARRAY,
  RESULT,
  FUNCTION,
  ITERATOR,
  DICT,
  UNION,
  PROMISE,
  CONSTRUCTOR,
  ASYNC_ITERATOR,
  DESCRIPTOR,
  CLASS_INSTANCE,
  ALIAS_INSTANCE,
  TUPLE,
  OBJECT,
};

struct ConstraintCheck {
  const Type *arg;
  const Type *param;
  const Type *own;
  std::vector<std::pair<const FuncSymbol *, const Type *>> known;
};

struct AliasInstance;

struct AliasDraft {
  TypeAliasDecl *alias;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
  std::vector<const Type *> params;
  std::vector<const Type *> defaults;
  std::vector<AliasInstance *> chain;
  AliasInstance *entry;
  std::string display;
  unsigned calls;
  size_t index;
  bool isMoved;
};

struct TypeParamsDraft {
  llvm::ArrayRef<TypeParamDecl> decls;
  std::vector<Type *> created;
  std::vector<const Type *> defaults;
  const Type *constraint;
  std::vector<const Type *> alternatives;
  bool isResolved;
  size_t index;
  size_t member;
  bool isAfterDefault;
};

struct ExplicitNewDraft {
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
  std::vector<const Type *> args;
};

struct ClassInstanceDraft {
  ClassDecl *generic;
  std::vector<const Type *> args;
  llvm::SMLoc use;
  size_t index;
};

struct InstanceMember {
  FunctionDecl *functionDecl;
  FunctionDecl *generic;
  bool isInit;
};

struct InstanceMembersDraft {
  ClassDecl *instance;
  std::vector<InstanceMember> members;
  size_t member;
  std::vector<const Type *> instanceParams;
  size_t param;
  bool hasInit;
};

struct MemberTypesDraft {
  const ClassDecl *instance;
  std::vector<const Type *> genericParams;
  std::vector<const Type *> instanceParams;
  std::vector<const Type *> types;
  std::vector<bool> isValues;
  size_t index;
  std::vector<const Type *> results;
};

struct TypeListDraft {
  std::vector<const Type *> types;
  std::vector<bool> isValues;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
  size_t index;
  std::vector<const Type *> results;
};

struct GenericValueDraft {
  NameExpr *name;
  FuncSymbol *generic;
  const Type *returnType;
  std::vector<const Type *> bound;
};

struct InstantiateDraft {
  FuncSymbol *generic;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
  std::vector<const Type *> exact;
  std::vector<const Type *> reps;
  NameExpr *name;
  CallExpr *call;
  bool isCopyingReturn;
  InstanceRecord *instance;
  FuncSymbol *function;
  FunctionDecl *copy;
  size_t index;
  bool isPrecise;
};

enum class CopyWalk {
  INSTANCE,
  PRECISE,
  TEMPLATE,
};

struct ClassBodiesDraft {
  InstanceRecord *instance;
  std::vector<FunctionDecl *> bodies;
  size_t index;
  std::vector<Diagnostic> diagnostics;
};

enum class BindingOwner {
  BIND,
  CALL,
};

enum class MissingTypeArgs {
  REPORTED,
  SILENT,
};

struct PendingLookups {
  std::vector<const Type *> elementTypes;
  std::vector<const Type *> asyncTypes;
};

struct GenericBindingDraft {
  CallExpr *call;
  FuncSymbol *generic;
  llvm::SMLoc loc;
  const Type *expected;
  llvm::ArrayRef<TypeRef> typeArgs;
  BindingOwner owner;
  const Type *result;
  const Type *listRest;
  size_t fixed;
  unsigned errorsBefore;
  std::vector<const Type *> bound;
  std::vector<bool> checked;
  std::vector<size_t> literals;
  std::vector<size_t> contextual;
  std::vector<const Type *> params;
  bool isInferred;
  std::optional<CandidateInference> inference;
  PendingLookups pending;
  size_t index;
};

struct TypeArgsDraft {
  FuncSymbol *generic;
  std::vector<const Type *> bound;
  llvm::SMLoc loc;
  TypeArgUse use;
  MissingTypeArgs missing;
  bool isInferred;
  size_t index;
};

struct InferredArgumentsDraft {
  FuncSymbol *generic;
  std::vector<Expr *> args;
  std::optional<CandidateInference> inference;
  PendingLookups pending;
  std::vector<const Type *> bound;
  size_t index;
};

enum class CycleStep {
  KEPT,
  CHANGED,
  FAILED,
};

struct DestinationTypes {
  std::vector<std::pair<const TypeRef *, const Type *>> known;
  std::vector<std::pair<const TypeRef *, TypeUse>> missing;
};

struct KnownReturns {
  std::vector<std::pair<const FuncSymbol *, const Type *>> known;
  std::vector<FuncSymbol *> missing;
};

struct CycleDraft {
  std::vector<FuncSymbol *> cycle;
  int round;
  size_t index;
  CycleStep roundStep;
};

struct UseModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::USE_MODULE;
  Module *module;
};

struct InstantiateEmitterParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_EMITTER_PARENT;
  ClassDecl *classDecl;
  ClassDecl *emitter;
  TypeRef *ref;
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

struct CheckExpr {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EXPR;
  Expr *expr;
  const Type *expected;
};

struct CheckExprSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EXPR_SLOT;
  Expr **slot;
  const Type *expected;
};

struct WriteCheckedSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::WRITE_CHECKED_SLOT;
  Expr **slot;
};

struct ReportError {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_ERROR;
  llvm::SMLoc loc;
  std::string message;
};

struct CheckExprKind {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EXPR_KIND;
  Expr *expr;
  const Type *expected;
};

struct AfterConditionTest {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONDITION_TEST;
  ConditionalExpr *conditional;
  const Type *expected;
};

struct AfterElseFirst {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ELSE_FIRST;
  ConditionalExpr *conditional;
  const Type *expected;
  Flow whenTrue;
};

struct AfterThenSecond {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_THEN_SECOND;
  ConditionalExpr *conditional;
  Flow afterElse;
};

struct AfterThenFirst {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_THEN_FIRST;
  ConditionalExpr *conditional;
  const Type *expected;
  Flow whenFalse;
  bool isElseWaitingOnly;
};

struct AfterElseSecond {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ELSE_SECOND;
  ConditionalExpr *conditional;
  Flow afterThen;
};

struct FinishConditional {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONDITIONAL;
  ConditionalExpr *conditional;
};

struct JoinConditionalBranches {
  static constexpr CheckTaskKind kind = CheckTaskKind::JOIN_CONDITIONAL_BRANCHES;
  ConditionalExpr *conditional;
  const Type *left;
  const Type *right;
};

struct AfterCastTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CAST_TARGET;
  CastExpr *castExpr;
};

struct FinishCast {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CAST;
  CastExpr *castExpr;
  bool isArrayLiteral;
};

struct NextSequenceItem {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_SEQUENCE_ITEM;
  SequenceExpr *sequence;
  size_t index;
  const Type *expected;
};

struct AfterSequenceItem {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SEQUENCE_ITEM;
  SequenceExpr *sequence;
  size_t index;
  const Type *expected;
};

struct FinishSequence {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_SEQUENCE;
  SequenceExpr *sequence;
};

struct FinishOptionalChain {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_OPTIONAL_CHAIN;
  OptionalChainExpr *chain;
  Flow before;
};

struct CheckIntLiteral {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INT_LITERAL;
  IntLitExpr *literal;
  const Type *expected;
  LiteralSign sign;
};

struct CheckTemplate {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_TEMPLATE;
  TemplateExpr *templateExpr;
  const Type *expected;
};

struct CheckArrayLiteral {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARRAY_LITERAL;
  ArrayLitExpr *literal;
  const Type *expected;
};

struct CheckObjectLiteral {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_OBJECT_LITERAL;
  ObjectLitExpr *literal;
  const Type *expected;
};

struct NextTemplatePart {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TEMPLATE_PART;
  TemplateDraft draft;
};

struct AfterTemplatePart {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TEMPLATE_PART;
  TemplateDraft draft;
};

struct AfterTemplateType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TEMPLATE_TYPE;
  TemplateExpr *templateExpr;
  const Type *want;
  bool isConstant;
};

struct AfterComputedKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_COMPUTED_KEY;
  ObjectLitDraft draft;
};

struct AfterUnionNeeds {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UNION_NEEDS;
  ObjectLitDraft draft;
};

struct NextInferredKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INFERRED_KEY;
  ObjectLitDraft draft;
};

struct AfterInferredKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INFERRED_KEY;
  ObjectLitDraft draft;
};

struct NextKeyedProperty {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_KEYED_PROPERTY;
  ObjectLitDraft draft;
};

struct NextNamedProperty {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_NAMED_PROPERTY;
  ObjectLitDraft draft;
};

struct AfterUntargetedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UNTARGETED_FIELD;
  ObjectLitDraft draft;
};

struct NextRecordProperty {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_RECORD_PROPERTY;
  RecordDraft draft;
};

struct AfterRecordSpread {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RECORD_SPREAD;
  RecordDraft draft;
};

struct NextSpreadProperty {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_SPREAD_PROPERTY;
  SpreadDraft draft;
};

struct AfterNamedProp {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NAMED_PROP;
  SpreadDraft draft;
};

struct AfterSpreadProp {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SPREAD_PROP;
  SpreadDraft draft;
};

struct NextLiteralMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_LITERAL_MEMBER;
  MethodLiteralDraft draft;
};

struct AfterLiteralField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_FIELD;
  MethodLiteralDraft draft;
};

struct AfterLiteralMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_METHODS;
  ObjectLitExpr *literal;
  const Type *wanted;
  Type *type;
};

struct WalkLiteralMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_LITERAL_METHODS;
  ClassDecl *classDecl;
};

struct WalkLiteralMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_LITERAL_METHOD;
  FunctionDecl *functionDecl;
};

struct AfterLiteralMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_METHOD_RETURN;
  FunctionDecl *functionDecl;
};

struct MatchLiteralTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::MATCH_LITERAL_TARGET;
  ObjectLitExpr *literal;
  const Type *wanted;
  Type *type;
};

struct FinishPairLiteral {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PAIR_LITERAL;
  ArrayLitExpr *literal;
};

struct NextArrayElement {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_ARRAY_ELEMENT;
  ArrayDraft draft;
};

struct AfterLiteralElement {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_ELEMENT;
  ArrayDraft draft;
};

struct AfterSpreadElement {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SPREAD_ELEMENT;
  ArrayDraft draft;
};

struct AfterSpreadMatch {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SPREAD_MATCH;
  ArrayDraft draft;
};

struct CheckEnumObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ENUM_OBJECT;
  NameExpr *name;
  EnumDecl *enumDecl;
  const Type *expected;
};

struct AfterEnumObjectResolved {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ENUM_OBJECT_RESOLVED;
  NameExpr *name;
  EnumDecl *enumDecl;
  const Type *expected;
};

struct RememberEnumObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::REMEMBER_ENUM_OBJECT;
  NameExpr *name;
  EnumDecl *enumDecl;
};

struct CheckField {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FIELD;
  MemberExpr *member;
};

struct CheckFieldCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FIELD_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct AfterFieldCallee {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FIELD_CALLEE;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckObjectFieldTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_OBJECT_FIELD_TARGET;
  MemberExpr *member;
};

struct CheckKeyedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_KEYED_FIELD;
  IndexExpr *index;
  KeyedAccess access;
};

struct AfterTupleKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TUPLE_KEY;
  IndexExpr *index;
  KeyedAccess access;
};

struct AfterObjectKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OBJECT_KEY;
  IndexExpr *index;
  KeyedAccess access;
};

struct ObjectKeyReady {
  static constexpr CheckTaskKind kind = CheckTaskKind::OBJECT_KEY_READY;
  IndexExpr *index;
  KeyedAccess access;
};

struct CheckName {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NAME;
  NameExpr *name;
};

struct LookupName {
  static constexpr CheckTaskKind kind = CheckTaskKind::LOOKUP_NAME;
  NameExpr *name;
};

struct VariableTypeKnown {
  static constexpr CheckTaskKind kind = CheckTaskKind::VARIABLE_TYPE_KNOWN;
  NameExpr *name;
  VarSymbol *variable;
};

struct FinishVariableName {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VARIABLE_NAME;
  NameExpr *name;
  VarSymbol *variable;
};

struct FinishFunctionName {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_NAME;
  NameExpr *name;
  FuncSymbol *function;
};

struct ConstText {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONST_TEXT;
  const Type *expected;
};

struct ToInt32 {
  static constexpr CheckTaskKind kind = CheckTaskKind::TO_INT32;
};

struct NoteUnsignedVar {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_UNSIGNED_VAR;
  const VarSymbol *variable;
  const Expr *init;
};

struct CheckShift {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_SHIFT;
  BinaryExpr *binary;
  const Type *expected;
};

struct CheckArithmetic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARITHMETIC;
  BinaryExpr *binary;
  const Type *expected;
};

struct AfterArithmeticRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARITHMETIC_RIGHT;
  ArithmeticDraft draft;
};

struct AfterArithmeticLeft {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARITHMETIC_LEFT;
  ArithmeticDraft draft;
};

struct AfterArithmeticOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARITHMETIC_OPERANDS;
  ArithmeticDraft draft;
};

struct FinishArithmetic {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ARITHMETIC;
  ArithmeticDraft draft;
};

struct ResolveArithmetic {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_ARITHMETIC;
  ArithmeticDraft draft;
};

struct ArithmeticResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::ARITHMETIC_RESULT;
  BinaryExpr *binary;
};

struct FinishIntegerArithmetic {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INTEGER_ARITHMETIC;
  BinaryExpr *binary;
  const Type *common;
};

struct CheckWrappingDivisor {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_WRAPPING_DIVISOR;
  BinaryExpr *binary;
};

struct StartBits {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_BITS;
  BinaryExpr *binary;
};

struct FinishBits {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_BITS;
  BinaryExpr *binary;
};

struct FoldBinaryNode {
  static constexpr CheckTaskKind kind = CheckTaskKind::FOLD_BINARY_NODE;
  BinaryExpr *binary;
};

struct CheckSign {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_SIGN;
  UnaryExpr *unary;
  const Type *expected;
};

struct FinishSign {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_SIGN;
  UnaryExpr *unary;
};

struct SignOperandReady {
  static constexpr CheckTaskKind kind = CheckTaskKind::SIGN_OPERAND_READY;
  UnaryExpr *unary;
};

struct CheckBitNot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BIT_NOT;
  UnaryExpr *unary;
  const Type *expected;
};

struct FinishBitNot {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_BIT_NOT;
  UnaryExpr *unary;
};

struct CheckConcatenation {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONCATENATION;
  BinaryExpr *binary;
};

struct TrackMixedConcat {
  static constexpr CheckTaskKind kind = CheckTaskKind::TRACK_MIXED_CONCAT;
  BinaryExpr *binary;
  bool isChainEnd;
  const BinaryExpr *outerMixed;
};

struct ToStringSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::TO_STRING_SLOT;
  Expr **slot;
};

struct ToStringOperand {
  static constexpr CheckTaskKind kind = CheckTaskKind::TO_STRING_OPERAND;
};

struct MarkTextCast {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_TEXT_CAST;
};

struct CheckMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct AfterListPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LIST_PART;
  MemberExpr *member;
  const Type *expected;
};

struct AfterNodeMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NODE_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct AfterProcessMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PROCESS_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct AfterBytesStaticMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BYTES_STATIC_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct AfterMemberObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_OBJECT;
  MemberExpr *member;
  const Type *expected;
};

struct MemberOfCommonBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::MEMBER_OF_COMMON_BASE;
  MemberExpr *member;
  const Type *common;
  const Type *expected;
};

struct AfterEnumResolved {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ENUM_RESOLVED;
  MemberExpr *member;
  EnumDecl *enumDecl;
  const Type *expected;
};

struct CheckTupleMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_TUPLE_MEMBER;
  MemberExpr *member;
};

struct CheckTupleMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_TUPLE_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct FinishTupleMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TUPLE_METHOD;
  CallExpr *call;
  MemberExpr *member;
  bool isResizing;
};

struct ReportTupleJoin {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_TUPLE_JOIN;
  CallExpr *call;
  MemberExpr *member;
  std::string message;
};

struct FinishTupleJoin {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TUPLE_JOIN;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckUnionMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNION_MEMBER;
  MemberExpr *member;
};

struct CheckUnionFieldTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNION_FIELD_TARGET;
  MemberExpr *member;
};

struct CheckUnionMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNION_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct ResolveUnionMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_UNION_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct ReportUnionMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_UNION_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
  std::string message;
};

struct CheckUnknownMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNKNOWN_MEMBER;
  MemberExpr *member;
};

struct CheckUnknownMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNKNOWN_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct FinishUnknownStaticCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_UNKNOWN_STATIC_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct FinishUnknownMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_UNKNOWN_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INDEX;
  IndexExpr *index;
};

struct CheckPosition {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_POSITION;
  Expr **slot;
};

struct FinishPosition {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_POSITION;
  Expr **slot;
};

struct AfterIndexListPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_LIST_PART;
  IndexExpr *index;
};

struct AfterProcessEnvIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PROCESS_ENV_INDEX;
  IndexExpr *index;
};

struct AfterIndexObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_OBJECT;
  IndexExpr *index;
};

struct FinishGenericIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_GENERIC_INDEX;
  IndexExpr *index;
};

struct WriteIndexType {
  static constexpr CheckTaskKind kind = CheckTaskKind::WRITE_INDEX_TYPE;
  IndexExpr *index;
};

struct FinishUnknownIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_UNKNOWN_INDEX;
  IndexExpr *index;
};

struct ReportUndefinedIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_UNDEFINED_INDEX;
  IndexExpr *index;
};

struct FinishPositionIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_POSITION_INDEX;
  IndexExpr *index;
};

struct AfterEnumIndexResolved {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ENUM_INDEX_RESOLVED;
  IndexExpr *index;
  EnumDecl *enumDecl;
};

struct FinishEnumIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ENUM_INDEX;
  IndexExpr *index;
  EnumDecl *enumDecl;
};

struct EnumIndexValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENUM_INDEX_VALUE;
  IndexExpr *index;
  EnumDecl *enumDecl;
};

struct CheckUnary {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UNARY;
  UnaryExpr *unary;
  const Type *expected;
};

struct CheckBinary {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BINARY;
  BinaryExpr *binary;
  const Type *expected;
};

struct FinishTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPE_OF;
  UnaryExpr *unary;
};

struct FinishVoid {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VOID;
  UnaryExpr *unary;
};

struct FinishNonNull {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_NON_NULL;
  UnaryExpr *unary;
};

struct CheckInOperator {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_IN_OPERATOR;
  BinaryExpr *binary;
};

struct AfterInOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_IN_OPERANDS;
  BinaryExpr *binary;
  std::optional<std::string> key;
  bool isLiteralKey;
  std::string array;
};

struct CheckEquality {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EQUALITY;
  BinaryExpr *binary;
};

struct AfterEqualityOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EQUALITY_OPERANDS;
  BinaryExpr *binary;
};

struct FinishEqualOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_EQUAL_OPERANDS;
  BinaryExpr *binary;
};

struct CheckComparison {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_COMPARISON;
  BinaryExpr *binary;
};

struct AfterComparisonOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_COMPARISON_OPERANDS;
  BinaryExpr *binary;
};

struct FinishComparison {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_COMPARISON;
  BinaryExpr *binary;
};

struct CompareOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::COMPARE_OPERANDS;
  BinaryExpr *binary;
};

struct FinishNumberComparison {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_NUMBER_COMPARISON;
  BinaryExpr *binary;
};

struct WalkTypeOfName {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_TYPE_OF_NAME;
  const Expr *operand;
  const StringLitExpr *text;
};

struct CheckAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ASSIGN;
  AssignExpr *assign;
};

struct CheckUpdate {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_UPDATE;
  UpdateExpr *update;
};

struct CheckDeleteValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DELETE_VALUE;
  UnaryExpr *unary;
};

struct AfterTargetAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TARGET_ALIAS;
  AssignExpr *assign;
};

struct AfterProcessAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PROCESS_ASSIGN;
  AssignExpr *assign;
};

struct AfterLogicalAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LOGICAL_ASSIGN;
  BinaryExpr *binary;
};

struct AfterNullishHolder {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NULLISH_HOLDER;
  AssignExpr *assign;
};

struct AfterRecordFieldNullish {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RECORD_FIELD_NULLISH;
  AssignExpr *assign;
};

struct AfterIndexNullish {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_NULLISH;
  AssignExpr *assign;
};

struct AfterFieldNullish {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FIELD_NULLISH;
  AssignExpr *assign;
  AssignExpr *set;
};

struct AfterNullishVariable {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NULLISH_VARIABLE;
  AssignExpr *assign;
};

struct FinishNullishVariable {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_NULLISH_VARIABLE;
  AssignExpr *assign;
  const Type *type;
  Flow before;
};

struct FinishVarTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VAR_TARGET;
  NameExpr *name;
};

struct VarTargetTypeKnown {
  static constexpr CheckTaskKind kind = CheckTaskKind::VAR_TARGET_TYPE_KNOWN;
  NameExpr *name;
  VarSymbol *variable;
};

struct AnswerVarTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::ANSWER_VAR_TARGET;
  NameExpr *name;
};

struct DiscardChecked {
  static constexpr CheckTaskKind kind = CheckTaskKind::DISCARD_CHECKED;
};

struct AfterIndexTargetObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_TARGET_OBJECT;
  Expr **target;
  IndexExpr *index;
  AssignForm form;
};

struct AfterGenericTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_TARGET;
  IndexExpr *index;
};

struct AfterTypedArrayPosition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPED_ARRAY_POSITION;
  IndexExpr *index;
};

struct AfterKeyedTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_KEYED_TARGET;
  Expr **target;
  IndexExpr *index;
  AssignForm form;
};

struct AfterArrayPosition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARRAY_POSITION;
  IndexExpr *index;
};

struct AfterMemberTargetObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_TARGET_OBJECT;
  Expr **target;
  MemberExpr *member;
  AssignForm form;
};

struct AfterAssignTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASSIGN_TARGET;
  AssignExpr *assign;
  BinaryOp op;
  AssignForm form;
};

struct AfterInferredField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INFERRED_FIELD;
  AssignExpr *assign;
  AssignPlace place;
};

struct AfterPlainValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PLAIN_VALUE;
  AssignExpr *assign;
  const Type *type;
  AssignPlace place;
};

struct AfterStringCompound {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STRING_COMPOUND;
  AssignExpr *assign;
  BinaryOp op;
  const Type *type;
  AssignPlace place;
};

struct AfterNumericCompound {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NUMERIC_COMPOUND;
  AssignExpr *assign;
  BinaryOp op;
  const Type *type;
};

struct CheckAssignDivisor {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ASSIGN_DIVISOR;
  AssignExpr *assign;
  BinaryOp op;
};

struct CompoundFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::COMPOUND_FACTS;
  AssignExpr *assign;
  BinaryOp op;
  const VarSymbol *variable;
};

struct AfterAssignValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASSIGN_VALUE;
  AssignExpr *assign;
  AssignPlace place;
};

struct AssignedLength {
  static constexpr CheckTaskKind kind = CheckTaskKind::ASSIGNED_LENGTH;
  AssignExpr *assign;
  AssignPlace place;
};

struct LengthFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::LENGTH_FACTS;
  const MemberExpr *member;
  const Expr *value;
};

struct AssignedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::ASSIGNED_FIELD;
  AssignExpr *assign;
  AssignPlace place;
};

struct AfterUpdateTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UPDATE_TARGET;
  UpdateExpr *update;
};

struct AfterMemberDelete {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_DELETE;
  UnaryExpr *unary;
};

struct AfterOtherDelete {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OTHER_DELETE;
  UnaryExpr *unary;
};

struct AfterIndexDeleteObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_DELETE_OBJECT;
  UnaryExpr *unary;
  std::string array;
  std::string position;
};

struct AfterIndexDeleteKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INDEX_DELETE_KEY;
  UnaryExpr *unary;
  std::string array;
  std::string position;
};

struct CheckCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CALL;
  CallExpr *call;
  const Type *expected;
};

struct WalkArgsLoosely {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_ARGS_LOOSELY;
  CallExpr *call;
};

struct CheckArgSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARG_SLOT;
  Expr **slot;
  const Type *to;
};

struct CheckArguments {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARGUMENTS;
  CallExpr *call;
  std::vector<const Type *> params;
  size_t minArgs;
  bool hasRest;
  llvm::SMLoc loc;
  std::vector<bool> checked;
};

struct CheckNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct CheckValueCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_VALUE_CALL;
  CallExpr *call;
  const Type *type;
};

struct CheckDirectCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DIRECT_CALL;
  CallExpr *call;
  FuncSymbol *function;
  llvm::SMLoc loc;
  const Type *expected;
};

struct AfterNodeCallTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NODE_CALL_TRY;
  CallExpr *call;
  const Type *expected;
};

struct AfterResultStaticTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RESULT_STATIC_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterConsoleTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSOLE_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterProcessTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PROCESS_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterBytesStaticTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BYTES_STATIC_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterReceiverObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RECEIVER_OBJECT;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterResultMethodTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RESULT_METHOD_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterDateMethodTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DATE_METHOD_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterMatchReceiver {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MATCH_RECEIVER;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterCommonReceiver {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_COMMON_RECEIVER;
  CallExpr *call;
  MemberExpr *member;
  const Type *receiver;
  const Type *expected;
};

struct AfterSubtleTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SUBTLE_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *receiver;
  const Type *expected;
};

struct AfterClassMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
  bool isProven;
};

struct AfterNamedCallTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NAMED_CALL_TRY;
  NamedCallDraft draft;
};

struct ScopedCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::SCOPED_CALL;
  CallExpr *call;
  NameExpr *name;
  const Type *expected;
};

struct AfterKnownFunctionType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_KNOWN_FUNCTION_TYPE;
  CallExpr *call;
  NameExpr *name;
  VarSymbol *variable;
  const FunctionDecl *known;
};

struct AfterFunctionNameTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FUNCTION_NAME_TRY;
  FunctionNameDraft draft;
};

struct AfterNotCallableName {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NOT_CALLABLE_NAME;
  CallExpr *call;
  NameExpr *name;
};

struct AfterCalleeValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CALLEE_VALUE;
  CallExpr *call;
  const NameExpr *calleeName;
};

struct AfterDirectReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DIRECT_RETURN_TYPE;
  CallExpr *call;
  FuncSymbol *function;
  llvm::SMLoc loc;
};

struct AfterSpreadRest {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SPREAD_REST;
  CallExpr *call;
  size_t fixed;
  const Type *listRest;
};

struct AfterNewLoad {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NEW_LOAD;
  NewExpr *newExpr;
  const Type *expected;
};

struct AfterClassNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_NEW;
  ClassDecl *classDecl;
};

struct AfterArrayElement {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARRAY_ELEMENT;
  NewExpr *newExpr;
};

struct AfterArrayLength {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ARRAY_LENGTH;
  NewExpr *newExpr;
  const Type *element;
};

struct CheckClassExpr {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_EXPR;
  ClassExpr *classExpr;
};

struct CheckClassStmt {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_STMT;
  ClassDecl *classDecl;
};

struct CheckFieldsSet {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FIELDS_SET;
  llvm::SMLoc loc;
};

struct CheckFieldTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FIELD_TARGET;
  MemberExpr *member;
  AssignForm form;
};

struct CheckStaticTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STATIC_TARGET;
  Expr **target;
  ClassDecl *classDecl;
};

struct CheckMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckInstanceOfOperator {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INSTANCE_OF_OPERATOR;
  BinaryExpr *binary;
};

struct CheckThis {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_THIS;
  NameExpr *name;
};

struct CheckStaticMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STATIC_MEMBER;
  MemberExpr *member;
  ClassDecl *classDecl;
  const Type *expected;
};

struct CheckClassMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct CheckOverloadMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_OVERLOAD_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct CheckClassRefMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_REF_MEMBER;
  MemberExpr *member;
};

struct CheckClassStaticCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_STATIC_CALL;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
  const Type *expected;
};

struct CheckSuperCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_SUPER_CALL;
  CallExpr *call;
};

struct CheckSuperMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_SUPER_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckValueNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_VALUE_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct CheckClassNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
};

struct CheckOverloadCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_OVERLOAD_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckClassValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CLASS_VALUE;
  NameExpr *name;
  ClassDecl *classDecl;
};

struct BindOuterTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_OUTER_TYPE_PARAMS;
  const ClassDecl *classDecl;
};

struct DeclareThis {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_THIS;
  FunctionDecl *functionDecl;
};

struct WalkClassBodies {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_CLASS_BODIES;
  ClassDecl *classDecl;
};

struct AfterLateClassExpr {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LATE_CLASS_EXPR;
  ClassExpr *classExpr;
};

struct FinishClassExpr {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_EXPR;
  ClassExpr *classExpr;
};

struct WalkStaticFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_STATIC_FIELDS;
  ClassDecl *classDecl;
};

struct LeaveStaticFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_STATIC_FIELDS;
};

struct WalkStaticField {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_STATIC_FIELD;
  ClassField *field;
};

struct AfterStaticField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STATIC_FIELD;
  ClassField *field;
  const Type *declared;
};

struct DecorateClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECORATE_CLASS;
  ClassDecl *classDecl;
};

struct LeaveClassContext {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_CLASS_CONTEXT;
};

struct AfterLateBase {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LATE_BASE;
  ClassDecl *classDecl;
};

struct LeaveDeclaringClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_DECLARING_CLASS;
};

struct FinishLateClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LATE_CLASS;
  ClassDecl *classDecl;
};

struct WalkClassFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_CLASS_FUNCTION;
  FunctionDecl *functionDecl;
};

struct WalkFieldStarts {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FIELD_STARTS;
  const ClassDecl *classDecl;
};

struct WalkClassMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_CLASS_METHOD;
  FunctionDecl *functionDecl;
};

struct AfterClassMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_METHOD_RETURN;
  FunctionDecl *functionDecl;
};

struct FinishClassBodies {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_BODIES;
  ClassDecl *classDecl;
  KnownReturns returns;
};

struct NextLateClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_LATE_CLASS;
};

struct WalkLateClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_LATE_CLASS;
  ClassDecl *classDecl;
};

struct FinishLateClassWalk {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LATE_CLASS_WALK;
  InstanceRecord *owner;
  InstanceRecord *outerInstance;
};

struct AfterConstructorTypeArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRUCTOR_TYPE_ARGUMENT;
  const TypeRef *ref;
};

struct AfterMethodVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_METHOD_VARIANTS;
  MemberExpr *member;
  ClassDecl *classDecl;
  const Type *expected;
  std::vector<FunctionDecl *> methods;
};

struct AfterMemberFunctionType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_FUNCTION_TYPE;
  MemberExpr *member;
};

struct AfterStaticVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STATIC_VARIANTS;
  MemberExpr *member;
  const Type *expected;
  std::vector<FunctionDecl *> methods;
};

struct AfterNameFunctionType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NAME_FUNCTION_TYPE;
  NameExpr *name;
};

struct AfterClassRefStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_REF_STATIC;
  Expr *object;
};

struct AfterOverloadVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OVERLOAD_VARIANTS;
  MemberExpr *member;
  const Type *expected;
  std::vector<FunctionDecl *> variants;
};

struct AfterInstanceOfLeft {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_OF_LEFT;
  BinaryExpr *binary;
};

struct InstanceOfRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANCE_OF_RIGHT;
  BinaryExpr *binary;
};

struct AfterInstanceOfRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_OF_RIGHT;
  BinaryExpr *binary;
};

struct AfterMethodCallTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_METHOD_CALL_TRY;
  MethodCallDraft draft;
};

struct AfterClassFieldCallee {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_FIELD_CALLEE;
  CallExpr *call;
  MemberExpr *member;
};

struct AfterMethodChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_METHOD_CHOICE;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
  const Type *expected;
  size_t count;
};

struct AfterPlainMethodType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PLAIN_METHOD_TYPE;
  CallExpr *call;
  MemberExpr *member;
  FuncSymbol *function;
};

struct AfterGenericMethodCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_METHOD_CALL;
  CallExpr *call;
  MemberExpr *member;
  bool isSuper;
};

struct SetCallType {
  static constexpr CheckTaskKind kind = CheckTaskKind::SET_CALL_TYPE;
  CallExpr *call;
};

struct StartGenericCallTypes {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_GENERIC_CALL_TYPES;
  CallExpr *call;
  FuncSymbol *function;
  std::vector<const Type *> bound;
  llvm::SMLoc loc;
  GenericCallee kind;
};

struct AfterBoundTypes {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BOUND_TYPES;
  CallExpr *call;
  FuncSymbol *function;
  std::vector<const Type *> bound;
  llvm::SMLoc loc;
  GenericCallee kind;
  bool isReturnKnown;
};

struct AfterGenericCallee {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_CALLEE;
  CallExpr *call;
  GenericCallee kind;
};

struct ChooseVariant {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHOOSE_VARIANT;
  std::vector<FunctionDecl *> variants;
  std::vector<Expr *> *args;
  std::vector<bool> spread;
  GenericVariants generics;
};

struct RankVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::RANK_VARIANTS;
  ChoiceDraft draft;
};

struct NextRankedVariant {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_RANKED_VARIANT;
  ChoiceDraft draft;
};

struct AfterInferredArguments {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INFERRED_ARGUMENTS;
  ChoiceDraft draft;
};

struct AfterGenericParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_PARAMS;
  ChoiceDraft draft;
  std::vector<const Type *> typeArgs;
};

struct FinishSuperCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_SUPER_CALL;
  ClassDecl *classDecl;
};

struct AfterParentConstructorChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PARENT_CONSTRUCTOR_CHOICE;
  CallExpr *call;
  FunctionDecl *first;
};

struct AfterSuperObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SUPER_OBJECT;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *parent;
};

struct AfterSuperEmitterTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SUPER_EMITTER_TRY;
  CallExpr *call;
  MemberExpr *member;
  std::vector<FunctionDecl *> methods;
};

struct AfterSuperMethodChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SUPER_METHOD_CHOICE;
  CallExpr *call;
  MemberExpr *member;
};

struct AfterValueNewCallee {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_VALUE_NEW_CALLEE;
  NewExpr *newExpr;
  const Type *expected;
};

struct NextReflectedArg {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_REFLECTED_ARG;
  ReflectNewDraft draft;
};

struct AfterReflectedArg {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_REFLECTED_ARG;
  ReflectNewDraft draft;
};

struct AfterConstructorChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRUCTOR_CHOICE;
  NewExpr *newExpr;
  CallExpr *call;
  ClassDecl *classDecl;
  FunctionDecl *first;
};

struct FinishClassNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
};

struct AfterOverloadChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OVERLOAD_CHOICE;
  CallExpr *call;
  MemberExpr *member;
  std::vector<FunctionDecl *> variants;
};

struct AfterEventsFunctionTry {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EVENTS_FUNCTION_TRY;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterStaticFieldCallee {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STATIC_FIELD_CALLEE;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
};

struct AfterStaticMethodChoice {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STATIC_METHOD_CHOICE;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct Decorate {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECORATE;
  DecorateRun run;
};

struct NextHoistedDecorator {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_HOISTED_DECORATOR;
  DecorateRun run;
};

struct AfterFactoryMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FACTORY_METHOD_RETURN;
  DecorateRun run;
};

struct AfterDescriptorMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DESCRIPTOR_METHOD_RETURN;
  DecorateRun run;
};

struct DeclareHoistedDecorator {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_HOISTED_DECORATOR;
  VarDeclStmt *varDecl;
};

struct AfterDescriptorDeclared {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DESCRIPTOR_DECLARED;
  DecorateRun run;
  VarDeclStmt *varDecl;
};

struct NextAppliedDecorator {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_APPLIED_DECORATOR;
  DecorateRun run;
};

struct AfterDecoratorApplied {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DECORATOR_APPLIED;
  DecorateRun run;
  ExprStmt *statement;
};

struct RelinkClass {
  static constexpr CheckTaskKind kind = CheckTaskKind::RELINK_CLASS;
  ClassDecl *classDecl;
};

struct DropDiagnosticList {
  static constexpr CheckTaskKind kind = CheckTaskKind::DROP_DIAGNOSTIC_LIST;
};

struct WalkInferredBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_INFERRED_BODY;
  FunctionDecl *functionDecl;
};

struct CheckThisCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_THIS_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckListMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LIST_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckListArguments {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LIST_ARGUMENTS;
  CallExpr *call;
  size_t fixed;
  const Type *listParam;
  llvm::SMLoc loc;
  std::vector<bool> checked;
};

struct WalkStatements {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_STATEMENTS;
  BlockStmt *block;
};

struct NoteUse {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_USE;
  VarSymbol *variable;
  llvm::SMLoc loc;
};

struct FindListPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_LIST_PART;
  Expr *expr;
};

struct CheckFunctionExpr {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FUNCTION_EXPR;
  FunctionExpr *functionExpr;
  std::optional<Signature> expected;
};

struct CheckFunctionValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FUNCTION_VALUE;
  Expr *expr;
  const Type *expected;
  Signature signature;
};

struct WalkFunctionBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FUNCTION_BODY;
  FunctionDecl *functionDecl;
};

struct AfterBodyTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BODY_TYPE_PARAMS;
  FunctionDecl *functionDecl;
  FuncSymbol *outer;
};

struct AfterBodyThis {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BODY_THIS;
  BodyDraft draft;
};

struct NextBodyParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_BODY_PARAM;
  BodyDraft draft;
};

struct AfterDefaultedParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DEFAULTED_PARAM;
  BodyDraft draft;
};

struct AfterBodyStatements {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BODY_STATEMENTS;
  BodyDraft draft;
};

struct EnterErrorWatch {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_ERROR_WATCH;
};

struct LeaveErrorWatch {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_ERROR_WATCH;
};

struct AfterPredicateEnd {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PREDICATE_END;
  const FunctionDecl *functionDecl;
};

struct FinishFunctionBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FUNCTION_BODY;
  BodyDraft draft;
};

struct WalkFunctionFromScratch {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FUNCTION_FROM_SCRATCH;
  FunctionDecl *functionDecl;
};

struct DeclareArgumentList {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_ARGUMENT_LIST;
  FunctionDecl *functionDecl;
};

struct RememberArgumentList {
  static constexpr CheckTaskKind kind = CheckTaskKind::REMEMBER_ARGUMENT_LIST;
  llvm::StringRef name;
  std::vector<llvm::StringRef> parts;
};

struct AfterBlockTypes {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BLOCK_TYPES;
  BlockStmt *block;
};

struct EnterStatements {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_STATEMENTS;
  BlockStmt *block;
  NestedGroup *group;
};

struct NextStatement {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_STATEMENT;
  StatementsDraft draft;
};

struct AfterStatement {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_STATEMENT;
  StatementsDraft draft;
};

struct GroupMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::GROUP_MEMBER;
  FunctionDecl *functionDecl;
};

struct AfterGroupMemberReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GROUP_MEMBER_RETURN;
  FunctionDecl *functionDecl;
};

struct GroupUseOrder {
  static constexpr CheckTaskKind kind = CheckTaskKind::GROUP_USE_ORDER;
  NestedGroup *group;
};

struct LeaveGroup {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_GROUP;
};

struct NextClosureParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CLOSURE_PARAM;
  ClosureDraft draft;
};

struct AfterClosureParamType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLOSURE_PARAM_TYPE;
  ClosureDraft draft;
};

struct AfterClosureReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLOSURE_RETURN_TYPE;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct AfterClosureReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLOSURE_RETURN;
  ClosureDraft draft;
};

struct StartClosureBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLOSURE_BODY;
  FunctionDecl *functionDecl;
};

struct LeaveInitializerClosure {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_INITIALIZER_CLOSURE;
};

struct KeepConstantFlow {
  static constexpr CheckTaskKind kind = CheckTaskKind::KEEP_CONSTANT_FLOW;
  Flow outer;
};

struct DeclareOwnName {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_OWN_NAME;
  FunctionDecl *functionDecl;
};

struct AfterClosureType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLOSURE_TYPE;
  FunctionExpr *functionExpr;
};

struct CheckFunctionArgSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FUNCTION_ARG_SLOT;
  Expr **slot;
  Signature expected;
  unsigned primary;
  llvm::StringRef method;
};

struct AfterLiteralCallback {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_CALLBACK;
  Expr **slot;
  Signature expected;
  llvm::StringRef method;
};

struct AfterCallbackValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CALLBACK_VALUE;
  Expr **slot;
  Signature expected;
  unsigned primary;
  llvm::StringRef method;
};

struct LeaveCallback {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_CALLBACK;
};

struct CheckCallbackMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CALLBACK_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterExtraCallbackArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EXTRA_CALLBACK_ARGS;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct AfterReduceInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_REDUCE_INIT;
  ArrayCallbackShape shape;
  const Type *expected;
};

struct FinishCallbackMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CALLBACK_METHOD;
  ArrayCallbackShape shape;
};

struct CheckArrayCallbackResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARRAY_CALLBACK_RESULT;
  ArrayCallbackShape shape;
};

struct NextListArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_LIST_ARGUMENT;
  ListArgumentsDraft draft;
};

struct AfterListArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LIST_ARGUMENT;
  ListArgumentsDraft draft;
};

struct StartModuleBodies {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_MODULE_BODIES;
  Module *module;
};

struct WalkTopFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_TOP_FUNCTION;
  FunctionDecl *functionDecl;
};

struct AfterTopReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TOP_RETURN_TYPE;
  FunctionDecl *functionDecl;
};

struct StartLiftedFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LIFTED_FUNCTION;
  FunctionDecl *functionDecl;
};

struct CheckYield {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_YIELD;
  YieldExpr *yield;
};

struct FindIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_ITERATOR;
  Expr *expr;
};

struct AsyncUserIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::ASYNC_USER_ITERATOR;
  Expr *expr;
};

struct IterableOfSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::ITERABLE_OF_SLOT;
  Expr **slot;
};

struct ElementTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::ELEMENT_TYPE_OF;
  const Type *type;
};

struct DestructuredIteratorSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::DESTRUCTURED_ITERATOR_SLOT;
  Expr **slot;
  const VarDeclarator *declarator;
};

struct FinishIteratorResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITERATOR_RESULT;
  const TypeRef *ref;
};

struct FinishIteratorArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITERATOR_ARGS;
  const TypeRef *ref;
  bool isAsync;
};

struct FinishIterableParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITERABLE_PARAM;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
  TypeRef *ref;
};

struct AfterYieldValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_YIELD_VALUE;
  YieldExpr *yield;
  YieldShape shape;
};

struct AfterYieldDelegate {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_YIELD_DELEGATE;
  YieldExpr *yield;
  YieldShape shape;
};

struct FinishYield {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_YIELD;
  YieldExpr *yield;
  YieldShape shape;
};

struct AfterIterableUser {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ITERABLE_USER;
  Expr **slot;
};

struct AfterBuiltinIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BUILTIN_ITERATOR;
  Expr *expr;
};

struct UserIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::USER_ITERATOR;
  Expr *expr;
};

struct AfterSymbolIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SYMBOL_ITERATOR;
  Expr *expr;
};

struct AfterResultIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RESULT_ITERATOR;
  Expr *expr;
  Expr *result;
};

struct AfterAsyncSymbolIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASYNC_SYMBOL_ITERATOR;
  Expr *expr;
};

struct IteratorOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::ITERATOR_OF;
  Expr *expr;
};

struct AfterIteratorValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ITERATOR_VALUE;
  Expr *expr;
};

struct BuildIterateCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::BUILD_ITERATE_CALL;
  Expr *expr;
  const Type *value;
};

struct FinishIterateSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITERATE_SYMBOL;
  FunctionDecl *functionDecl;
};

struct FinishIterateCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ITERATE_CALL;
  CallExpr *call;
};

struct NextValueOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_VALUE_OF;
  const Type *type;
};

struct AfterNextMethods {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NEXT_METHODS;
  size_t count;
};

struct AfterSoleIteratorReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SOLE_ITERATOR_RETURN;
};

struct CheckCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONDITION;
  Expr *cond;
};

struct FinishCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONDITION;
};

struct StartBranch {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_BRANCH;
  Expr *expr;
  TruthUse use;
};

struct AfterBranchLeaf {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BRANCH_LEAF;
  TruthUse use;
};

struct FinishBranchLeaf {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_BRANCH_LEAF;
  Expr *checked;
  TruthUse use;
};

struct AfterAssignedLeaf {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASSIGNED_LEAF;
  AssignExpr *assign;
  TruthUse use;
};

struct AfterBranchLeft {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BRANCH_LEFT;
  BinaryExpr *binary;
};

struct AfterBranchRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BRANCH_RIGHT;
  BinaryExpr *binary;
  Flow lhsTrue;
  Flow lhsFalse;
};

struct AfterBranchNot {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BRANCH_NOT;
  UnaryExpr *unary;
};

struct CheckNot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NOT;
  UnaryExpr *unary;
};

struct CheckLogicalOperator {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LOGICAL_OPERATOR;
  BinaryExpr *binary;
  const Type *expected;
};

struct FinishBranchJoin {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_BRANCH_JOIN;
};

struct StartTestedValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TESTED_VALUE;
  Expr *expr;
  const Type *expected;
  TruthUse use;
};

struct AfterTestedLeaf {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TESTED_LEAF;
};

struct StartLogical {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LOGICAL;
  BinaryExpr *binary;
  const Type *expected;
  RightSide side;
  TruthUse use;
};

struct AfterLogicalLeft {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LOGICAL_LEFT;
  LogicalDraft draft;
};

struct StartLogicalRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_LOGICAL_RIGHT;
  LogicalDraft draft;
};

struct FinishTestedLogical {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TESTED_LOGICAL;
  LogicalDraft draft;
};

struct FinishValueLogical {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_VALUE_LOGICAL;
  LogicalDraft draft;
};

struct FinishLogicalType {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LOGICAL_TYPE;
  LogicalDraft draft;
};

struct StartNullish {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_NULLISH;
  BinaryExpr *binary;
  const Type *expected;
};

struct AfterNullishLeft {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NULLISH_LEFT;
  BinaryExpr *binary;
  const Type *expected;
};

struct StartNullishRight {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_NULLISH_RIGHT;
  BinaryExpr *binary;
  const Type *expected;
};

struct FinishNullish {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_NULLISH;
  BinaryExpr *binary;
  Flow afterLeft;
};

struct Convert {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONVERT;
  const Type *to;
};

struct Coerce {
  static constexpr CheckTaskKind kind = CheckTaskKind::COERCE;
  const Type *to;
  std::string what;
};

struct CoerceSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::COERCE_SLOT;
  Expr **slot;
  const Type *to;
  std::string what;
};

struct CoerceIteratorFound {
  static constexpr CheckTaskKind kind = CheckTaskKind::COERCE_ITERATOR_FOUND;
  Expr **slot;
  const Type *to;
  std::string what;
};

struct CoerceAsyncIteratorFound {
  static constexpr CheckTaskKind kind = CheckTaskKind::COERCE_ASYNC_ITERATOR_FOUND;
  Expr **slot;
  const Type *to;
  std::string what;
};

struct ApparentSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::APPARENT_SLOT;
  Expr **slot;
};

struct ConvertSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CONVERT_SLOT;
  Expr **slot;
  const Type *to;
};

struct AdaptLiteralSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADAPT_LITERAL_SLOT;
  Expr **slot;
  const Type *to;
};

struct CheckDecimalLiteralRange {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DECIMAL_LITERAL_RANGE;
  FloatLitExpr *literal;
};

struct CheckRegexLiteral {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REGEX_LITERAL;
  RegexLitExpr *literal;
};

struct CheckAwait {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_AWAIT;
  AwaitExpr *await;
  const Type *expected;
};

struct CheckUsedValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_USED_VALUE;
  Expr *expr;
};

struct WalkRedundantTests {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_REDUNDANT_TESTS;
  BinaryExpr *binary;
};

struct MatchItems {
  static constexpr CheckTaskKind kind = CheckTaskKind::MATCH_ITEMS;
};

struct WalkComparisonOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_COMPARISON_OPERANDS;
  BinaryExpr *binary;
};

struct NeverOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEVER_OPERANDS;
  BinaryExpr *binary;
};

struct CheckDateOperands {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_OPERANDS;
  BinaryExpr *binary;
};

struct CheckDecimalComparison {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DECIMAL_COMPARISON;
  BinaryExpr *binary;
};

struct CheckNullEquality {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NULL_EQUALITY;
  BinaryExpr *binary;
};

struct CheckJsonEquality {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_EQUALITY;
  BinaryExpr *binary;
};

struct ReportDecimalMix {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_DECIMAL_MIX;
  BinaryExpr *binary;
};

struct CheckDecimalArithmetic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DECIMAL_ARITHMETIC;
  BinaryExpr *binary;
};

struct DateToTime {
  static constexpr CheckTaskKind kind = CheckTaskKind::DATE_TO_TIME;
};

struct RegexToString {
  static constexpr CheckTaskKind kind = CheckTaskKind::REGEX_TO_STRING;
};

struct DateToString {
  static constexpr CheckTaskKind kind = CheckTaskKind::DATE_TO_STRING;
};

struct BytesToString {
  static constexpr CheckTaskKind kind = CheckTaskKind::BYTES_TO_STRING;
};

struct NoteReflectedConversion {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_REFLECTED_CONVERSION;
  const Type *from;
  const Type *to;
  CastExpr *castExpr;
};

struct NoteConstructorFlow {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_CONSTRUCTOR_FLOW;
  const Type *from;
  const Type *to;
};

struct CheckNodeName {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NODE_NAME;
  NameExpr *name;
};

struct FunctionTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::FUNCTION_TYPE_OF;
  FuncSymbol *function;
};

struct NoteHttpsValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_HTTPS_VALUE;
  const FunctionDecl *functionDecl;
  llvm::SMLoc loc;
};

struct CheckJsonMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_MEMBER;
  MemberExpr *member;
};

struct CheckResultMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RESULT_MEMBER;
  MemberExpr *member;
};

struct CheckRegexMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REGEX_MEMBER;
  MemberExpr *member;
};

struct CheckMatchMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_MATCH_MEMBER;
  MemberExpr *member;
};

struct CheckDateMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_MEMBER;
  MemberExpr *member;
};

struct CheckBytesMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_MEMBER;
  MemberExpr *member;
};

struct CheckCryptoMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CRYPTO_MEMBER;
  MemberExpr *member;
};

struct CheckRuntimeMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RUNTIME_MEMBER;
  MemberExpr *member;
};

struct CheckRegExpStaticMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REG_EXP_STATIC_MEMBER;
  MemberExpr *member;
};

struct CheckWebCryptoMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_WEB_CRYPTO_MEMBER;
  MemberExpr *member;
};

struct CheckNodeMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NODE_MEMBER;
  MemberExpr *member;
};

struct CheckProcessMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROCESS_MEMBER;
  MemberExpr *member;
};

struct CheckBytesStaticMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_STATIC_MEMBER;
  MemberExpr *member;
};

struct CheckIntlMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INTL_MEMBER;
  MemberExpr *member;
  const Type *expected;
};

struct CheckMatchIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_MATCH_INDEX;
  IndexExpr *index;
};

struct CheckJsonIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_INDEX;
  IndexExpr *index;
};

struct CheckBytesIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_INDEX;
  IndexExpr *index;
};

struct CheckProcessEnvIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROCESS_ENV_INDEX;
  IndexExpr *index;
};

struct CheckBytesUnionMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_UNION_MEMBER;
  MemberExpr *member;
};

struct ProcessAliasSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::PROCESS_ALIAS_SLOT;
  Expr **slot;
};

struct MarkFused {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_FUSED;
  Expr *expr;
};

struct ResolveWeakRefType {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESOLVE_WEAK_REF_TYPE;
  const TypeRef *ref;
};

struct SegmentsArraySlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::SEGMENTS_ARRAY_SLOT;
  Expr **slot;
};

struct NoteInheritedReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_INHERITED_RETURN;
  const FunctionDecl *functionDecl;
};

struct CheckUsedCallbackValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_USED_CALLBACK_VALUE;
  Expr *expr;
};

struct WalkForAwait {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FOR_AWAIT;
  ForOfStmt *forOf;
};

struct NoteErrorsHandled {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_ERRORS_HANDLED;
  const Expr *expr;
};

struct FindCallCycles {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_CALL_CYCLES;
  std::vector<FunctionDecl *> functions;
};

struct CheckProcessAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROCESS_ASSIGN;
  AssignExpr *assign;
};

struct CheckJsonTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_TARGET;
  Expr *target;
};

struct CheckRuntimeTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RUNTIME_TARGET;
  MemberExpr *member;
};

struct WalkDecimalAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_DECIMAL_ASSIGN;
  AssignExpr *assign;
  BinaryOp op;
};

struct WalkBytesStore {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_BYTES_STORE;
  IndexExpr *index;
  const Expr *value;
};

struct CheckNodeCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NODE_CALL;
  CallExpr *call;
  const Type *expected;
};

struct CheckPromiseStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROMISE_STATIC;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckResultStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RESULT_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckJsonStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckArrayIsArray {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARRAY_IS_ARRAY;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckJsonIsJson {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_IS_JSON;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckReflectCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REFLECT_CALL;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckConsoleCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONSOLE_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckProcessCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROCESS_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckBitCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BIT_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckMathCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_MATH_CALL;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckRegExpStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REG_EXP_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckIntlCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INTL_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckLocaleStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LOCALE_STATIC;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckIntlStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INTL_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckDecimalStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DECIMAL_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckX509Parse {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_X509_PARSE;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckWebCryptoCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_WEB_CRYPTO_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckBytesStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckNumberStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NUMBER_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckObjectCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_OBJECT_CALL;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckIteratorFrom {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ITERATOR_FROM;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckArrayFrom {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARRAY_FROM;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckMapStatic {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_MAP_STATIC;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckRuntimeMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RUNTIME_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckResultMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RESULT_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckJsonMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckRegexMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REGEX_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckDateMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckDateFormatMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_FORMAT_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckIntlMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INTL_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckCryptoMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CRYPTO_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckWeakRefMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_WEAK_REF_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckBytesMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckPromiseMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROMISE_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckTimerMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_TIMER_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckSubtleCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_SUBTLE_CALL;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct WalkHeadersCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_HEADERS_CALL;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckStringLocale {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STRING_LOCALE;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckNumberLocale {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NUMBER_LOCALE;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckDecimalMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DECIMAL_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckIteratorMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ITERATOR_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckDictMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckStringMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STRING_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckArrayMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ARRAY_METHOD;
  CallExpr *call;
  MemberExpr *member;
  const Type *expected;
};

struct CheckNumberMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_NUMBER_METHOD;
  CallExpr *call;
  MemberExpr *member;
};

struct CheckLibCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LIB_CALL;
  CallExpr *call;
  NameExpr *name;
};

struct CheckResultCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RESULT_CALL;
  CallExpr *call;
  NameExpr *name;
  const Type *expected;
};

struct CheckJsonGlobal {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_GLOBAL;
  CallExpr *call;
  NameExpr *name;
};

struct CheckAsyncGlobal {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_ASYNC_GLOBAL;
  CallExpr *call;
  NameExpr *name;
};

struct CheckGlobalCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_GLOBAL_CALL;
  CallExpr *call;
  NameExpr *name;
};

struct CheckEventsLibraryCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EVENTS_LIBRARY_CALL;
  CallExpr *call;
  NameExpr *name;
  FuncSymbol *function;
};

struct CheckStreamLibraryCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STREAM_LIBRARY_CALL;
  CallExpr *call;
  NameExpr *name;
  FuncSymbol *function;
};

struct NoteHttpsCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_HTTPS_CALL;
  CallExpr *call;
  const FunctionDecl *functionDecl;
};

struct CheckGenericCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_GENERIC_CALL;
  CallExpr *call;
  FuncSymbol *function;
  llvm::SMLoc loc;
  const Type *expected;
  llvm::ArrayRef<TypeRef> typeArgs;
};

struct CheckCryptoNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CRYPTO_NEW;
  NewExpr *newExpr;
  llvm::StringRef name;
};

struct CheckLocaleNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_LOCALE_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct CheckDateFormatNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_FORMAT_NEW;
  NewExpr *newExpr;
};

struct CheckIntlNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_INTL_NEW;
  NewExpr *newExpr;
};

struct NoteLibraryInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_LIBRARY_INSTANCE;
  NewExpr *made;
  const ClassDecl *classDecl;
};

struct CheckRegExpConstructor {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_REG_EXP_CONSTRUCTOR;
  NewExpr *newExpr;
};

struct CheckDateNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DATE_NEW;
  NewExpr *newExpr;
};

struct CheckPromiseNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_PROMISE_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct CheckWeakRefNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_WEAK_REF_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct CheckBytesNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTES_NEW;
  NewExpr *newExpr;
  const Type *bytes;
};

struct FindMethodCycles {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_METHOD_CYCLES;
  std::vector<FunctionDecl *> methods;
};

struct CheckJsonObjectLit {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_OBJECT_LIT;
  ObjectLitExpr *literal;
};

struct CheckJsonArrayLit {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JSON_ARRAY_LIT;
  ArrayLitExpr *literal;
};

struct WalkExternal {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_EXTERNAL;
  FunctionDecl *functionDecl;
};

struct EnterReturnBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_RETURN_BODY;
  FunctionDecl *functionDecl;
};

struct LeaveReturnBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_RETURN_BODY;
};

struct InferFromBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_FROM_BODY;
  FunctionDecl *functionDecl;
};

struct InferCycle {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_CYCLE;
  std::vector<FuncSymbol *> cycle;
};

struct EnterHomeModule {
  static constexpr CheckTaskKind kind = CheckTaskKind::ENTER_HOME_MODULE;
  const FunctionDecl *functionDecl;
};

struct NoteTlsMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_TLS_METHOD;
  const MemberExpr *member;
  const ClassDecl *classDecl;
};

struct CheckStatsDate {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STATS_DATE;
  MemberExpr *member;
  ClassDecl *classDecl;
};

struct ReportEmitterMethodValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_EMITTER_METHOD_VALUE;
  MemberExpr *member;
};

struct CheckFsClassMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_FS_CLASS_METHOD;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
};

struct CheckStreamCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STREAM_CALL;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
};

struct CheckEmitterCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EMITTER_CALL;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
  CallReceiver receiver;
};

struct CheckEventTargetCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EVENT_TARGET_CALL;
  CallExpr *call;
  MemberExpr *member;
  ClassDecl *classDecl;
};

struct CheckEventsFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EVENTS_FUNCTION;
  CallExpr *call;
  llvm::StringRef name;
  llvm::SMLoc loc;
};

struct CheckStreamFrom {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STREAM_FROM;
  CallExpr *call;
  MemberExpr *member;
};

struct WriteDesignRecords {
  static constexpr CheckTaskKind kind = CheckTaskKind::WRITE_DESIGN_RECORDS;
  ClassDecl *classDecl;
  ClassField *field;
  FunctionDecl *method;
  const FunctionDecl *constructor;
  llvm::SMLoc loc;
};

struct BuildVtable {
  static constexpr CheckTaskKind kind = CheckTaskKind::BUILD_VTABLE;
  ClassDecl *classDecl;
};

struct NoteRegexVariable {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_REGEX_VARIABLE;
  const VarSymbol *variable;
  const Expr *value;
  bool isConst;
  llvm::ArrayRef<Stmt *> block;
  size_t blockIndex;
};

struct LeaveBranch {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_BRANCH;
};

struct MarkUnreachable {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_UNREACHABLE;
};

struct AfterExprStmt {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EXPR_STMT;
  ExprStmt *statement;
};

struct FinishExprStmt {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_EXPR_STMT;
  ExprStmt *statement;
};

struct WalkBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_BODY;
  Stmt *body;
};

struct WalkVarDecl {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_VAR_DECL;
  VarDeclStmt *varDecl;
};

struct CloseScope {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_SCOPE;
};

struct NextDeclarator {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_DECLARATOR;
  DeclaratorDraft draft;
};

struct AfterDeclaredType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DECLARED_TYPE;
  DeclaratorDraft draft;
};

struct AfterConstFunctionSymbol {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONST_FUNCTION_SYMBOL;
  DeclaratorDraft draft;
  FunctionExpr *functionExpr;
};

struct FinishConstFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONST_FUNCTION;
  DeclaratorDraft draft;
};

struct AfterEarlyTimer {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EARLY_TIMER;
  DeclaratorDraft draft;
};

struct FindDestination {
  static constexpr CheckTaskKind kind = CheckTaskKind::FIND_DESTINATION;
  DeclaratorDraft draft;
  DestinationTypes types;
};

struct AfterInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INIT;
  DeclaratorDraft draft;
};

struct DestructureValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::DESTRUCTURE_VALUE;
  DeclaratorDraft draft;
};

struct AfterDestructuredIterator {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DESTRUCTURED_ITERATOR;
  DeclaratorDraft draft;
};

struct RememberValueDeclarator {
  static constexpr CheckTaskKind kind = CheckTaskKind::REMEMBER_VALUE_DECLARATOR;
  DeclaratorDraft draft;
};

struct WalkIf {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_IF;
  IfStmt *ifStmt;
};

struct WalkSwitch {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_SWITCH;
  SwitchStmt *switchStmt;
};

struct AfterIfCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_IF_CONDITION;
  IfStmt *ifStmt;
};

struct AfterThenBranch {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_THEN_BRANCH;
  IfStmt *ifStmt;
  Flow whenFalse;
};

struct FinishIf {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_IF;
  Flow afterThen;
};

struct AfterSwitchValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_SWITCH_VALUE;
  SwitchStmt *switchStmt;
};

struct NextCaseTest {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CASE_TEST;
  SwitchDraft draft;
};

struct AfterCaseTest {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CASE_TEST;
  SwitchDraft draft;
};

struct AddCaseFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_CASE_FACTS;
  SwitchStmt *switchStmt;
  size_t index;
  const Type *type;
};

struct FinishConstantCase {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONSTANT_CASE;
  SwitchCase *switchCase;
};

struct FinishCaseTest {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CASE_TEST;
  SwitchDraft draft;
};

struct NextCaseBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CASE_BODY;
  SwitchDraft draft;
};

struct WalkCaseStatement {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_CASE_STATEMENT;
  size_t position;
};

struct AfterCaseBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CASE_BODY;
  SwitchDraft draft;
};

struct FinishSwitch {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_SWITCH;
  Flow exit;
};

struct DeclareBlockTypes {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_BLOCK_TYPES;
  std::vector<Stmt *> stmts;
};

struct DeclareMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_METHOD_RETURN;
  FunctionDecl *functionDecl;
};

struct FinishMethodReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_METHOD_RETURN;
  FunctionDecl *functionDecl;
};

struct DeclareVariableSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_VARIABLE_SLOT;
  VarSymbol **slot;
  llvm::StringRef name;
  llvm::SMLoc loc;
  const Type *type;
  bool isConst;
};

struct DeclareVariable {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_VARIABLE;
  llvm::StringRef name;
  llvm::SMLoc loc;
  const Type *type;
  bool isConst;
};

struct WalkWhile {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_WHILE;
  WhileStmt *whileStmt;
};

struct WalkDoWhile {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_DO_WHILE;
  DoWhileStmt *doWhile;
};

struct WalkFor {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FOR;
  ForStmt *forStmt;
};

struct WalkForOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FOR_OF;
  ForOfStmt *forOf;
};

struct WalkLabeled {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_LABELED;
  LabeledStmt *labeled;
};

struct CheckJump {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_JUMP;
  Stmt *stmt;
};

struct FinishLabeledBlock {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_LABELED_BLOCK;
};

struct LeaveLoopBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_LOOP_BODY;
  bool isAwait;
};

struct RestoreLoopFlow {
  static constexpr CheckTaskKind kind = CheckTaskKind::RESTORE_LOOP_FLOW;
  Flow flow;
};

struct AfterWhileCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_WHILE_CONDITION;
  WhileStmt *whileStmt;
  LoopScan scan;
  Flow head;
};

struct AfterDoWhileBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DO_WHILE_BODY;
  DoWhileStmt *doWhile;
  LoopScan scan;
  Flow head;
};

struct AfterDoWhileCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DO_WHILE_CONDITION;
  DoWhileStmt *doWhile;
  LoopScan scan;
  Flow head;
};

struct AfterForInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_INIT;
  ForStmt *forStmt;
  llvm::SmallVector<llvm::StringRef, 1> labels;
};

struct AfterForCondition {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_CONDITION;
  ForDraft draft;
};

struct AfterForBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_BODY;
  ForDraft draft;
};

struct AfterForStep {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_STEP;
  ForStmt *forStmt;
};

struct FinishFor {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FOR;
  ForDraft draft;
};

struct AfterForAwait {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_AWAIT;
  ForOfStmt *forOf;
};

struct AfterForOfSource {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_OF_SOURCE;
  ForOfStmt *forOf;
};

struct AfterIterable {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ITERABLE;
  ForOfStmt *forOf;
};

struct AfterForInObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_IN_OBJECT;
  ForOfStmt *forOf;
  const NameExpr *variable;
  bool isEnv;
};

struct BindForOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_FOR_OF;
  ForOfStmt *forOf;
};

struct AfterForOfBinding {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FOR_OF_BINDING;
  ForOfStmt *forOf;
  const Type *element;
  LoopScan scan;
  Flow head;
};

struct WalkReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_RETURN;
  ReturnStmt *returnStmt;
};

struct AfterUnlistenedErrors {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_UNLISTENED_ERRORS;
  ReturnStmt *returnStmt;
  const FuncSymbol *function;
};

struct AfterReturnValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_RETURN_VALUE;
  ReturnStmt *returnStmt;
  const FuncSymbol *function;
  const Type *returned;
};

struct AfterTestedReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TESTED_RETURN;
  ReturnStmt *returnStmt;
};

struct AfterPredicateReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PREDICATE_RETURN;
  ReturnStmt *returnStmt;
};

struct AfterPredicateFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PREDICATE_FACTS;
  ReturnStmt *returnStmt;
};

struct AfterValueCoerce {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_VALUE_COERCE;
  ReturnStmt *returnStmt;
  const FuncSymbol *function;
};

struct AfterConstructorValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRUCTOR_VALUE;
  ReturnStmt *returnStmt;
  const FuncSymbol *function;
};

struct AfterInferredReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INFERRED_RETURN;
  ReturnStmt *returnStmt;
};

struct AfterAsyncGeneratorReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASYNC_GENERATOR_RETURN;
  ReturnStmt *returnStmt;
  const FuncSymbol *function;
  const Type *returned;
};

struct CheckDictIn {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_IN;
  BinaryExpr *binary;
};

struct CheckDictMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_MEMBER;
  MemberExpr *member;
};

struct CheckDictIndex {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_INDEX;
  IndexExpr *index;
};

struct CheckRecordKeySlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RECORD_KEY_SLOT;
  Expr **slot;
};

struct FinishRecordKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_RECORD_KEY;
  Expr **slot;
};

struct FinishDictIndexRead {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_INDEX_READ;
  IndexExpr *index;
};

struct FinishDictIndexError {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_INDEX_ERROR;
  IndexExpr *index;
};

struct CheckDictTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_TARGET;
  Expr *target;
  AssignForm form;
};

struct FinishDictTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_TARGET;
  Expr *target;
  AssignForm form;
};

struct CheckRecordNullishAssign {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_RECORD_NULLISH_ASSIGN;
  AssignExpr *assign;
  IndexExpr *index;
};

struct AfterNullishRecordObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NULLISH_RECORD_OBJECT;
  AssignExpr *assign;
  IndexExpr *index;
};

struct AfterNullishRecordKey {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NULLISH_RECORD_KEY;
  AssignExpr *assign;
  IndexExpr *index;
};

struct FinishNullishRecord {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_NULLISH_RECORD;
  AssignExpr *assign;
  IndexExpr *index;
  Flow before;
};

struct CheckDictDelete {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_DELETE;
  UnaryExpr *unary;
};

struct FinishDictDelete {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_DELETE;
  UnaryExpr *unary;
};

struct FinishDictTypeAnswer {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_TYPE_ANSWER;
  const TypeRef *ref;
  Collection collection;
};

struct CheckDictNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_DICT_NEW;
  NewExpr *newExpr;
  const Type *expected;
};

struct AfterDictNewTypeArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DICT_NEW_TYPE_ARGS;
  NewExpr *newExpr;
  Collection collection;
};

struct NextPairPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_PAIR_PART;
  PairsDraft draft;
};

struct AfterPairPart {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PAIR_PART;
  PairsDraft draft;
};

struct FinishPairsDict {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_PAIRS_DICT;
  NewExpr *newExpr;
  const Type *key;
  const Type *value;
};

struct AfterDictSource {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DICT_SOURCE;
  NewExpr *newExpr;
  const Type *type;
  bool isMap;
};

struct FinishDictSource {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_DICT_SOURCE;
  NewExpr *newExpr;
  const Type *type;
  bool isMap;
};

struct CalledFunction {
  static constexpr CheckTaskKind kind = CheckTaskKind::CALLED_FUNCTION;
};

struct NoteAssigned {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_ASSIGNED;
  const VarSymbol *variable;
  const Expr *value;
};

struct NoteIncremented {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_INCREMENTED;
  const VarSymbol *variable;
  int64_t step;
};

struct NoteArrayResized {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_ARRAY_RESIZED;
  const Type *array;
  Resize resize;
  const VarSymbol *except;
};

struct BeginPredicate {
  static constexpr CheckTaskKind kind = CheckTaskKind::BEGIN_PREDICATE;
  FunctionDecl *functionDecl;
};

struct EndPredicate {
  static constexpr CheckTaskKind kind = CheckTaskKind::END_PREDICATE;
  FunctionDecl *functionDecl;
};

struct NotePredicateExit {
  static constexpr CheckTaskKind kind = CheckTaskKind::NOTE_PREDICATE_EXIT;
  const Expr *value;
  Flow whenTrue;
  Flow whenFalse;
};

struct BuildPredicate {
  static constexpr CheckTaskKind kind = CheckTaskKind::BUILD_PREDICATE;
  const FuncSymbol *function;
};

struct StartPredicateCopy {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_PREDICATE_COPY;
  FunctionDecl *functionDecl;
};

struct AddByteCallbackFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_BYTE_CALLBACK_FACTS;
  FunctionDecl *functionDecl;
};

struct AddIndexCallbackFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_INDEX_CALLBACK_FACTS;
  FunctionDecl *functionDecl;
};

struct CheckByteCallbackResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BYTE_CALLBACK_RESULT;
  const FunctionDecl *functionDecl;
  Expr *value;
};

struct AddConditionFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_CONDITION_FACTS;
  const Expr *cond;
};

struct FinishConditionFacts {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CONDITION_FACTS;
  const Expr *cond;
};

struct WalkFreshCopy {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_FRESH_COPY;
  FunctionDecl *functionDecl;
};

struct WalkBodyNow {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_BODY_NOW;
  FunctionDecl *functionDecl;
};

struct FinishFilterResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FILTER_RESULT;
  ArrayCallbackShape shape;
};

struct FinishFilterDefined {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_FILTER_DEFINED;
  ArrayCallbackShape shape;
};

struct SubstituteType {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_TYPE;
  const Type *type;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
};

struct SubstituteValueType {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_VALUE_TYPE;
  const Type *type;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
};

struct SubstituteNode {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_NODE;
  const Type *type;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
  VoidAs voidAs;
};

struct BuildSubstitutedType {
  static constexpr CheckTaskKind kind = CheckTaskKind::BUILD_SUBSTITUTED_TYPE;
  const Type *type;
  SubstituteForm form;
  size_t count;
};

struct InstanceType {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANCE_TYPE;
  const ClassDecl *instance;
  const Type *type;
};

struct InstanceValueType {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANCE_VALUE_TYPE;
  const ClassDecl *instance;
  const Type *type;
};

struct SubstituteInstanceFields {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_INSTANCE_FIELDS;
  ClassDecl *instance;
};

struct NextInstanceField {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INSTANCE_FIELD;
  ClassDecl *instance;
  size_t index;
};

struct AfterInstanceField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_FIELD;
  ClassDecl *instance;
  size_t index;
};

struct SubstituteTypeList {
  static constexpr CheckTaskKind kind = CheckTaskKind::SUBSTITUTE_TYPE_LIST;
  TypeListDraft draft;
};

struct AfterTypeListItem {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_LIST_ITEM;
  TypeListDraft draft;
};

struct StartGenericValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_GENERIC_VALUE;
  NameExpr *name;
  FuncSymbol *generic;
  std::optional<Signature> wanted;
};

struct CheckGenericValue {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_GENERIC_VALUE;
  NameExpr *name;
  FuncSymbol *generic;
  std::optional<Signature> wanted;
};

struct NextValueTypeArg {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_VALUE_TYPE_ARG;
  GenericValueDraft draft;
  size_t index;
};

struct AfterValueTypeArg {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_VALUE_TYPE_ARG;
  GenericValueDraft draft;
  size_t index;
};

struct AfterGenericValueArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_VALUE_ARGS;
  GenericValueDraft draft;
};

struct AfterGenericValueType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_VALUE_TYPE;
  GenericValueDraft draft;
};

struct InstantiateCallTarget {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_CALL_TARGET;
  CallExpr *call;
  FuncSymbol *generic;
  std::vector<const Type *> typeArgs;
  llvm::SMLoc loc;
  std::vector<const Type *> exact;
  bool isCopyingReturn;
};

struct AfterReusedWalk {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_REUSED_WALK;
  InstantiateDraft draft;
};

struct NextMeaningfulParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_MEANINGFUL_PARAM;
  InstantiateDraft draft;
};

struct AfterMeaningfulParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEANINGFUL_PARAM;
  InstantiateDraft draft;
};

struct AfterInstanceTypes {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_TYPES;
  InstantiateDraft draft;
};

struct AfterNewInstanceWalk {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_NEW_INSTANCE_WALK;
  InstantiateDraft draft;
};

struct WalkPreciseCopy {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_PRECISE_COPY;
  InstanceRecord *instance;
};

struct CheckConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONSTRAINT;
  const Type *arg;
  const Type *param;
  std::vector<const Type *> params;
  std::vector<const Type *> args;
};

struct AfterConstraintSubstitution {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRAINT_SUBSTITUTION;
  const Type *arg;
  const Type *param;
};

struct EvaluateConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::EVALUATE_CONSTRAINT;
  ConstraintCheck check;
};

struct AfterConstraintMethod {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRAINT_METHOD;
  ConstraintCheck check;
  FuncSymbol *function;
};

struct InstantiateOwnAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_OWN_ALIAS;
  TypeAliasDecl *alias;
};

struct AfterOwnAliasParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_OWN_ALIAS_PARAMS;
  TypeAliasDecl *alias;
};

struct DropOwnAliasAnswer {
  static constexpr CheckTaskKind kind = CheckTaskKind::DROP_OWN_ALIAS_ANSWER;
};

struct InstantiateAlias {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_ALIAS;
  TypeAliasDecl *alias;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
};

struct AfterAliasParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_PARAMS;
  AliasDraft draft;
};

struct AfterAliasDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_DEFAULT;
  AliasDraft draft;
};

struct AfterAliasMatch {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_MATCH;
  AliasDraft draft;
};

struct AfterAliasObject {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_OBJECT;
  AliasDraft draft;
};

struct AfterAliasInner {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_INNER;
  AliasDraft draft;
};

struct CloseAliasBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_ALIAS_BODY;
  AliasDraft draft;
  const Type *type;
};

struct DeclareClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_CLASS_TYPE_PARAMS;
  ClassDecl *classDecl;
};

struct AfterClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CLASS_TYPE_PARAMS;
  ClassDecl *classDecl;
  Module *here;
  bool isMoved;
};

struct LeaveClassTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_CLASS_TYPE_PARAMS;
  ClassDecl *classDecl;
  std::vector<const Type *> defaults;
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

struct AfterFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FUNCTION_TYPE_PARAMS;
  FunctionDecl *functionDecl;
  FuncSymbol *function;
};

struct BindFunctionTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_FUNCTION_TYPE_PARAMS;
  const FunctionDecl *functionDecl;
};

struct DeclareAliasTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_ALIAS_TYPE_PARAMS;
  TypeAliasDecl *alias;
};

struct AfterAliasTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ALIAS_TYPE_PARAMS;
  TypeAliasDecl *alias;
};

struct StartTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_TYPE_PARAMS;
  llvm::ArrayRef<TypeParamDecl> decls;
  bool isScoped;
};

struct NextTypeParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TYPE_PARAM;
  TypeParamsDraft draft;
};

struct AfterConstraintType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRAINT_TYPE;
  TypeParamsDraft draft;
};

struct NextConstraintMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CONSTRAINT_MEMBER;
  TypeParamsDraft draft;
};

struct AfterConstraintMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONSTRAINT_MEMBER;
  TypeParamsDraft draft;
};

struct AfterDefaultType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DEFAULT_TYPE;
  TypeParamsDraft draft;
};

struct AfterDefaultConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DEFAULT_CONSTRAINT;
  TypeParamsDraft draft;
  const Type *fallback;
};

struct CheckGenericNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_GENERIC_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
};

struct AfterEmitterNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EMITTER_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
};

struct NextExplicitNewArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_EXPLICIT_NEW_ARGUMENT;
  ExplicitNewDraft draft;
};

struct AfterExplicitNewArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EXPLICIT_NEW_ARGUMENT;
  ExplicitNewDraft draft;
};

struct AfterExplicitInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EXPLICIT_INSTANCE;
  ExplicitNewDraft draft;
};

struct AfterGenericNewBinding {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_NEW_BINDING;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  CallExpr *call;
  FunctionDecl *constructor;
  FuncSymbol *function;
};

struct AfterGenericNewInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_GENERIC_NEW_INSTANCE;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  CallExpr *call;
  FunctionDecl *constructor;
  FuncSymbol *function;
};

struct InstantiateClassType {
  static constexpr CheckTaskKind kind = CheckTaskKind::INSTANTIATE_CLASS_TYPE;
  ClassDecl *classDecl;
  std::vector<const Type *> args;
  llvm::SMLoc loc;
};

struct AfterInstanceTypeParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_TYPE_PARAMS;
  ClassInstanceDraft draft;
};

struct NextInstanceDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INSTANCE_DEFAULT;
  ClassInstanceDraft draft;
};

struct AfterInstanceDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_DEFAULT;
  ClassInstanceDraft draft;
};

struct NextInstanceArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INSTANCE_ARGUMENT;
  ClassInstanceDraft draft;
};

struct AfterInstanceArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_ARGUMENT;
  ClassInstanceDraft draft;
};

struct FinishClassInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_CLASS_INSTANCE;
  ClassDecl *instance;
};

struct WalkOrPostpone {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_OR_POSTPONE;
  const ClassDecl *instance;
  llvm::SMLoc loc;
};

struct AfterInstanceWalk {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_WALK;
  InstanceRecord *check;
  llvm::SMLoc loc;
};

struct FillInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::FILL_INSTANCE;
  ClassDecl *instance;
};

struct LeaveFilling {
  static constexpr CheckTaskKind kind = CheckTaskKind::LEAVE_FILLING;
};

struct LinkInstanceBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::LINK_INSTANCE_BASES;
  ClassDecl *instance;
};

struct AfterInstanceParent {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_PARENT;
  ClassDecl *instance;
};

struct NextInstanceInterface {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INSTANCE_INTERFACE;
  ClassDecl *instance;
  size_t index;
};

struct AfterInstanceInterface {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_INTERFACE;
  ClassDecl *instance;
  size_t index;
};

struct NextFilledField {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_FILLED_FIELD;
  ClassDecl *instance;
  size_t index;
};

struct AfterFilledField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FILLED_FIELD;
  ClassDecl *instance;
  size_t index;
};

struct DeclareInstanceMembers {
  static constexpr CheckTaskKind kind = CheckTaskKind::DECLARE_INSTANCE_MEMBERS;
  ClassDecl *instance;
};

struct NextInstanceMember {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_INSTANCE_MEMBER;
  InstanceMembersDraft draft;
};

struct AfterInstanceInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_INIT;
  InstanceMembersDraft draft;
};

struct NextIteratedParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_ITERATED_PARAM;
  InstanceMembersDraft draft;
};

struct AfterIteratedParam {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ITERATED_PARAM;
  InstanceMembersDraft draft;
};

struct AfterMemberSignature {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_SIGNATURE;
  InstanceMembersDraft draft;
};

struct NextMemberType {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_MEMBER_TYPE;
  MemberTypesDraft draft;
};

struct AfterMemberInstanceType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_INSTANCE_TYPE;
  MemberTypesDraft draft;
};

struct AfterMemberType {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_MEMBER_TYPE;
  MemberTypesDraft draft;
};

struct WalkInstanceVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_INSTANCE_VARIANTS;
  ClassDecl *instance;
};

struct AfterInstanceVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INSTANCE_VARIANTS;
  ClassDecl *instance;
};

struct LinkInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::LINK_INSTANCE;
  ClassDecl *instance;
};

struct MarkOverridden {
  static constexpr CheckTaskKind kind = CheckTaskKind::MARK_OVERRIDDEN;
  FunctionDecl *functionDecl;
};

struct ForceConversion {
  static constexpr CheckTaskKind kind = CheckTaskKind::FORCE_CONVERSION;
  const Type *to;
};

struct WalkInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_INSTANCE;
  InstanceRecord *instance;
};

struct WalkPreciseBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_PRECISE_BODY;
  InstanceRecord *instance;
  FunctionDecl *copy;
};

struct WalkTemplateInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_TEMPLATE_INSTANCE;
  InstanceRecord *instance;
};

struct StartCopyBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_COPY_BODY;
  InstanceRecord *instance;
  FunctionDecl *copy;
  CopyWalk walk;
};

struct PopCopyDiagnostics {
  static constexpr CheckTaskKind kind = CheckTaskKind::POP_COPY_DIAGNOSTICS;
  InstanceRecord *instance;
  CopyWalk walk;
};

struct FinishCopyWalk {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_COPY_WALK;
  InstanceRecord *instance;
  CopyWalk walk;
  std::vector<Diagnostic> diagnostics;
};

struct ReportInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_INSTANCE;
  InstanceRecord *instance;
};

struct ReportInstanceAt {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_INSTANCE_AT;
  InstanceRecord *instance;
  llvm::SMLoc loc;
  std::vector<const Type *> args;
};

struct NextPendingInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_PENDING_INSTANCE;
  int index;
};

struct ReportFailedPending {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_FAILED_PENDING;
  InstanceRecord *instance;
};

struct WalkClassInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_CLASS_INSTANCE;
  InstanceRecord *check;
};

struct NextClassBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CLASS_BODY;
  ClassBodiesDraft draft;
};

struct StartClassBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_CLASS_BODY;
  ClassBodiesDraft draft;
};

struct ClassBodyEntered {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLASS_BODY_ENTERED;
  ClassBodiesDraft draft;
};

struct PopClassBodyDiagnostics {
  static constexpr CheckTaskKind kind = CheckTaskKind::POP_CLASS_BODY_DIAGNOSTICS;
  ClassBodiesDraft draft;
};

struct ReportClassInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::REPORT_CLASS_INSTANCE;
  InstanceRecord *check;
  llvm::SMLoc loc;
};

struct FinishTypeArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TYPE_ARGS;
  FuncSymbol *generic;
  std::vector<const Type *> bound;
  llvm::SMLoc loc;
  TypeArgUse use;
  MissingTypeArgs missing;
};

struct BindGenericCall {
  static constexpr CheckTaskKind kind = CheckTaskKind::BIND_GENERIC_CALL;
  CallExpr *call;
  FuncSymbol *function;
  llvm::SMLoc loc;
  const Type *expected;
  llvm::ArrayRef<TypeRef> typeArgs;
};

struct StartBinding {
  static constexpr CheckTaskKind kind = CheckTaskKind::START_BINDING;
  CallExpr *call;
  FuncSymbol *generic;
  llvm::SMLoc loc;
  const Type *expected;
  llvm::ArrayRef<TypeRef> typeArgs;
  BindingOwner owner;
};

struct InferFromArguments {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_FROM_ARGUMENTS;
  FuncSymbol *function;
  std::vector<Expr *> args;
};

struct NextBindingArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_BINDING_ARGUMENT;
  GenericBindingDraft draft;
};

struct InferFromArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_FROM_ARGUMENT;
  GenericBindingDraft draft;
  size_t index;
  const Type *actual;
  std::optional<ArgumentStrength> strength;
};

struct NextPrecheckedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_PRECHECKED_FIELD;
  GenericBindingDraft draft;
  ObjectLitExpr *object;
  size_t argument;
  size_t property;
};

struct AfterPrecheckedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_PRECHECKED_FIELD;
  GenericBindingDraft draft;
  ObjectLitExpr *object;
  size_t argument;
  size_t property;
};

struct InferPrecheckedField {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_PRECHECKED_FIELD;
  GenericBindingDraft draft;
  ObjectLitExpr *object;
  size_t argument;
  size_t property;
};

struct InferFromListRest {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_FROM_LIST_REST;
  GenericBindingDraft draft;
};

struct RetryDeferred {
  static constexpr CheckTaskKind kind = CheckTaskKind::RETRY_DEFERRED;
  GenericBindingDraft draft;
};

struct NextContextualArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CONTEXTUAL_ARGUMENT;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterContextualPartial {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONTEXTUAL_PARTIAL;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterDefaultedHint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_DEFAULTED_HINT;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterContextualArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CONTEXTUAL_ARGUMENT;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterExplicitTypeArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_EXPLICIT_TYPE_ARGS;
  GenericBindingDraft draft;
  size_t count;
};

struct AfterBindingTypeArgs {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BINDING_TYPE_ARGS;
  GenericBindingDraft draft;
};

struct AfterBoundParams {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BOUND_PARAMS;
  GenericBindingDraft draft;
};

struct NextLiteralArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_LITERAL_ARGUMENT;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterLiteralHint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_LITERAL_HINT;
  GenericBindingDraft draft;
  size_t position;
};

struct AfterBoundResult {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BOUND_RESULT;
  GenericBindingDraft draft;
};

struct AfterBoundArguments {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BOUND_ARGUMENTS;
  GenericBindingDraft draft;
};

struct NextTypeArgCheck {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_TYPE_ARG_CHECK;
  TypeArgsDraft draft;
};

struct AfterTypeArgDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_ARG_DEFAULT;
  TypeArgsDraft draft;
};

struct AfterIteratedArgFit {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ITERATED_ARG_FIT;
  TypeArgsDraft draft;
};

struct AfterTypeArgConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TYPE_ARG_CONSTRAINT;
  TypeArgsDraft draft;
};

struct InferArgumentPatterns {
  static constexpr CheckTaskKind kind = CheckTaskKind::INFER_ARGUMENT_PATTERNS;
  InferredArgumentsDraft draft;
};

struct NextFittingArgument {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_FITTING_ARGUMENT;
  InferredArgumentsDraft draft;
};

struct AfterFittingDefault {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FITTING_DEFAULT;
  InferredArgumentsDraft draft;
};

struct AfterFittingConstraint {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_FITTING_CONSTRAINT;
  InferredArgumentsDraft draft;
};

struct JoinInstances {
  static constexpr CheckTaskKind kind = CheckTaskKind::JOIN_INSTANCES;
};

struct AfterJoinedInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_JOINED_INSTANCE;
  Expr *expr;
};

struct CheckEmitterNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_EMITTER_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
};

struct CheckStreamNew {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_STREAM_NEW;
  NewExpr *newExpr;
  ClassDecl *classDecl;
  const Type *expected;
};

struct AddStreamBases {
  static constexpr CheckTaskKind kind = CheckTaskKind::ADD_STREAM_BASES;
  ClassDecl *instance;
  llvm::SMLoc loc;
};

struct WalkVariants {
  static constexpr CheckTaskKind kind = CheckTaskKind::WALK_VARIANTS;
  ClassDecl *classDecl;
};

struct BuildInstanceInit {
  static constexpr CheckTaskKind kind = CheckTaskKind::BUILD_INSTANCE_INIT;
  ClassDecl *instance;
};

struct CheckConstructorAccess {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_CONSTRUCTOR_ACCESS;
  NewExpr *newExpr;
  const FunctionDecl *constructor;
};

struct CheckBuiltinCallbackSlot {
  static constexpr CheckTaskKind kind = CheckTaskKind::CHECK_BUILTIN_CALLBACK_SLOT;
  Expr **slot;
  Signature expected;
};

struct AsyncIterableTypeOf {
  static constexpr CheckTaskKind kind = CheckTaskKind::ASYNC_ITERABLE_TYPE_OF;
  const Type *type;
};

struct AfterAsyncIterableReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_ASYNC_ITERABLE_RETURN;
};

struct AnswerReturnType {
  static constexpr CheckTaskKind kind = CheckTaskKind::ANSWER_RETURN_TYPE;
  FuncSymbol *function;
};

struct AnswerFunctionType {
  static constexpr CheckTaskKind kind = CheckTaskKind::ANSWER_FUNCTION_TYPE;
  FuncSymbol *function;
};

struct AfterTemplateReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TEMPLATE_RETURN;
  FuncSymbol *function;
};

struct AfterTemplateInstance {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_TEMPLATE_INSTANCE;
  FuncSymbol *function;
};

struct SetInferredReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::SET_INFERRED_RETURN;
  FuncSymbol *function;
};

struct AfterInheritedReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_INHERITED_RETURN;
  FuncSymbol *function;
};

struct OpenBodyCheck {
  static constexpr CheckTaskKind kind = CheckTaskKind::OPEN_BODY_CHECK;
  FuncSymbol *function;
  InstanceRecord *lateOwner;
};

struct CloseBodyCheck {
  static constexpr CheckTaskKind kind = CheckTaskKind::CLOSE_BODY_CHECK;
  InstanceRecord *lateOwner;
};

struct FinishTopLevelEntry {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TOP_LEVEL_ENTRY;
  const FunctionDecl *functionDecl;
};

struct FinishInferFromBody {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_INFER_FROM_BODY;
  FunctionDecl *functionDecl;
  std::vector<ReturnStmt *> *outerReturns;
  std::vector<YieldExpr *> *outerYields;
};

struct NextCycleTrial {
  static constexpr CheckTaskKind kind = CheckTaskKind::NEXT_CYCLE_TRIAL;
  CycleDraft draft;
};

struct AfterCycleTrial {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_CYCLE_TRIAL;
  CycleDraft draft;
};

struct FinishTrialReturn {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_TRIAL_RETURN;
  FuncSymbol *function;
  FunctionDecl *copy;
};

struct AfterBodyNowInference {
  static constexpr CheckTaskKind kind = CheckTaskKind::AFTER_BODY_NOW_INFERENCE;
  FunctionDecl *functionDecl;
};

struct FinishAsyncReturns {
  static constexpr CheckTaskKind kind = CheckTaskKind::FINISH_ASYNC_RETURNS;
  FuncSymbol *function;
  std::vector<ReturnStmt *> returns;
};

using CheckTask = Union<UseModule, InstantiateEmitterParent, FindStreamHookParams, WalkHookParams, WalkIterableParam, WalkGeneratorReturn, WalkAsyncReturn, WalkStmt, ReportUnlistenedErrors, EnterLoadBody, LeaveLoadBody, ImportBuiltinModule, FindIntlType, ResolveResultType, ResolveIteratorType, ResolveDictType, ResolvePromiseType, MakeMethodDescriptor, ResolveConstructorType, CheckConstructorArgs, ClassValueType, LiteralShape, CreatedType, InferReturnType, ReturnTypeOf, ResolveTypeRef, ResolveInnerTypeRef, ResolveObjectFields, WriteResolvedType, ResolveTypeAnswer, FinishTypeUse, FinishArrayType, NextUnionMember, AfterUnionMember, FinishAliasType, FinishEnumType, FinishEnumMember, FinishIntlType, NextTypeArgument, AfterTypeArgument, FinishDescriptor, FinishInstanceType, NextUtilityOperand, AfterUtilityOperand, AfterUtilitySignature, NextFunctionParam, AfterFunctionParam, FinishFunctionType, FinishAssertsType, FinishTupleType, NextObjectField, AfterObjectField, AfterObjectSignature, EvalTypeOp, EvalKeyOf, EvalIndexed, EvalSpread, StartKeyOf, AfterKeyOfOperand, AfterKeyOfEval, StartIndexed, AfterIndexedOperands, AfterIndexedEval, EvalMapped, NextMappedKey, AfterMappedValue, AfterMappedRename, StartMapped, AfterMappedKeys, AfterMappedSource, AfterMappedParts, StartIntersection, NextIntersectionPart, AfterIntersectionPart, StartConditional, AfterConditionalCheck, AfterInferConstraint, AfterConditionalPattern, ChooseBranch, CloseConditionalScope, AfterDistributedMember, EvalConditional, AfterEvalConditional, EvalFiltered, FinishFiltered, PrepareExpectation, FinishBranches, StartTemplate, AfterTemplateHole, FinishTemplate, EvalTemplate, EvalStringCase, EvalFunctionPart, StartTypeOf, FindTypeOfCandidates, AfterTypeOfCandidate, AfterTypeOfMember, AfterFunctionValue, FinishDeclaredFunctionValue, SignatureForUtility, ContinueSignature, AfterErasedReturn, ResolveImport, StartTopLevel, NextTopLevelItem, FinishTopLevel, CloseTopLevel, StartLoadEarly, LoadEarlyBody, FinishLoadEarly, BindLoadedNames, ResolveAlias, StartAlias, EnterAlias, FinishAlias, ResolveEnum, EnterEnum, NextEnumMember, StartFunctionSymbol, FinishFunctionSymbol, StartFunctionParams, StartFunctionParam, FinishFunctionParam, StartFunctionReturn, FinishFunctionReturn, StartReturnType, FinishAssertsReturn, FinishPredicateReturn, ResolveClass, OpenClassScope, CloseClassScope, ResolveClassMembers, StartClassField, FinishClassField, StartClassBases, FinishClassBases, FinishParentClass, FinishLinkParent, FinishImplemented, AddBase, DeclareMethods, FinishConstructor, FinishClassMethod, StartFieldMethod, FinishFieldMethod, StartTypedMethod, FinishTypedParam, FinishTypedMethod, FinishMethods, NextFieldDefault, ContinueFieldDefault, MakeFieldInit, FinishFieldInit, FinishVariants, FinishItemMethods, CompleteClass, FinishCompletion, StartInterfaceFields, StartInterfaceBase, FinishInterfaceBase, FinishInterfaceFields, ReportVariantPair, StartLinkClasses, FinishLinkClasses, StartLinkOverrides, FinishLinkOverrides, LinkOverride, StartDeclarationFile, FinishDeclarationFile, StartDeclaredAlias, FinishDeclaredAlias, StartDeclaredFunction, StartDeclaredClass, FinishDeclaredClass, StartDeclaredMember, FinishDeclaredField, StartDeclaredParam, FinishDeclaredParam, FinishDeclaredReturn, CheckExpr, CheckExprSlot, WriteCheckedSlot, ReportError, CheckExprKind, AfterConditionTest, AfterElseFirst, AfterThenSecond, AfterThenFirst, AfterElseSecond, FinishConditional, JoinConditionalBranches, AfterCastTarget, FinishCast, NextSequenceItem, AfterSequenceItem, FinishSequence, FinishOptionalChain, CheckIntLiteral, CheckTemplate, CheckArrayLiteral, CheckObjectLiteral, NextTemplatePart, AfterTemplatePart, AfterTemplateType, AfterComputedKey, AfterUnionNeeds, NextInferredKey, AfterInferredKey, NextKeyedProperty, NextNamedProperty, AfterUntargetedField, NextRecordProperty, AfterRecordSpread, NextSpreadProperty, AfterNamedProp, AfterSpreadProp, NextLiteralMember, AfterLiteralField, AfterLiteralMethods, WalkLiteralMethods, WalkLiteralMethod, AfterLiteralMethodReturn, MatchLiteralTarget, FinishPairLiteral, NextArrayElement, AfterLiteralElement, AfterSpreadElement, AfterSpreadMatch, CheckEnumObject, AfterEnumObjectResolved, RememberEnumObject, CheckField, CheckFieldCall, AfterFieldCallee, CheckObjectFieldTarget, CheckKeyedField, AfterTupleKey, AfterObjectKey, ObjectKeyReady, CheckName, LookupName, VariableTypeKnown, FinishVariableName, FinishFunctionName, ConstText, ToInt32, NoteUnsignedVar, CheckShift, CheckArithmetic, AfterArithmeticRight, AfterArithmeticLeft, AfterArithmeticOperands, FinishArithmetic, ResolveArithmetic, ArithmeticResult, FinishIntegerArithmetic, CheckWrappingDivisor, StartBits, FinishBits, FoldBinaryNode, CheckSign, FinishSign, SignOperandReady, CheckBitNot, FinishBitNot, CheckConcatenation, TrackMixedConcat, ToStringSlot, ToStringOperand, MarkTextCast, CheckMember, AfterListPart, AfterNodeMember, AfterProcessMember, AfterBytesStaticMember, AfterMemberObject, MemberOfCommonBase, AfterEnumResolved, CheckTupleMember, CheckTupleMethod, FinishTupleMethod, ReportTupleJoin, FinishTupleJoin, CheckUnionMember, CheckUnionFieldTarget, CheckUnionMethodCall, ResolveUnionMethodCall, ReportUnionMethodCall, CheckUnknownMember, CheckUnknownMethod, FinishUnknownStaticCall, FinishUnknownMethod, CheckIndex, CheckPosition, FinishPosition, AfterIndexListPart, AfterProcessEnvIndex, AfterIndexObject, FinishGenericIndex, WriteIndexType, FinishUnknownIndex, ReportUndefinedIndex, FinishPositionIndex, AfterEnumIndexResolved, FinishEnumIndex, EnumIndexValue, CheckUnary, CheckBinary, FinishTypeOf, FinishVoid, FinishNonNull, CheckInOperator, AfterInOperands, CheckEquality, AfterEqualityOperands, FinishEqualOperands, CheckComparison, AfterComparisonOperands, FinishComparison, CompareOperands, FinishNumberComparison, WalkTypeOfName, CheckAssign, CheckUpdate, CheckDeleteValue, AfterTargetAlias, AfterProcessAssign, AfterLogicalAssign, AfterNullishHolder, AfterRecordFieldNullish, AfterIndexNullish, AfterFieldNullish, AfterNullishVariable, FinishNullishVariable, FinishVarTarget, VarTargetTypeKnown, AnswerVarTarget, DiscardChecked, AfterIndexTargetObject, AfterGenericTarget, AfterTypedArrayPosition, AfterKeyedTarget, AfterArrayPosition, AfterMemberTargetObject, AfterAssignTarget, AfterInferredField, AfterPlainValue, AfterStringCompound, AfterNumericCompound, CheckAssignDivisor, CompoundFacts, AfterAssignValue, AssignedLength, LengthFacts, AssignedField, AfterUpdateTarget, AfterMemberDelete, AfterOtherDelete, AfterIndexDeleteObject, AfterIndexDeleteKey, CheckCall, WalkArgsLoosely, CheckArgSlot, CheckArguments, CheckNew, CheckValueCall, CheckDirectCall, AfterNodeCallTry, AfterResultStaticTry, AfterConsoleTry, AfterProcessTry, AfterBytesStaticTry, AfterReceiverObject, AfterResultMethodTry, AfterDateMethodTry, AfterMatchReceiver, AfterCommonReceiver, AfterSubtleTry, AfterClassMethodCall, AfterNamedCallTry, ScopedCall, AfterKnownFunctionType, AfterFunctionNameTry, AfterNotCallableName, AfterCalleeValue, AfterDirectReturnType, AfterSpreadRest, AfterNewLoad, AfterClassNew, AfterArrayElement, AfterArrayLength, CheckClassExpr, CheckClassStmt, CheckFieldsSet, CheckFieldTarget, CheckStaticTarget, CheckMethodCall, CheckInstanceOfOperator, CheckThis, CheckStaticMember, CheckClassMember, CheckOverloadMember, CheckClassRefMember, CheckClassStaticCall, CheckSuperCall, CheckSuperMethodCall, CheckValueNew, CheckClassNew, CheckOverloadCall, CheckClassValue, BindOuterTypeParams, DeclareThis, WalkClassBodies, AfterLateClassExpr, FinishClassExpr, WalkStaticFields, LeaveStaticFields, WalkStaticField, AfterStaticField, DecorateClass, LeaveClassContext, AfterLateBase, LeaveDeclaringClass, FinishLateClass, WalkClassFunction, WalkFieldStarts, WalkClassMethod, AfterClassMethodReturn, FinishClassBodies, NextLateClass, WalkLateClass, FinishLateClassWalk, AfterConstructorTypeArgument, AfterMethodVariants, AfterMemberFunctionType, AfterStaticVariants, AfterNameFunctionType, AfterClassRefStatic, AfterOverloadVariants, AfterInstanceOfLeft, InstanceOfRight, AfterInstanceOfRight, AfterMethodCallTry, AfterClassFieldCallee, AfterMethodChoice, AfterPlainMethodType, AfterGenericMethodCall, SetCallType, StartGenericCallTypes, AfterBoundTypes, AfterGenericCallee, ChooseVariant, RankVariants, NextRankedVariant, AfterInferredArguments, AfterGenericParams, FinishSuperCall, AfterParentConstructorChoice, AfterSuperObject, AfterSuperEmitterTry, AfterSuperMethodChoice, AfterValueNewCallee, NextReflectedArg, AfterReflectedArg, AfterConstructorChoice, FinishClassNew, AfterOverloadChoice, AfterEventsFunctionTry, AfterStaticFieldCallee, AfterStaticMethodChoice, Decorate, NextHoistedDecorator, AfterFactoryMethodReturn, AfterDescriptorMethodReturn, DeclareHoistedDecorator, AfterDescriptorDeclared, NextAppliedDecorator, AfterDecoratorApplied, RelinkClass, DropDiagnosticList, WalkInferredBody, CheckThisCall, CheckListMethod, CheckListArguments, WalkStatements, NoteUse, FindListPart, CheckFunctionExpr, CheckFunctionValue, WalkFunctionBody, AfterBodyTypeParams, AfterBodyThis, NextBodyParam, AfterDefaultedParam, AfterBodyStatements, EnterErrorWatch, LeaveErrorWatch, AfterPredicateEnd, FinishFunctionBody, WalkFunctionFromScratch, DeclareArgumentList, RememberArgumentList, AfterBlockTypes, EnterStatements, NextStatement, AfterStatement, GroupMember, AfterGroupMemberReturn, GroupUseOrder, LeaveGroup, NextClosureParam, AfterClosureParamType, AfterClosureReturnType, AfterClosureReturn, StartClosureBody, LeaveInitializerClosure, KeepConstantFlow, DeclareOwnName, AfterClosureType, CheckFunctionArgSlot, AfterLiteralCallback, AfterCallbackValue, LeaveCallback, CheckCallbackMethod, AfterExtraCallbackArgs, AfterReduceInit, FinishCallbackMethod, CheckArrayCallbackResult, NextListArgument, AfterListArgument, StartModuleBodies, WalkTopFunction, AfterTopReturnType, StartLiftedFunction, CheckYield, FindIterator, AsyncUserIterator, IterableOfSlot, ElementTypeOf, DestructuredIteratorSlot, FinishIteratorResult, FinishIteratorArgs, FinishIterableParam, AfterYieldValue, AfterYieldDelegate, FinishYield, AfterIterableUser, AfterBuiltinIterator, UserIterator, AfterSymbolIterator, AfterResultIterator, AfterAsyncSymbolIterator, IteratorOf, AfterIteratorValue, BuildIterateCall, FinishIterateSymbol, FinishIterateCall, NextValueOf, AfterNextMethods, AfterSoleIteratorReturn, CheckCondition, FinishCondition, StartBranch, AfterBranchLeaf, FinishBranchLeaf, AfterAssignedLeaf, AfterBranchLeft, AfterBranchRight, AfterBranchNot, CheckNot, CheckLogicalOperator, FinishBranchJoin, StartTestedValue, AfterTestedLeaf, StartLogical, AfterLogicalLeft, StartLogicalRight, FinishTestedLogical, FinishValueLogical, FinishLogicalType, StartNullish, AfterNullishLeft, StartNullishRight, FinishNullish, Convert, Coerce, CoerceSlot, CoerceIteratorFound, CoerceAsyncIteratorFound, ApparentSlot, ConvertSlot, AdaptLiteralSlot, CheckDecimalLiteralRange, CheckRegexLiteral, CheckAwait, CheckUsedValue, WalkRedundantTests, MatchItems, WalkComparisonOperands, NeverOperands, CheckDateOperands, CheckDecimalComparison, CheckNullEquality, CheckJsonEquality, ReportDecimalMix, CheckDecimalArithmetic, DateToTime, RegexToString, DateToString, BytesToString, NoteReflectedConversion, NoteConstructorFlow, CheckNodeName, FunctionTypeOf, NoteHttpsValue, CheckJsonMember, CheckResultMember, CheckRegexMember, CheckMatchMember, CheckDateMember, CheckBytesMember, CheckCryptoMember, CheckRuntimeMember, CheckRegExpStaticMember, CheckWebCryptoMember, CheckNodeMember, CheckProcessMember, CheckBytesStaticMember, CheckIntlMember, CheckMatchIndex, CheckJsonIndex, CheckBytesIndex, CheckProcessEnvIndex, CheckBytesUnionMember, ProcessAliasSlot, MarkFused, ResolveWeakRefType, SegmentsArraySlot, NoteInheritedReturn, CheckUsedCallbackValue, WalkForAwait, NoteErrorsHandled, FindCallCycles, CheckProcessAssign, CheckJsonTarget, CheckRuntimeTarget, WalkDecimalAssign, WalkBytesStore, CheckNodeCall, CheckPromiseStatic, CheckResultStatic, CheckJsonStatic, CheckArrayIsArray, CheckJsonIsJson, CheckReflectCall, CheckConsoleCall, CheckProcessCall, CheckBitCall, CheckMathCall, CheckRegExpStatic, CheckIntlCall, CheckLocaleStatic, CheckIntlStatic, CheckDecimalStatic, CheckX509Parse, CheckWebCryptoCall, CheckBytesStatic, CheckNumberStatic, CheckObjectCall, CheckIteratorFrom, CheckArrayFrom, CheckMapStatic, CheckRuntimeMethod, CheckResultMethod, CheckJsonMethod, CheckRegexMethod, CheckDateMethod, CheckDateFormatMethod, CheckIntlMethod, CheckCryptoMethod, CheckWeakRefMethod, CheckBytesMethod, CheckPromiseMethod, CheckTimerMethod, CheckSubtleCall, WalkHeadersCall, CheckStringLocale, CheckNumberLocale, CheckDecimalMethod, CheckIteratorMethod, CheckDictMethod, CheckStringMethod, CheckArrayMethod, CheckNumberMethod, CheckLibCall, CheckResultCall, CheckJsonGlobal, CheckAsyncGlobal, CheckGlobalCall, CheckEventsLibraryCall, CheckStreamLibraryCall, NoteHttpsCall, CheckGenericCall, CheckCryptoNew, CheckLocaleNew, CheckDateFormatNew, CheckIntlNew, NoteLibraryInstance, CheckRegExpConstructor, CheckDateNew, CheckPromiseNew, CheckWeakRefNew, CheckBytesNew, FindMethodCycles, CheckJsonObjectLit, CheckJsonArrayLit, WalkExternal, EnterReturnBody, LeaveReturnBody, InferFromBody, InferCycle, EnterHomeModule, NoteTlsMethod, CheckStatsDate, ReportEmitterMethodValue, CheckFsClassMethod, CheckStreamCall, CheckEmitterCall, CheckEventTargetCall, CheckEventsFunction, CheckStreamFrom, WriteDesignRecords, BuildVtable, NoteRegexVariable, LeaveBranch, MarkUnreachable, AfterExprStmt, FinishExprStmt, WalkBody, WalkVarDecl, CloseScope, NextDeclarator, AfterDeclaredType, AfterConstFunctionSymbol, FinishConstFunction, AfterEarlyTimer, FindDestination, AfterInit, DestructureValue, AfterDestructuredIterator, RememberValueDeclarator, WalkIf, WalkSwitch, AfterIfCondition, AfterThenBranch, FinishIf, AfterSwitchValue, NextCaseTest, AfterCaseTest, AddCaseFacts, FinishConstantCase, FinishCaseTest, NextCaseBody, WalkCaseStatement, AfterCaseBody, FinishSwitch, DeclareBlockTypes, DeclareMethodReturn, FinishMethodReturn, DeclareVariableSlot, DeclareVariable, WalkWhile, WalkDoWhile, WalkFor, WalkForOf, WalkLabeled, CheckJump, FinishLabeledBlock, LeaveLoopBody, RestoreLoopFlow, AfterWhileCondition, AfterDoWhileBody, AfterDoWhileCondition, AfterForInit, AfterForCondition, AfterForBody, AfterForStep, FinishFor, AfterForAwait, AfterForOfSource, AfterIterable, AfterForInObject, BindForOf, AfterForOfBinding, WalkReturn, AfterUnlistenedErrors, AfterReturnValue, AfterTestedReturn, AfterPredicateReturn, AfterPredicateFacts, AfterValueCoerce, AfterConstructorValue, AfterInferredReturn, AfterAsyncGeneratorReturn, CheckDictIn, CheckDictMember, CheckDictIndex, CheckRecordKeySlot, FinishRecordKey, FinishDictIndexRead, FinishDictIndexError, CheckDictTarget, FinishDictTarget, CheckRecordNullishAssign, AfterNullishRecordObject, AfterNullishRecordKey, FinishNullishRecord, CheckDictDelete, FinishDictDelete, FinishDictTypeAnswer, CheckDictNew, AfterDictNewTypeArgs, NextPairPart, AfterPairPart, FinishPairsDict, AfterDictSource, FinishDictSource, CalledFunction, NoteAssigned, NoteIncremented, NoteArrayResized, BeginPredicate, EndPredicate, NotePredicateExit, BuildPredicate, StartPredicateCopy, AddByteCallbackFacts, AddIndexCallbackFacts, CheckByteCallbackResult, AddConditionFacts, FinishConditionFacts, WalkFreshCopy, WalkBodyNow, FinishFilterResult, FinishFilterDefined, SubstituteType, SubstituteValueType, SubstituteNode, BuildSubstitutedType, InstanceType, InstanceValueType, SubstituteInstanceFields, NextInstanceField, AfterInstanceField, SubstituteTypeList, AfterTypeListItem, StartGenericValue, CheckGenericValue, NextValueTypeArg, AfterValueTypeArg, AfterGenericValueArgs, AfterGenericValueType, InstantiateCallTarget, AfterReusedWalk, NextMeaningfulParam, AfterMeaningfulParam, AfterInstanceTypes, AfterNewInstanceWalk, WalkPreciseCopy, CheckConstraint, AfterConstraintSubstitution, EvaluateConstraint, AfterConstraintMethod, InstantiateOwnAlias, AfterOwnAliasParams, DropOwnAliasAnswer, InstantiateAlias, AfterAliasParams, AfterAliasDefault, AfterAliasMatch, AfterAliasObject, AfterAliasInner, CloseAliasBody, DeclareClassTypeParams, AfterClassTypeParams, LeaveClassTypeParams, BindClassTypeParams, DeclareFunctionTypeParams, AfterFunctionTypeParams, BindFunctionTypeParams, DeclareAliasTypeParams, AfterAliasTypeParams, StartTypeParams, NextTypeParam, AfterConstraintType, NextConstraintMember, AfterConstraintMember, AfterDefaultType, AfterDefaultConstraint, CheckGenericNew, AfterEmitterNew, NextExplicitNewArgument, AfterExplicitNewArgument, AfterExplicitInstance, AfterGenericNewBinding, AfterGenericNewInstance, InstantiateClassType, AfterInstanceTypeParams, NextInstanceDefault, AfterInstanceDefault, NextInstanceArgument, AfterInstanceArgument, FinishClassInstance, WalkOrPostpone, AfterInstanceWalk, FillInstance, LeaveFilling, LinkInstanceBases, AfterInstanceParent, NextInstanceInterface, AfterInstanceInterface, NextFilledField, AfterFilledField, DeclareInstanceMembers, NextInstanceMember, AfterInstanceInit, NextIteratedParam, AfterIteratedParam, AfterMemberSignature, NextMemberType, AfterMemberInstanceType, AfterMemberType, WalkInstanceVariants, AfterInstanceVariants, LinkInstance, MarkOverridden, ForceConversion, WalkInstance, WalkPreciseBody, WalkTemplateInstance, StartCopyBody, PopCopyDiagnostics, FinishCopyWalk, ReportInstance, ReportInstanceAt, NextPendingInstance, ReportFailedPending, WalkClassInstance, NextClassBody, StartClassBody, ClassBodyEntered, PopClassBodyDiagnostics, ReportClassInstance, FinishTypeArgs, BindGenericCall, StartBinding, InferFromArguments, NextBindingArgument, InferFromArgument, NextPrecheckedField, AfterPrecheckedField, InferPrecheckedField, InferFromListRest, RetryDeferred, NextContextualArgument, AfterContextualPartial, AfterDefaultedHint, AfterContextualArgument, AfterExplicitTypeArgs, AfterBindingTypeArgs, AfterBoundParams, NextLiteralArgument, AfterLiteralHint, AfterBoundResult, AfterBoundArguments, NextTypeArgCheck, AfterTypeArgDefault, AfterIteratedArgFit, AfterTypeArgConstraint, InferArgumentPatterns, NextFittingArgument, AfterFittingDefault, AfterFittingConstraint, JoinInstances, AfterJoinedInstance, CheckEmitterNew, CheckStreamNew, AddStreamBases, WalkVariants, BuildInstanceInit, CheckConstructorAccess, CheckBuiltinCallbackSlot, AsyncIterableTypeOf, AfterAsyncIterableReturn, AnswerReturnType, AnswerFunctionType, AfterTemplateReturn, AfterTemplateInstance, SetInferredReturn, AfterInheritedReturn, OpenBodyCheck, CloseBodyCheck, FinishTopLevelEntry, FinishInferFromBody, NextCycleTrial, AfterCycleTrial, FinishTrialReturn, AfterBodyNowInference, FinishAsyncReturns>;

}
