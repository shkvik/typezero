#include "compiler/semantic/code/classes/code_classes_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextThis::undeclared(llvm::StringRef function) {
  return (llvm::Twine("internal error: 'this' of '") + function +
          "' is used before the method is checked")
      .str();
}

std::string ErrorTextClassStmt::nested(llvm::StringRef className) {
  return (llvm::Twine("a class can be declared only at the top level of a file; here, "
                      "make it with a class expression, 'const ") +
          className + " = class { ... }'")
      .str();
}

std::string ErrorTextFieldsSet::pathUnset(llvm::StringRef className, llvm::StringRef field) {
  return (llvm::Twine("the constructor of '") + className + "' does not set field '" + field +
          "' on every path; set it on every path, or make it optional: '" + field + "?: T'")
      .str();
}

std::string ErrorTextInitThis::outside() {
  return "the code is not in the body of the constructor being checked";
}

std::string ErrorTextInitThis::notThis() {
  return "the expression is not the 'this' of the constructor";
}

std::string ErrorTextDeclareThis::notMethod(llvm::StringRef function) {
  return (llvm::Twine("internal error: 'this' is declared for '") + function +
          "', which is not a method")
      .str();
}

std::string ErrorTextDecorators::none(llvm::StringRef className) {
  return (llvm::Twine("class '") + className + "' has no decorators").str();
}

std::string ErrorTextThisReady::noSuper() {
  return "'super(...)' is not called yet";
}

std::string ErrorTextThisReady::unset(llvm::StringRef field) {
  return (llvm::Twine("field '") + field + "' is not set yet").str();
}

std::string ErrorTextFirstUnset::outside() {
  return "the code is not in a constructor";
}

std::string ErrorTextFirstUnset::allSet(llvm::StringRef className) {
  return (llvm::Twine("every field of '") + className + "' is set").str();
}

std::string ErrorTextStartValue::noStart(llvm::StringRef field, llvm::StringRef type) {
  return (llvm::Twine("field '") + field + "' has no value to start with: '" + type +
          "' has no empty value, like \"\" or 0; set it in a constructor, or make it optional: '" +
          field + "?: " + type + "'")
      .str();
}

std::string ErrorTextStartValue::neverSet(llvm::StringRef field, llvm::StringRef type) {
  return (llvm::Twine("field '") + field +
          "' never gets a value: set it in a constructor, or make it optional: '" + field +
          "?: " + type + "'")
      .str();
}

std::string ErrorTextOverride::otherType(llvm::StringRef method, llvm::StringRef mine,
                                         llvm::StringRef className, llvm::StringRef theirs,
                                         llvm::StringRef baseClass) {
  return (llvm::Twine("'") + method + "' returns '" + mine + "' in '" + className + "', and '" +
          theirs + "' in '" + baseClass +
          "', which it overrides; an override returns what the method it "
          "overrides returns")
      .str();
}

std::string ErrorTextThisOf::outside() {
  return "'this' is used outside of a method; it exists in the methods "
         "of classes and object literals";
}

std::string ErrorTextThisOf::inField(llvm::StringRef className, llvm::StringRef member) {
  return (llvm::Twine("in a static field 'this' would be the class itself; use the "
                      "class's name: '") +
          className + "." + member + "'")
      .str();
}

std::string ErrorTextThisOf::inStatic(llvm::StringRef className, llvm::StringRef member) {
  return (llvm::Twine("in a static method 'this' would be the class itself; use the "
                      "class's name: '") +
          className + "." + member + "'")
      .str();
}

std::string ErrorTextThisOf::inFunction() {
  return "a 'function' has no 'this' of its own; use an arrow function, "
         "which sees the 'this' of the method around it";
}

std::string ErrorTextReadyThis::inClosure() {
  return "this function uses 'this', and it is created before every field "
         "is set; create it after the constructor sets them";
}

std::string ErrorTextReadyThis::noSuper(llvm::StringRef parentClass) {
  return (llvm::Twine("'this' cannot be used before 'super(...)': the object exists once "
                      "the constructor of '") +
          parentClass + "' has run")
      .str();
}

std::string ErrorTextReadyThis::unset(llvm::StringRef field) {
  return (llvm::Twine("'this' is used before every field is set: field '") + field +
          "' is not set yet; until then 'this' can only set fields "
          "and read the ones set")
      .str();
}

std::string ErrorTextAccess::elsewhere(llvm::StringRef name, llvm::StringRef cls,
                                       llvm::StringRef current, llvm::StringRef other) {
  return (llvm::Twine("'") + name + "' is protected in class '" + cls + "'; '" + current +
          "' may use it only on its own objects, and this one is of class '" + other + "'")
      .str();
}

std::string ErrorTextAccess::hiddenPrivate(llvm::StringRef name, llvm::StringRef word,
                                           llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is " + word + " in class '" + cls + "'").str();
}

std::string ErrorTextAccess::hiddenProtected(llvm::StringRef name, llvm::StringRef word,
                                             llvm::StringRef cls) {
  auto message = ErrorTextAccess::hiddenPrivate(name, word, cls);

  return message +
         (llvm::Twine("; only '") + cls + "' and the classes that extend it can use it").str();
}

std::string ErrorTextClassField::readEarly(llvm::StringRef name) {
  return (llvm::Twine("field '") + name + "' is read before it is set").str();
}

std::string ErrorTextClassField::laterType(llvm::StringRef name) {
  return (llvm::Twine("the type of field '") + name +
          "' comes from its initializer, which runs later; declare "
          "it: '" +
          name + ": T = ...'")
      .str();
}

std::string ErrorTextClassField::isStatic(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static member of '" + cls +
          "': use it through the class, '" + cls + "." + name + "'")
      .str();
}

std::string ErrorTextClassField::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextClassField::earlyThis(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'this.") + name + "' is used before every field is set: field '" + field +
          "' is not set yet")
      .str();
}

std::string ErrorTextClassField::genericVal(llvm::StringRef name) {
  return (llvm::Twine("'") + name +
          "' is a generic method, so it can only be called: each "
          "call runs a copy of it made for its types; wrap it in an arrow, e.g. "
          "'(x) => o." +
          name + "(x)'")
      .str();
}

std::string ErrorTextFieldTarget::statsDate(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' cannot be assigned: it is made from '" + name +
          "Ms' when it is read; keep the date in a variable of its own")
      .str();
}

std::string ErrorTextFieldTarget::setter(llvm::StringRef name, llvm::StringRef type,
                                         llvm::StringRef setter) {
  return (llvm::Twine("'") + name + "' is a method of '" + type + "'; change it with '" + setter +
          "(...)'")
      .str();
}

std::string ErrorTextFieldTarget::library(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is a method of '" + type +
          "' and cannot be assigned: the object changes "
          "what it gives itself")
      .str();
}

std::string ErrorTextFieldTarget::method(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("'") + name + "' is a method of '" + type +
          "' and cannot be replaced: a method belongs to the class, "
          "not to the object; to change what it does, declare a "
          "field that holds a function: '" +
          name + ": () => void = () => { ... }'")
      .str();
}

std::string ErrorTextFieldTarget::readOnly(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("cannot assign to '") + name + "' because it is a read-only field of '" +
          type + "'; only its constructor can set it")
      .str();
}

std::string ErrorTextClassRefMember::prototype() {
  return "there are no prototypes in tz: the methods belong to the class "
         "itself, and a decorator gets the class; use it as it is";
}

std::string ErrorTextClassRefMember::runtime(llvm::StringRef type) {
  return (llvm::Twine("'") + type +
          "' is some class known only when the program "
          "runs, which has only its 'name'; make an object of it with 'new', "
          "or use reflection (docs/decorators.md, section 6)")
      .str();
}

std::string ErrorTextClassRefMember::nameOnly(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' as a value has only its 'name' in tz").str();
}

std::string ErrorTextValueNew::notClass(llvm::StringRef type) {
  return (llvm::Twine("'new' makes an object of a class, and this is a '") + type + "'").str();
}

std::string ErrorTextValueNew::valueOnly(llvm::StringRef name) {
  return (llvm::Twine("'new' of '") + name + "' as a value is not supported; write 'new " + name +
          "(...)' or a literal")
      .str();
}

std::string ErrorTextValueNew::typeArgs() {
  return "a class known only when the program runs takes no type "
         "arguments";
}

std::string ErrorTextValueNew::voidArg() {
  return "an expression of type 'void' has no value to pass";
}

std::string ErrorTextDecoratedNames::shared(llvm::StringRef what, llvm::StringRef cls,
                                            llvm::StringRef records) {
  return (llvm::Twine(what) + " of '" + cls + "' are decorated, and " + records +
          " for both would be one: metadata is kept on the class "
          "itself, by the member's name (docs/decorators.md, section 3); "
          "give one of them another name")
      .str();
}

std::string ErrorTextDecoratedNames::bothKinds(llvm::StringRef name) {
  return (llvm::Twine("both the static member '") + name + "' and the member '" + name + "'").str();
}

std::string ErrorTextDecoratedNames::variants(llvm::StringRef name) {
  return (llvm::Twine("two variants of '") + name + "'").str();
}

std::string ErrorTextDecoratedNames::design() {
  return "the records 'design:*' the compiler writes";
}

std::string ErrorTextDecoratedNames::recordsOf(llvm::StringRef shared) {
  return (llvm::Twine("the records of '@") + shared + "'").str();
}

std::string ErrorTextDecoratedNames::other() {
  return "the other one is here";
}

std::string ErrorTextDecoratedNames::sameRecord(llvm::StringRef cls) {
  return (llvm::Twine("two constructors of '") + cls +
          "' have decorated parameters, and the record "
          "'design:paramtypes' the compiler writes for the class would be "
          "one: metadata is kept on the class itself (docs/decorators.md, "
          "section 3); decorate the parameters of one of them")
      .str();
}

std::string ErrorTextDecorate::accessor() {
  return "a decorator cannot decorate an 'accessor' field: in tz it is an ordinary "
         "field, and reading or writing it never runs code";
}

std::string ErrorTextDecorate::abstract() {
  return "a decorator cannot decorate an abstract method, which has no code; decorate "
         "the methods that implement it";
}

std::string ErrorTextDecorate::arguments(llvm::StringRef label, llvm::StringRef place,
                                         llvm::StringRef needs, llvm::StringRef given,
                                         llvm::StringRef args) {
  return (llvm::Twine(label) + " cannot decorate " + place + ": it takes " + needs +
          " arguments, and a decorator of " + place + " gets " + given + ", " + args)
      .str();
}

std::string ErrorTextDecorate::descriptor(llvm::StringRef label) {
  return (llvm::Twine(label) + " takes the descriptor of a method: it decorates methods, not "
                               "parameters")
      .str();
}

std::string ErrorTextDecorate::position(llvm::StringRef label) {
  return (llvm::Twine(label) + " takes the position of a parameter: it decorates parameters, not "
                               "methods")
      .str();
}

std::string ErrorTextDecorate::generic(llvm::StringRef cls) {
  return (llvm::Twine("a decorator that takes the descriptor of a method of a generic class is not "
                      "supported yet: each instance of '") +
          cls +
          "' has a method of its own; decorate a method of a class "
          "that is not generic, or take only the class and the name")
      .str();
}

std::string ErrorTextDecorate::metadata() {
  return "'Reflect.metadata' takes the key and the value of the record, e.g. "
         "'@Reflect.metadata(Role, \"admin\")'";
}

std::string ErrorTextDecoratorResult::runtime(llvm::StringRef label, llvm::StringRef result,
                                              llvm::StringRef current) {
  return (llvm::Twine(label) + " returns '" + result +
          "', some class known only when the program runs, and the class it "
          "gives in place of '" +
          current +
          "' must be known when compiling: make the decorator generic, "
          "'<T extends Constructor>(cls: T)', and return 'cls' or a class "
          "that extends it")
      .str();
}

std::string ErrorTextDecoratorResult::forClass(llvm::StringRef label, llvm::StringRef result) {
  return (llvm::Twine(label) + " returns '" + result +
          "': a class decorator gives back nothing, the class it got, or a "
          "class that extends it, which then replaces it")
      .str();
}

std::string ErrorTextDecoratorResult::forMethod(llvm::StringRef label, llvm::StringRef result) {
  return (llvm::Twine(label) + " returns '" + result +
          "': a decorator of a method returns nothing, or the descriptor "
          "it got")
      .str();
}

std::string ErrorTextDecoratorResult::other(llvm::StringRef label, llvm::StringRef result,
                                            llvm::StringRef place) {
  return (llvm::Twine(label) + " returns '" + result + "', which a decorator of " + place +
          " cannot give back; return nothing")
      .str();
}

std::string ErrorTextDecoratorPlace::aClass() {
  return "a class";
}

std::string ErrorTextDecoratorPlace::aField() {
  return "a field";
}

std::string ErrorTextDecoratorPlace::aMethod() {
  return "a method";
}

std::string ErrorTextDecoratorPlace::aParameter() {
  return "a parameter";
}

std::string ErrorTextDecoratorPlace::classArgs() {
  return "the class";
}

std::string ErrorTextDecoratorPlace::fieldArgs() {
  return "the class and the name of the field";
}

std::string ErrorTextDecoratorPlace::methodArgs() {
  return "the class, the name of the method and its descriptor";
}

std::string ErrorTextDecoratorPlace::paramArgs() {
  return "the class, the name of the method (undefined for a constructor) and the position of "
         "the parameter";
}

std::string ErrorTextDecoratorPlace::label(llvm::StringRef name) {
  if (name.empty()) {
    return "the decorator";
  }

  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextAbstract::needsClass(llvm::StringRef fn, llvm::StringRef cls) {
  return (llvm::Twine("abstract method '") + fn + "' needs an abstract class: '" + cls +
          "' can be created with 'new', and the method would "
          "have no body; write 'abstract class " +
          cls + "'")
      .str();
}

std::string ErrorTextAbstract::notImpl(llvm::StringRef cls, llvm::StringRef variant,
                                       llvm::StringRef owner) {
  return (llvm::Twine("class '") + cls + "' does not implement the abstract method " + variant +
          " of '" + owner + "'; implement it, or make '" + cls + "' abstract too")
      .str();
}

std::string ErrorTextImplements::prefix(llvm::StringRef cls, llvm::StringRef iface) {
  return (llvm::Twine("class '") + cls + "' implements '" + iface + "'").str();
}

std::string ErrorTextImplements::fieldType(llvm::StringRef prefix, llvm::StringRef name,
                                           llvm::StringRef type, llvm::StringRef cls,
                                           llvm::StringRef actual) {
  return (llvm::Twine(prefix) + ", whose field '" + name + "' is '" + type + "', but in '" + cls +
          "' it is '" + actual +
          "'; the types must be the same, or objects "
          "of a type down the chain of the interface's, such as 'Dog' for "
          "'Animal'")
      .str();
}

std::string ErrorTextImplements::noMember(llvm::StringRef prefix, llvm::StringRef name,
                                          bool optional, llvm::StringRef type) {
  return (llvm::Twine(prefix) + ", but has no '" + name + "'; declare it, e.g. '" + name +
          (optional ? "?" : "") + ": " + type + "'")
      .str();
}

std::string ErrorTextImplements::fieldNotFn(llvm::StringRef name, llvm::StringRef type,
                                            llvm::StringRef iface, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a field of type '" + type + "' in '" + iface +
          "', which '" + cls + "' implements, so it cannot be a method")
      .str();
}

std::string ErrorTextImplements::mustField(llvm::StringRef name, llvm::StringRef iface,
                                           llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a field in '" + iface +
          "' that code may replace, so in '" + cls +
          "', which implements it, it must be a field too: '" + name +
          " = (...) => ...'; or declare it in '" + iface + "' as a method, '" + name +
          "(...): ...'")
      .str();
}

std::string ErrorTextImplements::returns(llvm::StringRef name, llvm::StringRef ret,
                                         llvm::StringRef iface, llvm::StringRef cls,
                                         llvm::StringRef says) {
  return (llvm::Twine("'") + name + "' returns '" + ret + "', and '" + iface + "', which '" + cls +
          "' implements, says it returns '" + says + "'")
      .str();
}

std::string ErrorTextImplements::params(llvm::StringRef prefix, llvm::StringRef name,
                                        llvm::StringRef type, llvm::StringRef cls,
                                        llvm::StringRef variants) {
  return (llvm::Twine(prefix) + ", whose '" + name + "' is '" + type + "', but '" + cls + "." +
          name + "' takes other parameters: " + variants)
      .str();
}

std::string ErrorTextImplements::noVariant(llvm::StringRef prefix, llvm::StringRef name,
                                           llvm::StringRef signature) {
  return (llvm::Twine(prefix) + ", but has no variant of '" + name + "' for its signature '" +
          signature + "'; each signature needs a variant with these parameters")
      .str();
}

std::string ErrorTextSuperCall::outside() {
  return "'super(...)' can be called only in the constructor of a class that "
         "extends another";
}

std::string ErrorTextSuperCall::noParent(llvm::StringRef cls) {
  return (llvm::Twine("'super(...)' calls the constructor of the class that '") + cls +
          "' extends, and it extends none")
      .str();
}

std::string ErrorTextSuperCall::statement() {
  return "'super(...)' must be a statement of its own in the body of the "
         "constructor, not inside a block, a condition, a loop or another "
         "expression: the fields are set right after it";
}

std::string ErrorTextSuperCall::twice(llvm::StringRef parent) {
  return (llvm::Twine("'super(...)' is called a second time; the constructor of '") + parent +
          "' runs once")
      .str();
}

std::string ErrorTextSuperCall::noArgs(llvm::StringRef parent) {
  return (llvm::Twine("class '") + parent +
          "' has no constructor, so "
          "'super()' takes no arguments")
      .str();
}

std::string ErrorTextSuperMethod::outside() {
  return "'super' is the class that the class of the method "
         "extends; it exists in the methods of a class that "
         "extends another";
}

std::string ErrorTextSuperMethod::early(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'super.") + name + "()' is called before every field is set: field '" +
          field + "' is not set yet, and the method may read it")
      .str();
}

std::string ErrorTextSuperMethod::isField(llvm::StringRef name, llvm::StringRef parent) {
  return (llvm::Twine("'") + name + "' is a field of '" + parent +
          "'; call the function it holds as 'this." + name + "(...)'")
      .str();
}

std::string ErrorTextSuperMethod::noMethod(llvm::StringRef parent, llvm::StringRef name) {
  return (llvm::Twine("class '") + parent + "' has no method '" + name + "'").str();
}

std::string ErrorTextSuperMethod::abstract(llvm::StringRef name, llvm::StringRef owner) {
  return (llvm::Twine("'") + name + "' is abstract in '" + owner + "': it has no body to call")
      .str();
}

std::string ErrorTextInstanceOf::untypedMap(llvm::StringRef builtin, llvm::StringRef example) {
  return (llvm::Twine("'instanceof ") + builtin +
          "' cannot tell the types of the keys and values, which "
          "tz needs to use a " +
          builtin + "; keep it in a variable of its type, e.g. '" + example + "'")
      .str();
}

std::string ErrorTextInstanceOf::noneIs(llvm::StringRef base, llvm::StringRef what) {
  return (llvm::Twine("a '") + base + "' is never a " + what +
          ": none of its members is one, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::allAre(llvm::StringRef base, llvm::StringRef what,
                                        llvm::StringRef name) {
  return (llvm::Twine("every member of '") + base + "' is a " + what + ", so 'instanceof " + name +
          "' is always true")
      .str();
}

std::string ErrorTextInstanceOf::alwaysTrue(llvm::StringRef type, llvm::StringRef name) {
  return (llvm::Twine("the value is a '") + type + "', so 'instanceof " + name + "' is always true")
      .str();
}

std::string ErrorTextInstanceOf::orUndef(llvm::StringRef type) {
  return (llvm::Twine("the value is a '") + type + "' or undefined; test it with '!= undefined'")
      .str();
}

std::string ErrorTextInstanceOf::neverIs(llvm::StringRef type, llvm::StringRef what,
                                         llvm::StringRef name) {
  return (llvm::Twine("a '") + type + "' is never a " + what + ", so 'instanceof " + name +
          "' is always false")
      .str();
}

std::string ErrorTextInstanceOf::notObject(llvm::StringRef type) {
  return (llvm::Twine("'instanceof' tests an object, found '") + type +
          "', which is never an instance of a class")
      .str();
}

std::string ErrorTextInstanceOf::notClass() {
  return "the right side of 'instanceof' must be a class";
}

std::string ErrorTextInstanceOf::cantTell(llvm::StringRef generic, llvm::StringRef base,
                                          llvm::StringRef hint) {
  return (llvm::Twine("'instanceof ") + generic + "' cannot tell which '" + generic + "' a '" +
          base +
          "' would be: each list of type "
          "arguments of '" +
          generic + "' makes a class of its own; " + hint)
      .str();
}

std::string ErrorTextInstanceOf::extendHint(llvm::StringRef generic) {
  return (llvm::Twine("test for a class that extends it, e.g. 'x instanceof Bus' with 'class Bus "
                      "extends ") +
          generic + " {}'")
      .str();
}

std::string ErrorTextInstanceOf::unionHint(llvm::StringRef generic, llvm::StringRef other) {
  return (llvm::Twine("keep the value in a variable of a union of the classes it may be, e.g. '") +
          generic + "<int> | " + other + "'")
      .str();
}

std::string ErrorTextInstanceOf::neverThat(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls + "', so this is always false").str();
}

std::string ErrorTextInstanceOf::neverUnion(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls +
          "': none of its members is or extends it, so this is always "
          "false")
      .str();
}

std::string ErrorTextInstanceOf::neverClass(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls +
          "': classes are told apart by their names, and neither extends "
          "the other, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::notImpl(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("a '") + base + "' is never a '" + cls + "': '" + cls +
          "' does not implement it, so this is always false")
      .str();
}

std::string ErrorTextInstanceOf::neverMade(llvm::StringRef base, llvm::StringRef cls) {
  return (llvm::Twine("an object of type '") + base + "' is never a '" + cls + "': only 'new " +
          cls + "(...)' creates one, so this is always false")
      .str();
}

std::string ErrorTextFieldCall::undefined() {
  return "the function may be undefined; check it first, or call it with "
         "'?.()', which gives undefined then";
}

std::string ErrorTextFieldCall::notFunc(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("field '") + name + "' is not a function; it has type '" + type + "'").str();
}

std::string ErrorTextMethodLookup::isStatic(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static method of '" + cls +
          "': call it through the class, '" + cls + "." + name + "(...)'")
      .str();
}

std::string ErrorTextMethodLookup::noProperty(llvm::StringRef name, llvm::StringRef type) {
  return (llvm::Twine("property '") + name + "' does not exist on type '" + type + "'").str();
}

std::string ErrorTextUnsetCall::beforeSet(llvm::StringRef name, llvm::StringRef field) {
  return (llvm::Twine("'this.") + name + "()' is called before every field is set: field '" +
          field + "' is not set yet, and the method may read it")
      .str();
}

std::string ErrorTextMethodTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' is not a generic method; remove the type arguments").str();
}

std::string ErrorTextRuntimeNew::onlyFs(llvm::StringRef name) {
  return (llvm::Twine("'new ") + name + "' is not possible: only the functions of node:fs make " +
          name)
      .str();
}

std::string ErrorTextClassTypeArgs::notGeneric(llvm::StringRef name) {
  return (llvm::Twine("class '") + name + "' is not generic; remove the type arguments").str();
}

std::string ErrorTextClassOrder::usedEarly(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is used before its declaration, which JavaScript has not run "
          "yet here (a ReferenceError); move the class up")
      .str();
}

std::string ErrorTextAbstractNew::abstract(llvm::StringRef name) {
  return (llvm::Twine("class '") + name +
          "' is abstract and cannot be created: create "
          "an object of a class that extends it")
      .str();
}

std::string ErrorTextNoConstructor::noArgs(llvm::StringRef name) {
  return (llvm::Twine("class '") + name + "' has no constructor, so 'new " + name +
          "()' takes no arguments")
      .str();
}

std::string ErrorTextVariant::ctorOf(llvm::StringRef cls) {
  return (llvm::Twine("the constructor of '") + cls + "'").str();
}

std::string ErrorTextVariant::quoted(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "'").str();
}

std::string ErrorTextVariant::spread(llvm::StringRef what) {
  return (llvm::Twine("a spread argument cannot choose among the variants of ") + what +
          "; pass the arguments one by one")
      .str();
}

std::string ErrorTextVariant::count(llvm::StringRef what, size_t count, llvm::StringRef list) {
  return (llvm::Twine("no variant of ") + what + " takes " + std::to_string(count) + " argument" +
          (count == 1 ? "" : "s") + ": there are " + list)
      .str();
}

std::string ErrorTextVariant::typeParams(llvm::StringRef variant) {
  return (llvm::Twine("; ") + variant + " cannot take these arguments for its type parameters")
      .str();
}

std::string ErrorTextVariant::mismatch(llvm::StringRef variant, llvm::StringRef param, size_t index,
                                       llvm::StringRef type) {
  return (llvm::Twine(variant) + " takes '" + param + "' as argument " + std::to_string(index) +
          ", which is '" + type + "'")
      .str();
}

std::string ErrorTextVariant::noFit(llvm::StringRef what, llvm::StringRef why,
                                    llvm::StringRef hint) {
  return (llvm::Twine("no variant of ") + what + " fits these arguments" + why + hint).str();
}

std::string ErrorTextVariant::generic(llvm::StringRef variant) {
  return (llvm::Twine("the call chooses ") + variant + ", a generic variant, which " +
          "cannot be chosen here")
      .str();
}

std::string ErrorTextVariant::tie(llvm::StringRef what, llvm::StringRef list, bool literals) {
  return (llvm::Twine("the call fits several variants of ") + what + " equally well: " + list +
          (literals ? "; a number literal fits each of them: write it so "
                      "that it has the type of one, e.g. '2.0' for a "
                      "double, or give it a type through a constant, "
                      "'const k: int32 = 2'"
                    : "; convert an argument to choose one"))
      .str();
}

std::string ErrorTextVariant::text(const FunctionDecl *fn) {
  std::string text = (fn->isConstructor() ? llvm::StringRef("constructor") : fn->name).str() + "(";
  const FuncSymbol *sym = fn->sym;
  for (size_t i = 0; i < fn->params.size(); ++i) {
    if (i > 0) {
      text += ", ";
    }
    const Param &param = fn->params[i];
    text += (param.rest() ? "..." : "") + param.label().str();
    const Type *type = sym != nullptr && i < sym->params.size() ? sym->params[i] : nullptr;
    if (type == nullptr) {
      continue;
    }
    if (type->isOptional() && !param.rest() &&
        (param.optional() || param.defaultValue() != nullptr)) {
      text += "?: " + type->inner()->name().str();
    } else {
      text += ": " + type->name().str();
    }
  }

  return text + ")";
}

std::string ErrorTextVariant::list(llvm::ArrayRef<FunctionDecl *> variants) {
  auto text = std::string();
  for (size_t i = 0; i < variants.size(); ++i) {
    text += (i == 0                     ? ""
             : i + 1 == variants.size() ? " and "
                                        : ", ") +
            ErrorTextVariant::text(variants[i]);
  }

  return text;
}

std::string ErrorTextVariantType::several(llvm::StringRef name, llvm::StringRef list,
                                          llvm::StringRef type, llvm::StringRef object) {
  return (llvm::Twine("'") + name + "' has several variants, " + list +
          ": say which one with the type of the place it goes, e.g. "
          "'const f: " +
          type + " = " + object + "." + name + "'")
      .str();
}

std::string ErrorTextStaticOrder::usedEarly(llvm::StringRef var, llvm::StringRef cls) {
  return (llvm::Twine("'") + var + "' is used before class '" + cls +
          "' is declared, which sets it")
      .str();
}

std::string ErrorTextStaticOrder::beforeSet(llvm::StringRef var) {
  return (llvm::Twine("'") + var +
          "' is read before it is set: the static "
          "fields are set in the order they are declared")
      .str();
}

std::string ErrorTextStaticLater::typeOf(llvm::StringRef var, llvm::StringRef field) {
  return (llvm::Twine("the type of '") + var +
          "' comes from its value, which is set later; declare it: "
          "'static " +
          field + ": T = ...'")
      .str();
}

std::string ErrorTextStaticMember::instance(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a member of the instances of '" + cls +
          "', not of the class: create one with 'new " + cls + "(...)'")
      .str();
}

std::string ErrorTextStaticMember::noMember(llvm::StringRef cls, llvm::StringRef name) {
  return (llvm::Twine("class '") + cls + "' has no static member '" + name + "'").str();
}

std::string ErrorTextStaticField::isMethod(llvm::StringRef name, llvm::StringRef cls) {
  return (llvm::Twine("'") + name + "' is a static method of '" + cls + "' and cannot be replaced")
      .str();
}

std::string ErrorTextStaticField::noField(llvm::StringRef cls, llvm::StringRef name) {
  return (llvm::Twine("class '") + cls + "' has no static field '" + name + "'").str();
}

std::string ErrorTextStaticField::readOnly(llvm::StringRef var) {
  return (llvm::Twine("cannot assign to '") + var + "' because it is a read-only static field")
      .str();
}

std::string ErrorTextStaticCall::notFunc(llvm::StringRef cls, llvm::StringRef name,
                                         llvm::StringRef type) {
  return (llvm::Twine("'") + cls + "." + name + "' is not a function; it has type '" + type + "'")
      .str();
}

std::string ErrorTextBaseClass::atRun(llvm::StringRef type) {
  return (llvm::Twine("the class extended must be known when compiling: the fields of an object "
                      "lie at places fixed then, and '") +
          type +
          "' is some class known only when the program runs (docs/decorators.md, section 5); "
          "extend a class by its name, or take it in a generic function, 'function Mixin<B "
          "extends Constructor>(Base: B)'")
      .str();
}

std::string ErrorTextBaseClass::notClass(llvm::StringRef type) {
  return (llvm::Twine("a class can extend only a class, and this is a '") + type + "'").str();
}

std::string ErrorTextBaseClass::javascript(llvm::StringRef className) {
  return (llvm::Twine("a class cannot extend '") + className +
          "' of JavaScript; extend a class of the program")
      .str();
}

std::string ErrorTextBaseClass::generic(llvm::StringRef className) {
  return (llvm::Twine("'") + className + "' is generic; extend one of its instances, e.g. '" +
          className + "<int>'")
      .str();
}

std::string ErrorTextBaseClass::madeByFs(llvm::StringRef className) {
  return (llvm::Twine("a class cannot extend '") + className +
          "': only the functions of node:fs make its objects")
      .str();
}

std::string ErrorTextStaticType::unknown(llvm::StringRef field) {
  return (llvm::Twine("cannot infer the type of static field '") + field +
          "' from this value; declare it, e.g. 'static " + field + ": int | undefined = undefined'")
      .str();
}

std::string ErrorTextStaticType::fieldLabel(llvm::StringRef name) {
  return (llvm::Twine("static field '") + name + "'").str();
}

std::string ErrorTextOldObject::held(llvm::StringRef className, llvm::StringRef field) {
  return (llvm::Twine("the decorator gives '") + className +
          "' another class in its place, and static field '" + field +
          "' holds an object made before, of the class as declared; make that object where it "
          "is used, or keep it outside the class")
      .str();
}

std::string ErrorTextOldObject::decorator() {
  return "the decorator is here";
}

std::string ErrorTextReplacedField::clash(llvm::StringRef child, llvm::StringRef className,
                                          llvm::StringRef field) {
  return (llvm::Twine("'") + child + "' extends '" + className +
          "', whose decorator gives a class with a field '" + field +
          "' of its own in its place; give this field another name")
      .str();
}

std::string ErrorTextOverriddenCall::overridden(llvm::StringRef caller, llvm::StringRef method,
                                                llvm::StringRef path, llvm::StringRef child) {
  return (llvm::Twine(caller) + " calls '" + method + "'" + path + ", which '" + child +
          "' overrides: it would run before the fields of '" + child +
          "' are set; call it once the object is made")
      .str();
}

std::string ErrorTextOverriddenCall::ctorCall(llvm::StringRef className, llvm::StringRef method,
                                              llvm::StringRef child) {
  std::string caller = (llvm::Twine("the constructor of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, "", child);
}

std::string ErrorTextOverriddenCall::ctorReach(llvm::StringRef className, llvm::StringRef method,
                                               llvm::StringRef child) {
  std::string caller = (llvm::Twine("the constructor of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, " (through the methods it calls)",
                                             child);
}

std::string ErrorTextOverriddenCall::fieldsCall(llvm::StringRef className, llvm::StringRef method,
                                                llvm::StringRef child) {
  std::string caller = (llvm::Twine("the initializers of the fields of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, "", child);
}

std::string ErrorTextOverriddenCall::fieldsReach(llvm::StringRef className, llvm::StringRef method,
                                                 llvm::StringRef child) {
  std::string caller = (llvm::Twine("the initializers of the fields of '") + className + "'").str();

  return ErrorTextOverriddenCall::overridden(caller, method, " (through the methods it calls)",
                                             child);
}

}
