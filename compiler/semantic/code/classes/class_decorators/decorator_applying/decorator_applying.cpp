#include "compiler/semantic/code/classes/class_decorators/decorator_applying/decorator_applying.h"
#include "compiler/program/library/builtin_names/builtin_property_table/builtin_property_table.h"
#include "compiler/program/library/builtin_names/global_name_table/global_name_table.h"
#include "compiler/program/library/reflect/reflect_function_table/reflect_function_table.h"
#include "compiler/semantic/declare/class_tree/class_tree.h"
#include "compiler/semantic/types/type_parts/type_routes.h"
#include "llvm/ADT/STLExtras.h"

using llvm::cast;

namespace tz {

DecoratorApplying::DecoratorApplying(SemanticState &owner, ClassFacts &sharedFacts,
                                     DecoratorRules &sharedDecoratorRules)
    : facts(sharedFacts),
      state(owner),
      decoratorRules(sharedDecoratorRules) {}

void DecoratorApplying::nextAppliedDecorator(const NextAppliedDecorator &task) {
  auto run = task.run;
  auto &tasks = this->state.walk.tasks;
  if (run.index == 0 && run.descriptorVar != nullptr) {
    return this->storeDescriptorValue(run);
  }
  if (run.index == 0) {
    return;
  }
  run.index -= 1;
  const DecoratorUse &use = run.uses[run.index];
  if (run.callees[run.index] == nullptr) {
    tasks.pushTask(NextAppliedDecorator{.run = run});
    return this->applyMetadata(run, use);
  }
  llvm::SMLoc loc = use.decorator->loc;
  DecoratorPlace place = this->decoratorRules.usePlace(run, use);
  auto args = this->decoratorArgs(run, use, place);
  args.resize(std::min(args.size(), run.takes[run.index]));
  auto *call = this->state.ast.make<CallExpr>(loc, run.callees[run.index], std::move(args));
  auto *statement = this->state.ast.make<ExprStmt>(loc, call);
  run.classDecl->decorations.push_back(statement);
  tasks.pushTask(AfterDecoratorApplied{.run = std::move(run), .statement = statement});

  return tasks.pushTask(CheckExprSlot{.slot = &statement->expr, .expected = nullptr});
}

void DecoratorApplying::afterDecoratorApplied(const AfterDecoratorApplied &task) {
  auto run = task.run;
  ExprStmt *statement = task.statement;
  const DecoratorUse &use = run.uses[run.index];
  DecoratorPlace place = this->decoratorRules.usePlace(run, use);
  auto moved = std::vector<ClassDecl *>();
  statement->expr = this->decoratorOutcome(run, use, place, statement->expr, moved);
  this->state.walk.tasks.pushTask(NextAppliedDecorator{.run = std::move(run)});
  for (ClassDecl *child : llvm::reverse(moved)) {
    this->pushRelink(child);
  }
}

void DecoratorApplying::applyMetadata(const DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  DecoratorPlace place = this->decoratorRules.usePlace(run, use);
  auto *metadata = cast<CallExpr>(use.decorator->expr);
  auto pair = this->checkMetadataArgs(metadata);
  if (pair.isErr()) {
    return this->state.diagnostics.error(loc, pair.error().message);
  }
  auto reflectName = GlobalNameTable::getName(GlobalName::REFLECT);
  auto *reflect = this->state.ast.make<NameExpr>(loc, reflectName);
  auto defineName = ReflectFunctionTable::getName(ReflectFunction::DEFINE_METADATA);
  auto *callee = this->state.ast.make<MemberExpr>(loc, reflect, defineName);
  auto *target = this->classValueAt(run.classDecl, loc);
  std::vector<Expr *> args{metadata->args[0], metadata->args[1], target};
  if (place != DecoratorPlace::CLASS && !this->isConstructorParamUse(use)) {
    auto *key = this->memberKey(run, use);
    args.push_back(key);
  }
  auto *call = this->state.ast.make<CallExpr>(loc, callee, args);
  auto *statement = this->state.ast.make<ExprStmt>(loc, call);
  run.classDecl->decorations.push_back(statement);

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &statement->expr, .expected = nullptr});
}

Expr *DecoratorApplying::decoratorOutcome(DecorateRun &run, const DecoratorUse &use,
                                          DecoratorPlace place, Expr *applied,
                                          std::vector<ClassDecl *> &moved) {
  llvm::SMLoc loc = use.decorator->loc;
  const Type *result = applied->type;
  if (run.descriptorVar != nullptr && place == DecoratorPlace::METHOD &&
      result == run.descriptorType) {
    return this->assignDescriptor(run, use, applied);
  }
  const ClassDecl *made = result->classRef();
  if (place == DecoratorPlace::CLASS && made != nullptr && made->derivesFrom(run.current)) {
    auto *given = const_cast<ClassDecl *>(made);
    if (given != run.current) {
      this->replaceClass(run.current, given, loc, moved);
      run.current = given;
    }
    return applied;
  }
  auto label = this->decoratorRules.decoratorLabel(use.decorator->expr);
  bool hasDescriptor = run.descriptorVar != nullptr;
  auto returned = this->checkDecoratorResult(label, result, place, hasDescriptor, run.current);
  if (returned.isErr()) {
    this->state.diagnostics.error(loc, returned.error().message);
  }

  return applied;
}

void DecoratorApplying::storeDescriptorValue(const DecorateRun &run) {
  llvm::SMLoc at = run.at;
  auto *target = this->state.ast.make<NameExpr>(at, run.method->impl()->name);
  auto *descriptor = this->state.ast.make<NameExpr>(at, run.descriptorVar->name);
  auto valueName = BuiltinPropertyTable::getName(BuiltinProperty::DESCRIPTOR_VALUE);
  auto *value = this->state.ast.make<MemberExpr>(at, descriptor, valueName);
  auto *assign = this->state.ast.make<AssignExpr>(at, std::nullopt, target, value);
  auto *statement = this->state.ast.make<ExprStmt>(at, assign);
  run.classDecl->decorations.push_back(statement);

  return this->state.walk.tasks.pushTask(
      CheckExprSlot{.slot = &statement->expr, .expected = nullptr});
}

void DecoratorApplying::replaceClass(ClassDecl *classDecl, ClassDecl *replacement, llvm::SMLoc loc,
                                     std::vector<ClassDecl *> &moved) {
  for (const ClassField &field : classDecl->fields) {
    auto kept = this->oldObject(classDecl, field);
    if (kept.isErr()) {
      auto here = ErrorTextOldObject::decorator();
      this->state.diagnostics.error(field.loc, kept.error().message);
      this->state.diagnostics.note(loc, here);
      return;
    }
  }
  this->swapTypes(classDecl, replacement);
  auto declared = this->declaredChildren(classDecl, replacement);
  for (ClassDecl *child : declared) {
    for (const ClassField &field : child->fields) {
      auto unique = this->replacedField(classDecl, replacement, child, field);
      if (unique.isErr()) {
        return this->state.diagnostics.error(field.loc, unique.error().message);
      }
    }
    this->moveChild(classDecl, replacement, child);
    moved.push_back(child);
  }
  classDecl->replacement = replacement;
  replacement->usedAsValue = true;
}

void DecoratorApplying::moveChild(ClassDecl *classDecl, ClassDecl *replacement,
                                  ClassDecl *child) const {
  llvm::erase(classDecl->children, child);
  replacement->children.push_back(child);
  child->parent = replacement;
}

void DecoratorApplying::pushRelink(ClassDecl *classDecl) {
  auto below = ClassTree::descendants(classDecl);
  for (ClassDecl *descendant : llvm::reverse(below)) {
    this->state.walk.tasks.pushTask(RelinkClass{.classDecl = descendant});
  }

  return this->state.walk.tasks.pushTask(RelinkClass{.classDecl = classDecl});
}

void DecoratorApplying::relinkClass(const RelinkClass &task) {
  ClassDecl *classDecl = task.classDecl;
  auto &tasks = this->state.walk.tasks;
  const_cast<Type *>(classDecl->type)->forgetFields();
  this->state.diagnostics.pushList(DiagnosticRoute::DISCARD);
  tasks.pushTask(BuildVtable{.classDecl = classDecl});
  tasks.pushTask(DropDiagnosticList{});

  return tasks.pushTask(StartLinkOverrides{.classDecl = classDecl});
}

void DecoratorApplying::dropDiagnosticList() {
  this->state.diagnostics.popList();
}

void DecoratorApplying::addInventorySubtype(const Type *type) {
  if (!llvm::is_contained(this->state.inventory.subtypes, type)) {
    this->state.inventory.subtypes.push_back(type);
  }
}

Expr *DecoratorApplying::classValueAt(ClassDecl *classDecl, llvm::SMLoc loc) {
  auto *name = this->state.ast.make<NameExpr>(loc, classDecl->name);
  name->binding = NamedClass{.cls = classDecl};
  name->type = this->facts.classRefType(classDecl);
  classDecl->usedAsValue = true;

  return name;
}

bool DecoratorApplying::isConstructorParamUse(const DecoratorUse &use) const {
  return this->decoratorRules.isParamUse(use) && use.owner->isConstructor();
}

llvm::StringRef DecoratorApplying::useOwnerName(const DecorateRun &run,
                                                const DecoratorUse &use) const {
  return this->decoratorRules.isParamUse(use) ? use.owner->name : run.memberName;
}

DecoratorApplying::ResultMetadataArgsPair
DecoratorApplying::checkMetadataArgs(const CallExpr *metadata) const {
  if (metadata->args.size() != 2 || metadata->hasSpread()) {
    auto message = ErrorTextDecorate::metadata();

    return err(CodeClassesRegistryMetadataArgsError{
        .code = CodeClassesRegistryMetadataArgsErrorCode::WRONG_METADATA_ARGUMENTS,
        .message = message,
    });
  }

  return ok(metadata);
}

DecoratorApplying::ResultDecoratorReturns
DecoratorApplying::checkDecoratorResult(llvm::StringRef label, const Type *result,
                                        DecoratorPlace place, bool hasDescriptor,
                                        const ClassDecl *current) const {
  bool isNothing = result->isVoid() || result->isUndefined() || result->isError();
  if (!isNothing && place == DecoratorPlace::CLASS && result->isConstructor()) {
    auto message = ErrorTextDecoratorResult::runtime(label, result->name(), current->name);

    return err(CodeClassesRegistryDecoratorResultError{
        .code = CodeClassesRegistryDecoratorResultErrorCode::RUNTIME_CLASS,
        .message = message,
    });
  }
  if (!isNothing && place == DecoratorPlace::CLASS) {
    auto message = ErrorTextDecoratorResult::forClass(label, result->name());

    return err(CodeClassesRegistryDecoratorResultError{
        .code = CodeClassesRegistryDecoratorResultErrorCode::CLASS_RESULT,
        .message = message,
    });
  }
  if (!isNothing && place == DecoratorPlace::METHOD && hasDescriptor) {
    auto message = ErrorTextDecoratorResult::forMethod(label, result->name());

    return err(CodeClassesRegistryDecoratorResultError{
        .code = CodeClassesRegistryDecoratorResultErrorCode::METHOD_RESULT,
        .message = message,
    });
  }
  if (!isNothing) {
    auto where = this->decoratorRules.placeText(place);
    auto message = ErrorTextDecoratorResult::other(label, result->name(), where);

    return err(CodeClassesRegistryDecoratorResultError{
        .code = CodeClassesRegistryDecoratorResultErrorCode::OTHER_RESULT,
        .message = message,
    });
  }

  return ok(result);
}

std::vector<Expr *> DecoratorApplying::decoratorArgs(const DecorateRun &run,
                                                     const DecoratorUse &use,
                                                     DecoratorPlace place) {
  llvm::SMLoc loc = use.decorator->loc;
  ClassDecl *target = place == DecoratorPlace::CLASS ? run.current : run.classDecl;
  auto *classValue = this->classValueAt(target, loc);
  std::vector<Expr *> args{classValue};
  if (place != DecoratorPlace::CLASS) {
    auto *key = this->memberKey(run, use);
    args.push_back(key);
  }
  if (place == DecoratorPlace::PARAMETER) {
    auto *position = this->state.ast.make<IntLitExpr>(loc, uint64_t(use.param));
    args.push_back(position);
  } else if (place == DecoratorPlace::METHOD && run.descriptorVar != nullptr) {
    auto *descriptor = this->state.ast.make<NameExpr>(loc, run.descriptorVar->name);
    args.push_back(descriptor);
  }

  return args;
}

Expr *DecoratorApplying::memberKey(const DecorateRun &run, const DecoratorUse &use) {
  llvm::SMLoc loc = use.decorator->loc;
  if (this->isConstructorParamUse(use)) {
    return this->state.ast.make<UndefinedExpr>(loc);
  }
  auto name = this->useOwnerName(run, use);
  auto text = std::u16string(name.begin(), name.end());

  return this->state.ast.make<StringLitExpr>(loc, text);
}

Expr *DecoratorApplying::assignDescriptor(const DecorateRun &run, const DecoratorUse &use,
                                          Expr *applied) {
  llvm::SMLoc loc = use.decorator->loc;
  auto *target = this->state.ast.make<NameExpr>(loc, run.descriptorVar->name);
  target->binding = NamedVar{.var = run.descriptorVar};
  target->type = run.descriptorType;
  auto *assign = this->state.ast.make<AssignExpr>(loc, std::nullopt, target, applied);
  assign->type = run.descriptorType;

  return assign;
}

bool DecoratorApplying::isHoldingType(const Type *type, const Type *target) const {
  auto parts = TypeParts::collect(type, kHoldsRoute);

  return llvm::is_contained(parts.getTypes(), target);
}

DecoratorApplying::ResultNoOldObjectHeld
DecoratorApplying::oldObject(const ClassDecl *classDecl, const ClassField &field) const {
  bool isTyped = field.isStatic() && field.var() != nullptr && field.var()->type != nullptr;
  bool isHolding = isTyped && this->isHoldingType(field.var()->type, classDecl->type);
  if (isHolding) {
    auto message = ErrorTextOldObject::held(classDecl->name, field.name);

    return err(CodeClassesRegistryOldObjectError{
        .code = CodeClassesRegistryOldObjectErrorCode::HOLDS_OLD_OBJECT,
        .message = message,
    });
  }

  return ok(&field);
}

DecoratorApplying::ResultFieldNameIsFree
DecoratorApplying::replacedField(const ClassDecl *classDecl, const ClassDecl *replacement,
                                 const ClassDecl *child, const ClassField &field) const {
  bool isClash = !field.isStatic() && replacement->field(field.name) != nullptr;
  if (isClash) {
    auto message = ErrorTextReplacedField::clash(child->name, classDecl->name, field.name);

    return err(CodeClassesRegistryReplacedFieldError{
        .code = CodeClassesRegistryReplacedFieldErrorCode::FIELD_IN_REPLACEMENT,
        .message = message,
    });
  }

  return ok(&field);
}

void DecoratorApplying::swapTypes(ClassDecl *classDecl, ClassDecl *replacement) {
  auto *mine = const_cast<Type *>(classDecl->type);
  auto *theirs = const_cast<Type *>(replacement->type);
  std::string name = mine->name().str();
  Type::swapClasses(mine, theirs);
  classDecl->type = theirs;
  replacement->type = mine;
  theirs->rename(name + " (as declared)");
  this->addInventorySubtype(mine);
}

std::vector<ClassDecl *> DecoratorApplying::declaredChildren(const ClassDecl *classDecl,
                                                             const ClassDecl *replacement) {
  auto *declaredType = const_cast<Type *>(classDecl->type);
  auto *replacementType = const_cast<Type *>(replacement->type);
  auto declared = std::vector<ClassDecl *>();
  for (ClassDecl *child : classDecl->children) {
    if (child == replacement) {
      continue;
    }
    if (replacement->derivesFrom(child)) {
      const_cast<Type *>(child->type)->replaceBase(replacementType, declaredType);
      continue;
    }
    declared.push_back(child);
  }

  return declared;
}

}
