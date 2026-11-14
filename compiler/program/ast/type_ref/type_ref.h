#pragma once

#include "compiler/union/union/union.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

class Type;

enum class TypeRefKind {
  NAME_REF,
  ARRAY_REF,
  UNION_REF,
  GENERIC_REF,
  FUNCTION_REF,
  OBJECT_REF,
  TUPLE_REF,
  LITERAL_REF,
  PREDICATE_REF,
  KEY_OF_REF,
  INDEXED_REF,
  TYPE_OF_REF,
  MAPPED_REF,
  CONDITIONAL_REF,
  INFER_REF,
  TEMPLATE_REF,
  INTERSECTION_REF,
};

struct NameTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::NAME_REF;
};

struct ArrayTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::ARRAY_REF;
};

struct UnionTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::UNION_REF;
};

struct GenericTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::GENERIC_REF;
};

struct FunctionTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::FUNCTION_REF;
  std::vector<llvm::StringRef> names;
  std::vector<bool> optional;
  bool rest = false;
};

struct ObjectTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::OBJECT_REF;
  std::vector<llvm::StringRef> names;
  std::vector<llvm::SMLoc> locs;
  std::vector<bool> optional;
  std::vector<bool> readonly;
  std::vector<bool> methods;
};

struct TupleTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::TUPLE_REF;
  std::vector<bool> readonly;
  std::vector<bool> spread;
};

struct LiteralTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::LITERAL_REF;
  std::u16string text;
  double number = 0;
  bool integer = false;
};

struct PredicateTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::PREDICATE_REF;
  bool asserts = false;
};

struct KeyOfTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::KEY_OF_REF;
};

struct IndexedTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::INDEXED_REF;
};

struct TypeOfTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::TYPE_OF_REF;
  std::vector<llvm::StringRef> names;
};

struct MappedTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::MAPPED_REF;
  int mappedOptional = 0;
  int mappedReadonly = 0;
};

struct ConditionalTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::CONDITIONAL_REF;
};

struct InferTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::INFER_REF;
};

struct TemplateTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::TEMPLATE_REF;
  std::vector<std::u16string> texts;
};

struct IntersectionTypeRef {
  static constexpr TypeRefKind kind = TypeRefKind::INTERSECTION_REF;
};

using TypeRefForm = Union<NameTypeRef, ArrayTypeRef, UnionTypeRef, GenericTypeRef, FunctionTypeRef,
                          ObjectTypeRef, TupleTypeRef, LiteralTypeRef, PredicateTypeRef,
                          KeyOfTypeRef, IndexedTypeRef, TypeOfTypeRef, MappedTypeRef,
                          ConditionalTypeRef, InferTypeRef, TemplateTypeRef, IntersectionTypeRef>;

struct TypeRef {
  TypeRefForm form = NameTypeRef{};
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
  llvm::StringRef source;
  const Type *resolved = nullptr;

  TypeRefKind kind() const;
  const std::vector<llvm::StringRef> &names() const;
  const std::vector<llvm::SMLoc> &locs() const;
  const std::vector<bool> &optional() const;
  const std::vector<bool> &readonly() const;
  const std::vector<bool> &methods() const;
  const std::vector<bool> &spread() const;
  bool rest() const;
  const std::u16string &text() const;
  double number() const;
  bool integer() const;
  const std::vector<std::u16string> &texts() const;
  bool asserts() const;
  int mappedOptional() const;
  int mappedReadonly() const;
};

}
