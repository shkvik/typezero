#include "compiler/semantic/code/functions/code_functions_registry_errors.h"
#include "llvm/ADT/Twine.h"

namespace tz {

std::string ErrorTextFunctionBody::noSuper(llvm::StringRef cls, llvm::StringRef parent) {
  return (llvm::Twine("the constructor of '") + cls + "' must call 'super(...)': '" + cls +
          "' extends '" + parent + "', whose constructor sets the fields it declares")
      .str();
}

std::string ErrorTextFunctionBody::forward(llvm::StringRef label, llvm::StringRef used) {
  return (llvm::Twine("the default value of '") + label + "' uses '" + used +
          "', which is not set yet: a default value can "
          "use only the parameters before it")
      .str();
}

std::string ErrorTextFunctionBody::noReturn(llvm::StringRef label, llvm::StringRef type) {
  return (llvm::Twine(label) + " must return a value of type '" + type + "' on every path").str();
}

std::string ErrorTextFunctionName::quoted(llvm::StringRef name) {
  return name.empty() ? "the function" : "'" + name.str() + "'";
}

std::string ErrorTextFunctionName::label(llvm::StringRef name) {
  return name.empty() ? "the function" : "function '" + name.str() + "'";
}

std::string ErrorTextNotGeneric::generic() {
  return "a generic arrow or function expression can only be the value of a top-level "
         "constant for now, e.g. 'const f = <T>(x: T) => x'; or declare a generic function "
         "at the top level: 'function f<T>(x: T) { ... }'";
}

std::string ErrorTextParamAgrees::declared(llvm::StringRef label, llvm::StringRef passed,
                                           llvm::StringRef given) {
  return (llvm::Twine("parameter '") + label + "' is declared as '" + passed +
          "', but the function gets '" + given + "' here")
      .str();
}

std::string ErrorTextDefaultMatters::unused(llvm::StringRef label) {
  return (llvm::Twine("the default value of '") + label +
          "' is never used: the function always gets a value here")
      .str();
}

std::string ErrorTextParamTypeKnown::needsType(llvm::StringRef label) {
  return (llvm::Twine("parameter '") + label +
          "' needs a type: nothing here says what the function is called with")
      .str();
}

std::string ErrorTextThisParam::arrowThis() {
  return "an arrow function cannot have a 'this' parameter: it sees the "
         "'this' of the function around it; write a 'function'";
}

std::string ErrorTextRestParam::notArray(llvm::StringRef name) {
  return (llvm::Twine("a rest parameter must be an array, e.g. '...") + name + ": int[]'").str();
}

std::string ErrorTextParamOrder::order() {
  return "a required parameter cannot follow an optional one";
}

std::string ErrorTextPromiseAwaited::dropped() {
  return "the Promise of this async function is dropped here, so nothing "
         "waits for it to finish; loop with 'for (const x of xs) await "
         "f(x)', or wait for all with 'await Promise.all(xs.map(async x "
         "=> ...))'";
}

std::string ErrorTextCallbackArg::notFunc(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' needs a function, found '" + type + "'").str();
}

std::string ErrorTextCallbackArg::rest(llvm::StringRef name) {
  return (llvm::Twine(name) + " takes a rest parameter; pass an arrow that calls it instead").str();
}

std::string ErrorTextCallbackArg::paramType(llvm::StringRef name, llvm::StringRef param,
                                            llvm::StringRef ordinal, llvm::StringRef method,
                                            llvm::StringRef given) {
  return (llvm::Twine(name) + " takes '" + param + "' as its " + ordinal + " argument, but '" +
          method + "' passes '" + given + "'")
      .str();
}

std::string ErrorTextCallbackArg::extraParam(llvm::StringRef name, llvm::StringRef argName,
                                             llvm::StringRef paramName, llvm::StringRef method,
                                             llvm::StringRef args) {
  return (llvm::Twine(name) + " would also get the " + argName + " in its optional parameter" +
          paramName + ": '" + method + "' calls it with " + args +
          "; pass an arrow that gives it only what it needs")
      .str();
}

std::string ErrorTextCallbackArg::tooFew(llvm::StringRef method, llvm::StringRef passes,
                                         llvm::StringRef name, llvm::StringRef needs) {
  return (llvm::Twine("'") + method + "' passes " + passes + ", but " + name + " needs " + needs)
      .str();
}

std::string ErrorTextCallbackArg::badReturn(llvm::StringRef method, llvm::StringRef wanted,
                                            llvm::StringRef name, llvm::StringRef returns) {
  return (llvm::Twine("'") + method + "' needs a function that returns '" + wanted + "', but " +
          name + " returns '" + returns + "'")
      .str();
}

std::string ErrorTextCallbackArg::noCheck(llvm::StringRef name, llvm::StringRef method) {
  return (llvm::Twine(name) + " returns a Result that '" + method +
          "' drops, so an error in it would go unchecked; pass an arrow "
          "that handles it, e.g. with '.match(...)'")
      .str();
}

std::string ErrorTextCallbackArg::noWait(llvm::StringRef name, llvm::StringRef method) {
  return (llvm::Twine(name) + " returns a Promise that '" + method +
          "' drops, so nothing waits for it to finish; loop with 'for "
          "(const x of xs) await f(x)', or wait for all with 'await "
          "Promise.all(xs.map(...))'")
      .str();
}

std::string ErrorTextCallbackArg::argsShape(llvm::StringRef method) {
  auto callback = ArrayMethodTable::findCallback(method);
  if (callback == ArrayCallback::REDUCE) {
    return "(accumulator, value, index, array)";
  }
  if (callback == ArrayCallback::COMPARE) {
    return "(a, b)";
  }

  return "(value, index, array)";
}

std::string ErrorTextCallbackArg::position(size_t index) {
  static const char *const names[] = {"first", "second", "third", "fourth", "fifth"};

  return index < 5 ? names[index] : "next";
}

std::string ErrorTextCallbackArg::argName(llvm::StringRef method, size_t index) {
  auto callback = ArrayMethodTable::findCallback(method);
  if (callback == ArrayCallback::REDUCE) {
    return index == 0 ? "accumulator" : index == 1 ? "value" : index == 2 ? "index" : "array";
  }

  return index == 0 ? "value" : index == 1 ? "index" : "array";
}

std::string ErrorTextCallbackArg::paramName(llvm::StringRef name) {
  return (llvm::Twine(" '") + name + "'").str();
}

std::string ErrorTextCallbackMethod::noFunction(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' needs a function").str();
}

std::string ErrorTextCallbackMethod::thisArg(llvm::StringRef name) {
  return (llvm::Twine("the second argument of '") + name +
          "' sets 'this' for the callback, which is not supported")
      .str();
}

std::string ErrorTextCallbackMethod::untyped(llvm::StringRef name) {
  return (llvm::Twine("the initial value of '") + name +
          "' needs a type; declare it in a "
          "variable first, e.g. 'const start: int | undefined = undefined'")
      .str();
}

std::string ErrorTextCallbackMethod::noValue(llvm::StringRef name) {
  return (llvm::Twine("the callback of '") + name + "' must return a value").str();
}

std::string ErrorTextCallbackMethod::notNumber(llvm::StringRef type) {
  return (llvm::Twine("a comparison function must return a number (negative, zero "
                      "or positive), found '") +
          type + "'")
      .str();
}

std::string ErrorTextUseOrder::early(llvm::StringRef function, llvm::StringRef variable) {
  return (llvm::Twine("'") + function + "' uses '" + variable +
          "' before it is declared: JavaScript would throw a ReferenceError when it "
          "runs here; move the declaration of '" +
          variable + "' up")
      .str();
}

std::string ErrorTextUseOrder::declared(llvm::StringRef variable) {
  return (llvm::Twine("'") + variable + "' is declared here").str();
}

std::string ErrorTextThisCall::noBind() {
  return "'bind' is not supported: pass the 'this' with every call, "
         "'f.call(obj, ...)'";
}

std::string ErrorTextThisCall::noProperty(llvm::StringRef name) {
  return (llvm::Twine("a function has no property '") + name +
          "'; call it with '.call(obj, ...)' or '.apply(obj, args)'")
      .str();
}

std::string ErrorTextThisCall::thisFirst(llvm::StringRef name) {
  return (llvm::Twine("'") + name + "' takes the 'this' of the call first, e.g. 'f." + name +
          "(this" + (name == "apply" ? ", args)'" : ", ...)'"))
      .str();
}

std::string ErrorTextThisCall::applyArgs() {
  return "'apply' takes two arguments: the 'this' of the call and the list "
         "of the arguments, e.g. 'f.apply(this, args)'";
}

std::string ErrorTextListMethod::noMethod(llvm::StringRef name) {
  return (llvm::Twine("a list of parameters has no method '") + name +
          "' here; it has 'length' and 'join', and goes on with "
          "'...args'")
      .str();
}

std::string ErrorTextExtraArguments::passed(llvm::StringRef function, llvm::StringRef argument,
                                            llvm::StringRef ordinal, llvm::StringRef method,
                                            llvm::StringRef passes) {
  return (llvm::Twine("'") + function + "' would also get the " + argument + " as its " + ordinal +
          " argument: '" + method + "' calls it with " + passes +
          "; pass only what it needs: 'x => " + function + "(x)'")
      .str();
}

std::string ErrorTextAsyncCallback::replace(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; JavaScript would insert \"[object "
          "Promise]\" "
          "for each match: work out the replacements first, then replace with a function that is "
          "not async")
      .str();
}

std::string ErrorTextAsyncCallback::test(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; a Promise is always truthy, so JavaScript "
          "would take every element as passing: await the tests first, e.g. 'const passed = await "
          "Promise.all(xs.map(async x => ...))', then test 'passed[i]'")
      .str();
}

std::string ErrorTextAsyncCallback::value(llvm::StringRef method, llvm::StringRef type) {
  return (llvm::Twine("'") + method + "' uses what the function returns as '" + type +
          "', but an async function returns a Promise; drop 'async', or await what the function "
          "needs before the call")
      .str();
}

std::string ErrorTextRestList::spreadOnly(llvm::StringRef list, llvm::StringRef type) {
  return (llvm::Twine("'") + list + "' of the parameters '" + type +
          "', which end with a rest parameter, can only be spread, '..." + list + "'")
      .str();
}

std::string ErrorTextListArgument::tooMany(llvm::StringRef list) {
  return (llvm::Twine("too many arguments for the parameters '") + list + "'").str();
}

std::string ErrorTextListArgument::spreadWrong(llvm::StringRef spread, llvm::StringRef list,
                                               llvm::StringRef part, bool partIsList) {
  return (llvm::Twine("'...") + spread + "' is spread where the parameters '" + list + "' take " +
          (partIsList ? "'..." : "'") + part + "'")
      .str();
}

std::string ErrorTextListArgument::notSpread(llvm::StringRef part) {
  return (llvm::Twine("the arguments for the parameters '") + part +
          "' of some function are spread from a list of them, e.g. "
          "'...args'")
      .str();
}

std::string ErrorTextListFilled::missing(llvm::StringRef list, llvm::StringRef part,
                                         bool partIsList) {
  return (llvm::Twine("the parameters '") + list + "' take " + (partIsList ? "'..." : "'") + part +
          "' too, after these arguments")
      .str();
}

}
