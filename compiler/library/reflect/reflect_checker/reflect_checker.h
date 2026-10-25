#pragma once

#include "compiler/check/sema/sema.h"

namespace tz {

// The classes reflection reaches, and fromJson of classes (docs/decorators.md, section 6).
class ReflectChecker {
public:
  explicit ReflectChecker(Sema &sema);

  // Reflection (sema_reflect.cpp, docs/decorators.md section 6).
  // Reflect.get, Reflect.apply and Reflect.construct, with unknowns for the
  // values, checked when the program runs; and fromJson(cls, j).
  Expr *checkReflection(CallExpr *call, MemberExpr *member, const Type *expected);
  Expr *checkFromJsonClass(CallExpr *call, NameExpr *name);
  // Why fromJson cannot make an object of `cls` (its constructor takes what
  // no Json gives...), or empty; finds the fields the Json sets after the
  // constructor runs (ClassDecl::jsonFields).
  std::string classJsonProblem(const ClassDecl *cls);
  // A value of `from` goes where one of `to` is, through `cast`: the classes
  // reflection may reach through it (see ClassDecl::reflectNew).
  void noteReflectedConversion(const Type *from, const Type *to, const CastExpr *cast);
  // At the end: which classes reflection reaches.
  void finishReflection();
  // The code being checked is never emitted: the generic body of a generic
  // function or class, which its copies stand for, or a trial copy (see
  // inferCycle). What it does reaches no class by reflection.
  bool onlyChecked() const;

private:
  Sema &sema_;
  // The classes whose objects become objects or unknowns, those whose objects
  // Reflect.get looks into by their type, and whether it looks into any.
  llvm::SmallPtrSet<ClassDecl *, 8> escaped_;
  llvm::SmallPtrSet<ClassDecl *, 8> reflectedObjects_;
  bool reflectGetAny_ = false;
  // fromJson(cls, j) of a class known only when the program runs.
  bool jsonReflect_ = false;
  llvm::DenseMap<const ClassDecl *, std::string> jsonProblems_;
};

} // namespace tz
