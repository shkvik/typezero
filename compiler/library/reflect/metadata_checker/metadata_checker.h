#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The metadata decorators write, and the design types (docs/decorators.md, section 5).
class MetadataChecker {
public:
  explicit MetadataChecker(Sema &sema);

  // The declaration an object key names (a function, a class, a constant);
  // null for anything else, which is a string key.
  const void *metadataKey(const Expr *key, std::string &label);
  // What the records of the object key `key` hold: known from a record
  // checked already, or from the functions that write one (metaWriters_),
  // checked now; or else from `expected`, where the read goes.
  const Type *metadataType(const void *key, llvm::StringRef label, llvm::SMLoc loc,
                           const Type *expected);
  void findMetadataWriters(SourceFile &program);
  // Checks the body of `fn` now, out of turn, if it is not checked yet.
  void checkBodyEarly(FunctionDecl *fn);
  // The class a record `design:*` holds for a value of `type`: Number,
  // String, Array, Function, Object... or the class itself.
  Expr *designClass(const Type *type, llvm::SMLoc loc);
  ClassDecl *designConstructor(const Type *type);
  // Number, String, Boolean, Object, Array, Function, Date, RegExp, Map,
  // Set, Promise and the types of binary data as values: classes of
  // JavaScript, which only `design:*` records and comparisons use; null for
  // any other name.
  ClassDecl *builtinConstructor(llvm::StringRef name);
  // The records design:type, design:paramtypes and design:returntype of a
  // decorated field or method (or of the class: of its constructor `ctor`),
  // added to cls->decorations.
  void writeDesignRecords(ClassDecl *cls, ClassField *field, FunctionDecl *method,
                          const FunctionDecl *ctor, llvm::SMLoc loc);
  void writeDesignRecord(ClassDecl *cls, const char16_t *key, Expr *value, llvm::StringRef member,
                         llvm::SMLoc loc);

private:
  Sema &sema_;
  // The functions that write records, by the name of the key they write.
  llvm::StringMap<std::vector<FunctionDecl *>> metaWriters_;
  llvm::StringMap<ClassDecl *> builtinConstructors_;
};

} // namespace tz
