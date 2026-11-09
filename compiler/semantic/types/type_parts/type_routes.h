#pragma once

#include "compiler/program/ast/type_ref/type_ref.h"
#include "compiler/semantic/types/type_parts/type_parts.h"

namespace tz {

inline constexpr TypeEdge kMentionsRoute[] = {
    TypeEdge::OPTIONAL,  TypeEdge::ARRAY,    TypeEdge::PROMISE,        TypeEdge::RESULT,
    TypeEdge::FUNCTION,  TypeEdge::ITERATOR, TypeEdge::ASYNC_ITERATOR, TypeEdge::DICT,
    TypeEdge::UNION,     TypeEdge::FIELDS,   TypeEdge::CLASS_FIELDS,   TypeEdge::TUPLE_REST,
    TypeEdge::TYPE_ARGS, TypeEdge::OPERANDS, TypeEdge::CONSTRUCTED,
};

inline constexpr TypeEdge kMentionsInObjectRoute[] = {
    TypeEdge::OPTIONAL,        TypeEdge::ARRAY, TypeEdge::RESULT, TypeEdge::FUNCTION,
    TypeEdge::ITERATOR_VALUES, TypeEdge::DICT,  TypeEdge::UNION,
};

inline constexpr TypeEdge kHoldsRoute[] = {
    TypeEdge::OPTIONAL, TypeEdge::ARRAY,  TypeEdge::PROMISE,      TypeEdge::DICT,
    TypeEdge::UNION,    TypeEdge::FIELDS, TypeEdge::CLASS_FIELDS,
};

inline constexpr TypeEdge kLiteralClassRoute[] = {
    TypeEdge::OPTIONAL, TypeEdge::ARRAY,    TypeEdge::PROMISE, TypeEdge::RESULT,
    TypeEdge::FUNCTION, TypeEdge::ITERATOR, TypeEdge::DICT,    TypeEdge::FIELDS,
};

inline constexpr TypeEdge kWideIntegerRoute[] = {
    TypeEdge::OPTIONAL,
    TypeEdge::UNION,
};

inline constexpr TypeEdge kToJsonRoute[] = {
    TypeEdge::OPTIONAL,     TypeEdge::ARRAY,  TypeEdge::RESULT,
    TypeEdge::RECORD_VALUE, TypeEdge::FIELDS, TypeEdge::CLASS_FIELDS,
};

inline constexpr TypeEdge kReflectClassesRoute[] = {
    TypeEdge::OPTIONAL,
    TypeEdge::ARRAY,
    TypeEdge::UNION,
};

inline constexpr TypeEdge kReflectConstructorsRoute[] = {
    TypeEdge::OPTIONAL, TypeEdge::ARRAY, TypeEdge::PROMISE, TypeEdge::RESULT, TypeEdge::FUNCTION,
    TypeEdge::YIELDED,  TypeEdge::DICT,  TypeEdge::UNION,   TypeEdge::FIELDS,
};

inline constexpr TypeEdge kSameEveryRoundRoute[] = {
    TypeEdge::OPTIONAL, TypeEdge::ARRAY, TypeEdge::PROMISE, TypeEdge::RESULT, TypeEdge::FUNCTION,
    TypeEdge::ITERATOR, TypeEdge::DICT,  TypeEdge::UNION,   TypeEdge::FIELDS, TypeEdge::TUPLE_REST,
};

inline constexpr TypeEdge kMembersOfRoute[] = {
    TypeEdge::CONSTRAINT,
    TypeEdge::UNION,
};

inline constexpr TypeEdge kGrowsRoute[] = {
    TypeEdge::OPTIONAL, TypeEdge::ARRAY, TypeEdge::PROMISE, TypeEdge::RESULT, TypeEdge::FUNCTION,
};

inline constexpr TypeRefKind kNeededAliasesRoute[] = {
    TypeRefKind::ARRAY_REF,    TypeRefKind::UNION_REF,  TypeRefKind::TUPLE_REF,
    TypeRefKind::FUNCTION_REF, TypeRefKind::OBJECT_REF,
};

}
