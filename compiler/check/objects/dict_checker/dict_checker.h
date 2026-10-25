#pragma once

#include "compiler/check/checker/checker.h"

namespace tz {

// Record, Map and Set (docs/objects.md, section 9): their types, members and keys.
class DictChecker {
public:
  explicit DictChecker(Checker &checker);

  // Dictionaries (sema_dict.cpp, docs/objects.md section 9): Record<string,
  // V>, also written `{ [key: string]: V }`, read and written as an object
  // is, and Map<K, V> and Set<T> with the methods of JavaScript. Their
  // operations become calls of built-ins (Builtin::MapGet...).
  //
  // `Record<K, V>`, `Map<K, V>`, `Set<T>` and the read-only names in a type
  // annotation; null if `ref` names none of them.
  const Type *resolveDictType(const TypeRef &ref);
  // Whether values of `type` can be keys; reports a type that cannot.
  bool checkKeyType(const Type *type, llvm::SMLoc loc);
  // A call of `builtin` on `receiver` (checked), as `receiver.name(args)`.
  Expr *dictCall(llvm::SMLoc loc, Expr *receiver, llvm::StringRef name, Builtin builtin,
                 std::vector<Expr *> args, const Type *type);
  // Where values are iterated: a Map as its entries(), a Set as its
  // values(), and anything else as it is.
  Expr *iterableOf(Expr *expr);
  Expr *checkRecordKey(Expr *key);
  // The key `name` of `r.name`, a string literal.
  Expr *keyLiteral(llvm::SMLoc loc, llvm::StringRef name);
  // `m.size` and `r.key`, `r[key]`, whose object is a checked dictionary.
  Expr *checkDictMember(MemberExpr *member);
  Expr *checkDictIndex(IndexExpr *index);
  // `r[key] = v` and `r.key = v`, whose object is a checked dictionary: the
  // type assigned. recordAssign makes the checked assignment a call; null
  // if it is not to a Record.
  const Type *checkDictTarget(Expr *target, bool compound);
  Expr *recordAssign(AssignExpr *assign);
  // `r[key] ??= v`; null, with the object checked, if it is no Record.
  Expr *checkRecordNullishAssign(AssignExpr *assign, IndexExpr *index);
  // `delete r[key]` (`keyExpr`) and `delete r.key` (`name`), and `key in r`,
  // whose dictionary is checked.
  Expr *checkDictDelete(UnaryExpr *unary, Expr *object, Expr *keyExpr, llvm::StringRef name);
  Expr *checkDictIn(BinaryExpr *bin);

private:
  Checker &checker_;
};

} // namespace tz
