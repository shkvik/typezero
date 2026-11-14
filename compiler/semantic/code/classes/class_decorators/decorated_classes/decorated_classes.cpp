#include "compiler/semantic/code/classes/class_decorators/decorated_classes/decorated_classes.h"
#include "llvm/ADT/STLExtras.h"

namespace tz {

DecoratedClasses::DecoratedClasses(SemanticState &owner, DecoratorRules &sharedDecoratorRules)
    : state(owner),
      decoratorRules(sharedDecoratorRules) {}

void DecoratedClasses::decorateClass(const DecorateClass &task) {
  ClassDecl *classDecl = task.classDecl;
  auto decorated = this->hasDecorators(classDecl);
  if (decorated.isErr()) {
    return;
  }
  this->state.walk.self.enterClass(classDecl);
  this->state.walk.tasks.pushTask(LeaveClassContext{});

  return this->startDecorateClass(classDecl);
}

void DecoratedClasses::leaveClassContext() {
  return this->state.walk.self.leaveClass();
}

void DecoratedClasses::startDecorateClass(ClassDecl *classDecl) {
  this->walkDecoratedNames(classDecl);
  auto &tasks = this->state.walk.tasks;
  auto classUses = this->classUses(classDecl);
  if (!classUses.empty()) {
    auto run = this->decorateRun(classDecl, nullptr, nullptr, std::move(classUses));
    tasks.pushTask(Decorate{.run = std::move(run)});
  }
  auto members = this->decoratedMembers(classDecl);
  for (const DecoratedMember &member : llvm::reverse(members)) {
    auto uses = this->memberUses(member);
    if (uses.empty()) {
      continue;
    }
    auto run = this->decorateRun(classDecl, member.field, member.method, std::move(uses));
    tasks.pushTask(Decorate{.run = std::move(run)});
  }
}

DecorateRun DecoratedClasses::decorateRun(ClassDecl *classDecl, ClassField *field,
                                          FunctionDecl *method,
                                          std::vector<DecoratorUse> uses) const {
  auto ownPlace = this->placeOfMember(field, method);
  auto memberName = this->nameOfMember(field, method);
  llvm::SMLoc at = uses[0].decorator->loc;

  return DecorateRun{
      .classDecl = classDecl,
      .field = field,
      .method = method,
      .uses = std::move(uses),
      .ownPlace = ownPlace,
      .memberName = memberName,
      .at = at,
      .earlyDescriptor = nullptr,
      .methodReturn = nullptr,
      .isMethodReturnKnown = false,
      .callees = {},
      .takes = {},
      .hasDescriptor = false,
      .descriptorVar = nullptr,
      .descriptorType = nullptr,
      .current = classDecl,
      .index = 0,
  };
}

DecoratedClasses::ResultHasDecorators
DecoratedClasses::hasDecorators(const ClassDecl *classDecl) const {
  auto functionTest = std::bind_front(&DecoratedClasses::isDecoratedFunction, this);
  auto fieldTest = std::bind_front(&DecoratedClasses::isDecoratedField, this);
  bool isDecoratedAnywhere = !classDecl->decorators.empty() ||
                             llvm::any_of(classDecl->methods, functionTest) ||
                             llvm::any_of(classDecl->constructors, functionTest) ||
                             llvm::any_of(classDecl->fields, fieldTest);
  if (!isDecoratedAnywhere) {
    auto message = ErrorTextDecorators::none(classDecl->name);

    return err(CodeClassesRegistryHasDecoratorsError{
        .code = CodeClassesRegistryHasDecoratorsErrorCode::NO_DECORATORS,
        .message = message,
    });
  }

  return ok(classDecl);
}

bool DecoratedClasses::isDecoratedFunction(const FunctionDecl *functionDecl) const {
  if (!functionDecl->decorators().empty()) {
    return true;
  }

  return llvm::any_of(functionDecl->params,
                      std::bind_front(&DecoratorRules::isDecoratedParam, &this->decoratorRules));
}

bool DecoratedClasses::isDecoratedField(const ClassField &field) const {
  return !field.decorators.empty();
}

DecoratedClasses::ResultNamesAreDistinct
DecoratedClasses::checkNamesDistinct(const DecoratedName &earlier, const DecoratedName &later,
                                     llvm::StringRef className) const {
  if (earlier.name == later.name) {
    std::string what = earlier.isStatic != later.isStatic
                           ? ErrorTextDecoratedNames::bothKinds(earlier.name)
                           : ErrorTextDecoratedNames::variants(earlier.name);
    auto shared = this->sharedDecorator(earlier, later);
    std::string records = shared.empty() ? ErrorTextDecoratedNames::design()
                                         : ErrorTextDecoratedNames::recordsOf(shared);
    auto message = ErrorTextDecoratedNames::shared(what, className, records);

    return err(CodeClassesRegistryNamesDistinctError{
        .code = CodeClassesRegistryNamesDistinctErrorCode::SAME_NAME,
        .message = message,
    });
  }

  return ok(&later);
}

std::string DecoratedClasses::sharedDecorator(const DecoratedName &earlier,
                                              const DecoratedName &later) const {
  for (const std::string &decorator : llvm::reverse(later.decorators)) {
    if (!decorator.empty() && llvm::is_contained(earlier.decorators, decorator)) {
      return decorator;
    }
  }

  return "";
}

DecoratedClasses::ResultFirstConstructor DecoratedClasses::checkOneDecoratedConstructor(
    const ClassDecl *classDecl, llvm::ArrayRef<const FunctionDecl *> decorated) const {
  if (decorated.size() > 1) {
    auto message = ErrorTextDecoratedNames::sameRecord(classDecl->name);

    return err(CodeClassesRegistrySecondConstructorError{
        .code = CodeClassesRegistrySecondConstructorErrorCode::TWO_DECORATED_CONSTRUCTORS,
        .message = message,
    });
  }

  return ok(classDecl);
}

void DecoratedClasses::walkDecoratedNames(ClassDecl *classDecl) {
  auto members = this->decoratedNames(classDecl);
  for (size_t index = 0; index < members.size(); ++index) {
    this->walkNameClash(members, index, classDecl->name);
  }
  this->walkDecoratedConstructors(classDecl);
}

std::vector<DecoratedClasses::DecoratedName>
DecoratedClasses::decoratedNames(const ClassDecl *classDecl) const {
  auto members = std::vector<DecoratedName>();
  for (const ClassField &field : classDecl->fields) {
    auto decorators = this->decoratorNames(field.decorators);
    if (!decorators.empty()) {
      members.push_back({
          .name = field.name,
          .loc = field.loc,
          .isStatic = field.isStatic(),
          .decorators = decorators,
      });
    }
  }
  for (const FunctionDecl *functionDecl : classDecl->methods) {
    auto decorators = this->methodDecoratorNames(functionDecl);
    if (!decorators.empty()) {
      members.push_back({
          .name = functionDecl->name,
          .loc = functionDecl->loc(),
          .isStatic = functionDecl->isStatic(),
          .decorators = decorators,
      });
    }
  }

  return members;
}

std::vector<std::string>
DecoratedClasses::decoratorNames(llvm::ArrayRef<Decorator> decorators) const {
  auto names = std::vector<std::string>();
  this->addDecoratorNames(names, decorators);

  return names;
}

std::vector<std::string>
DecoratedClasses::methodDecoratorNames(const FunctionDecl *functionDecl) const {
  auto names = this->decoratorNames(functionDecl->decorators());
  for (const Param &param : functionDecl->params) {
    this->addDecoratorNames(names, param.decorators);
  }

  return names;
}

void DecoratedClasses::addDecoratorNames(std::vector<std::string> &names,
                                         llvm::ArrayRef<Decorator> decorators) const {
  for (const Decorator &decorator : decorators) {
    auto name = this->decoratorRules.decoratorName(decorator.expr);
    names.push_back(name);
  }
}

void DecoratedClasses::walkNameClash(llvm::ArrayRef<DecoratedName> members, size_t index,
                                     llvm::StringRef className) {
  const DecoratedName &later = members[index];
  for (const DecoratedName &earlier : members.take_front(index)) {
    auto distinct = this->checkNamesDistinct(earlier, later, className);
    if (distinct.isErr()) {
      this->state.diagnostics.error(later.loc, distinct.error().message);
      auto other = ErrorTextDecoratedNames::other();
      this->state.diagnostics.note(earlier.loc, other);
      return;
    }
  }
}

void DecoratedClasses::walkDecoratedConstructors(const ClassDecl *classDecl) {
  auto decorated = this->decoratorRules.decoratedConstructors(classDecl);
  auto only = this->checkOneDecoratedConstructor(classDecl, decorated);
  if (only.isErr()) {
    this->state.diagnostics.error(decorated[1]->loc(), only.error().message);
    auto other = ErrorTextDecoratedNames::other();
    this->state.diagnostics.note(decorated[0]->loc(), other);
  }
}

std::vector<DecoratedClasses::DecoratedMember>
DecoratedClasses::decoratedMembers(ClassDecl *classDecl) const {
  auto members = std::vector<DecoratedMember>();
  for (ClassField &field : classDecl->fields) {
    if (!field.decorators.empty()) {
      members.push_back({
          .field = &field,
          .method = nullptr,
          .isStatic = field.isStatic(),
          .at = field.loc.getPointer(),
      });
    }
  }
  for (FunctionDecl *functionDecl : classDecl->methods) {
    if (!functionDecl->decorators().empty() ||
        this->decoratorRules.hasDecoratedParams(functionDecl)) {
      members.push_back({
          .field = nullptr,
          .method = functionDecl,
          .isStatic = functionDecl->isStatic(),
          .at = functionDecl->loc().getPointer(),
      });
    }
  }
  auto isBefore = std::bind_front(&DecoratedClasses::isMemberBefore, this);
  llvm::stable_sort(members, isBefore);

  return members;
}

bool DecoratedClasses::isMemberBefore(const DecoratedMember &first,
                                      const DecoratedMember &second) const {
  return std::make_pair(first.isStatic, first.at) < std::make_pair(second.isStatic, second.at);
}

std::vector<DecoratorUse> DecoratedClasses::memberUses(const DecoratedMember &member) const {
  if (member.field != nullptr) {
    return this->ownUses(member.field->decorators);
  }
  auto uses = this->ownUses(member.method->decorators());
  this->addParamUses(uses, member.method);

  return uses;
}

std::vector<DecoratorUse> DecoratedClasses::classUses(const ClassDecl *classDecl) const {
  auto uses = this->ownUses(classDecl->decorators);
  for (const FunctionDecl *constructor : classDecl->constructors) {
    this->addParamUses(uses, constructor);
  }

  return uses;
}

std::vector<DecoratorUse> DecoratedClasses::ownUses(llvm::ArrayRef<Decorator> decorators) const {
  auto uses = std::vector<DecoratorUse>();
  for (const Decorator &decorator : decorators) {
    uses.push_back({.decorator = &decorator});
  }

  return uses;
}

void DecoratedClasses::addParamUses(std::vector<DecoratorUse> &uses,
                                    const FunctionDecl *functionDecl) const {
  for (size_t index = 0; index < functionDecl->params.size(); ++index) {
    this->addParamUse(uses, functionDecl, index);
  }
}

void DecoratedClasses::addParamUse(std::vector<DecoratorUse> &uses,
                                   const FunctionDecl *functionDecl, size_t index) const {
  for (const Decorator &decorator : functionDecl->params[index].decorators) {
    uses.push_back({.decorator = &decorator, .param = int(index), .owner = functionDecl});
  }
}

DecoratorPlace DecoratedClasses::placeOfMember(const ClassField *field,
                                               const FunctionDecl *method) const {
  if (field != nullptr) {
    return DecoratorPlace::FIELD;
  }
  if (method != nullptr) {
    return DecoratorPlace::METHOD;
  }

  return DecoratorPlace::CLASS;
}

llvm::StringRef DecoratedClasses::nameOfMember(const ClassField *field,
                                               const FunctionDecl *method) const {
  if (field != nullptr) {
    return field->name;
  }
  if (method != nullptr) {
    return method->name;
  }

  return llvm::StringRef();
}

}
