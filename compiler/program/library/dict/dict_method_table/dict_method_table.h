#pragma once

#include "compiler/program/ast/builtin/builtin.h"
#include "compiler/union/union/union.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

namespace tz {

enum class DictMethod {
  GET,
  SET,
  ADD,
  HAS,
  DELETE_KEY,
  CLEAR,
  KEYS,
  VALUES,
  ENTRIES,
  FOR_EACH,
  UNION,
  INTERSECTION,
  DIFFERENCE,
  SYMMETRIC_DIFFERENCE,
  IS_SUBSET_OF,
  IS_SUPERSET_OF,
  IS_DISJOINT_FROM,
};

enum class DictMethodKind {
  MAP_ONLY,
  SET_ONLY,
  SHARED,
  ITERATING,
  SET_COMBINE,
  SET_TEST,
};

struct MapOnlyDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::MAP_ONLY;
  Builtin builtin;
};

struct SetOnlyDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::SET_ONLY;
  Builtin builtin;
};

struct SharedDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::SHARED;
  Builtin builtin;
};

struct IteratingDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::ITERATING;
  Builtin onMap;
  Builtin onSet;
};

struct SetCombineDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::SET_COMBINE;
  Builtin builtin;
};

struct SetTestDictMethod {
  static constexpr DictMethodKind kind = DictMethodKind::SET_TEST;
  Builtin builtin;
};

using DictMethodForm = Union<MapOnlyDictMethod, SetOnlyDictMethod, SharedDictMethod,
                             IteratingDictMethod, SetCombineDictMethod, SetTestDictMethod>;

struct DictMethodInfo {
  llvm::StringLiteral name;
  DictMethod method;
  DictMethodForm form;
};

class DictMethodTable {
public:
  static bool isOnMap(const DictMethodInfo &method);
  static bool isOnSet(const DictMethodInfo &method);
  static const DictMethodInfo *find(llvm::StringRef name);
  static Builtin getBuiltin(const DictMethodInfo &method, bool onMap);
  static llvm::StringRef getName(DictMethod method);
  static llvm::ArrayRef<DictMethodInfo> getMethods();
};

}
