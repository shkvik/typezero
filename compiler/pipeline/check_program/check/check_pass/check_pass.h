#pragma once

#include "compiler/pipeline/check_program/check/top_level_checker/top_level_checker.h"
#include "compiler/pipeline/check_program/declare/import_binder/import_binder.h"
#include "compiler/pipeline/check_program/declare/module_declarer/module_declarer.h"
#include "compiler/pipeline/check_program/infer/return_inferrer/return_inferrer.h"
#include "compiler/pipeline/check_program/instances/late_classes/late_classes.h"
#include "compiler/pipeline/check_program/topics/classes/class_checker/class_checker.h"
#include "compiler/pipeline/check_program/topics/declarations/declaration_checker/declaration_checker.h"
#include "compiler/pipeline/check_program/topics/functions/function_checker/function_checker.h"
#include "compiler/program/errors/errors.h"
#include "compiler/program/pass_done/pass_done.h"
#include "compiler/result/result/result.h"

namespace tz {

class CheckState;
struct SourceFile;

class CheckPass {
public:
  using ResultChecked = Result<PassDone, Errors>;

  CheckPass(CheckState &owner, SourceFile &program);

  ResultChecked run();

private:
  void checkClass(ClassDecl *cls);
  void checkFunction(FunctionDecl *fn);
  void checkLiftedFunction(FunctionDecl *fn);

  CheckState &state;
  SourceFile &file;

  LateClasses lateClasses;
  ClassChecker classChecker;
  ImportBinder importBinder;
  ModuleDeclarer moduleDeclarer;
  ReturnInferrer returnInferrer;
  FunctionChecker functionChecker;
  TopLevelChecker topLevelChecker;
  DeclarationChecker declarationChecker;
};

}
