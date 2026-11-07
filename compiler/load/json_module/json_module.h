#pragma once

#include "llvm/ADT/StringRef.h"
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace tz {

namespace pkg {
struct JsonValue;
}

inline constexpr llvm::StringLiteral kJsonExport = "json";

class JsonModule {
public:
  static bool jsonModuleText(llvm::StringRef json, llvm::StringRef exported, std::string &out,
                             size_t &errorOffset, std::string &error);

private:
  struct Shape {
    enum class Kind { Unknown, Json, Bool, Int, Double, String, Array, Object };
    Kind kind = Kind::Unknown;
    std::vector<Shape> items;
    std::vector<std::string> keys;
    std::vector<bool> optional;
  };

  using Fields = std::vector<std::pair<std::string, const pkg::JsonValue *>>;

  static Fields fieldsOf(const pkg::JsonValue &object);
  static bool integral(llvm::StringRef raw);
  static Shape join(const Shape &a, const Shape &b);
  static Shape shapeOf(const pkg::JsonValue &value, llvm::StringRef text);
  static std::string typeText(const Shape &shape);
  static std::string jsonText(const pkg::JsonValue &value, llvm::StringRef text);
  static std::string valueText(const pkg::JsonValue &value, const Shape &shape,
                               llvm::StringRef text);
};

}
