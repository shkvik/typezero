#pragma once

#include "compiler/lower/lowering/lowering.h"

namespace tz {

// The descriptors of classes and their tables of methods.
class ClassDescriptors {
public:
  explicit ClassDescriptors(Lowering &lowering);

  // Classes (codegen_class.cpp). A method is a function (ptr this,
  // params...) -> ret, the shape of the code of a closure: the object is its
  // environment, so a method bound to an object is the function value
  // { info, object }. A static method is a plain function, a static field a
  // global.
  void declareMethods(ClassDecl *cls);
  void emitMethods(ClassDecl *cls);
  llvm::Function *methodCode(const FunctionDecl *fn);
  // The descriptor of the instances: the layout's, with the class's name
  // and number, and the keys as node prints them.
  llvm::Constant *classDescriptor(const ClassDecl *cls);
  // What an environment or a box holds in place of an object's layout: a
  // constant { ptr drop }, which starts as a layout does, so that a function
  // value's environment may be either (see tz.obj.release).
  llvm::Constant *dropDescriptor(llvm::Function *drop);

private:
  Lowering &lowering_;
};

} // namespace tz
