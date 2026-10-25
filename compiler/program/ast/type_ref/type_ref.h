#pragma once

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/SMLoc.h"
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tz {

// A type written in the source: `int`, `string[]`, `int | undefined`,
// `Array<int>`, `{ x: int; y?: string }`, `[string, int]`.
struct TypeRef {
  enum class Kind {
    Name,     // `name`; empty after a syntax error
    Array,    // `args[0][]`
    Union,    // `args[0] | args[1] | ...`
    Generic,  // `name<args...>`
    Function, // `(a: args[0], b?: args[1]) => args.back()`
    Object,   // `{ names[0]: args[0]; names[1]?: args[1] }`
    Tuple,    // `[args[0], args[1]]`; `readonly` before it makes every element read-only
    // `"circle"`, `0`, `-1.5`, `true`: the type of one value. `name` is
    // "string", "number" or "boolean"; the value is in `text` or `number`.
    Literal,
    // `x is args[0]`, `asserts x is args[0]` and `asserts x` (no args), in
    // a return type: `name` is the parameter, or "this".
    Predicate,
    // Computations over types (docs/generics.md, section 10): `keyof
    // args[0]`; `args[0][args[1]]`; `typeof names[0].names[1]...`; and a
    // mapped type `{ [name in args[0]]: args[1] }`, whose modifiers are in
    // `mappedOptional` and `mappedReadonly`, and whose keys may be renamed,
    // `[name in args[0] as args[2]]`.
    KeyOf,
    Indexed,
    TypeOf,
    Mapped,
    // `args[0] extends args[1] ? args[2] : args[3]`; `source` is the text
    // written, which names the type in messages.
    Conditional,
    // `infer name` in the pattern of a conditional type, with its
    // constraint in args[0] if it has one (`infer N extends int`).
    Infer,
    // `` `on${args[0]}` ``: the texts around the types are in `texts`, one
    // more of them than of the types.
    Template,
    // `args[0] & args[1] & ...`
    Intersection,
  };

  Kind kind = Kind::Name;
  llvm::StringRef name;
  llvm::SMLoc loc;
  std::vector<TypeRef> args;
  // Function types: which parameters are optional, and whether the last one
  // is a rest parameter. Object types: which fields are optional.
  std::vector<bool> optional;
  bool rest = false;
  // Object types: the names of the fields (function types: of the
  // parameters, of which `this` is the `this` the function gets), where they
  // are written, and which are `readonly`. A method, `area(): double`, is a
  // read-only field that
  // holds a function (docs/objects.md, section 5); `methods` marks those
  // written so.
  std::vector<llvm::StringRef> names;
  std::vector<llvm::SMLoc> locs;
  std::vector<bool> readonly;
  std::vector<bool> methods;
  // Tuple types: the elements spread into it, `[int, ...A]`, a list of the
  // types of the parameters of a function (docs/generics.md, section 9).
  std::vector<bool> spread;
  // Mapped types: `?` added (+1) or removed (-1, `-?`) on every field, and
  // the same for `readonly`; 0 keeps them.
  int mappedOptional = 0;
  int mappedReadonly = 0;
  // Literal types: the value (1 or 0 for a boolean), and whether a number is
  // written without a fraction. Predicates: whether it is `asserts`.
  std::u16string text;
  double number = 0;
  bool integer = false;
  bool asserts = false;
  // Template literal types: the texts (see Kind::Template).
  std::vector<std::u16string> texts;
  llvm::StringRef source; // of a conditional type
};

} // namespace tz
