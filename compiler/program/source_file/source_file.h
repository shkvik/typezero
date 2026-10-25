#pragma once

#include "compiler/program/ast/export_decl/export_decl.h"
#include "compiler/program/ast/function_decl/function_decl.h"
#include "compiler/program/ast/import_decl/import_decl.h"
#include "compiler/program/ast/stmt/stmt.h"

namespace tz {

struct Module;
class Type;

// One parsed .tz file, or a whole program: the loader joins the statements
// of all its files in the order they load. Top-level statements other than
// function declarations form the program's entry point.
struct SourceFile {
  std::vector<Stmt *> items;
  std::vector<ImportDecl> imports;
  std::vector<ExportDecl> exports;
  // A program: its files in the order they load, the entry last.
  std::vector<Module *> modules;
  // The names of the fields of the object types and literals of the program:
  // `o.name(...)` with any other name cannot call a function in a field.
  llvm::StringSet<> fieldNames;
  // The strings of the constants declared with a string literal, by name
  // (in any scope), and of the members of string enums, as "Enum.Member":
  // the fields `o[key] = v` may assign to, which Sema must know before it
  // checks the program (see Sema::keyNames).
  llvm::StringMap<std::vector<std::string>> stringConstants;
  // Set by Sema: the program uses promises, timers or the queues, so main
  // runs the event loop after the top-level code (docs/async.md); and its
  // top-level code awaits, so it is a coroutine itself.
  bool usesAsync = false;
  bool topLevelAwait = false;
  // Set by Sema: the program compares functions (`==`, indexOf...), so every
  // evaluation of a function expression must give a distinct value.
  bool comparesFunctions = false;
  // Set by Sema: the program may use https (a fetch of a URL not known to be
  // http://, serve with `tls`), so it gets TLS (docs/net.md, section 4).
  bool usesTls = false;
  // Set by Sema: the functions of imported files that the program never
  // uses, which are left out of it.
  llvm::SmallPtrSet<const FunctionDecl *, 8> unusedFunctions;
  // Set by Sema: the copies of the generic functions that the program calls,
  // one for each representation of their type arguments in memory. CodeGen
  // emits them as top-level functions, and no generic function itself.
  std::vector<FunctionDecl *> instances;
  // Set by Sema: the object types declared to be others too (Type::bases):
  // classes that extend a class or implement interfaces, and interfaces that
  // extend interfaces. CodeGen lays out and describes their objects so that
  // code that knows them by a base type works on them.
  std::vector<const Type *> subtypes;
  // Set by Sema: the object types whose objects code sees as another type
  // with the same fields, some of them read-only there: CodeGen treats the
  // second as an interface that the first implements.
  std::vector<std::pair<const Type *, const Type *>> views;
};

} // namespace tz
