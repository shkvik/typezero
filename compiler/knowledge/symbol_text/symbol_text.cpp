#include "compiler/knowledge/symbol_text/symbol_text.h"
#include "compiler/program/ast/ast.h"
#include "compiler/program/types/type/type.h"

namespace tz {

namespace {

std::string typeName(const Type *type) { return type ? type->name().str() : "?"; }

std::string modifiers(Access access, bool isStatic) {
  std::string text;
  if (access == Access::Private) {
    text += "private ";
  } else if (access == Access::Protected) {
    text += "protected ";
  }
  if (isStatic) {
    text += "static ";
  }

  return text;
}

bool opens(char c) { return c == '(' || c == '[' || c == '{'; }

bool closes(char c) { return c == ')' || c == ']' || c == '}'; }

bool continues(char c) {
  return c == '|' || c == '&' || c == '.' || c == '=' || c == '?' || c == ':' || c == ',' ||
         c == '+' || c == '-' || c == '*' || c == '/' || c == '>';
}

}

std::string SymbolText::collapse(llvm::StringRef text, size_t limit) {
  std::string out;
  bool space = false;
  for (char c : text) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      space = !out.empty();
      continue;
    }
    if (space) {
      out.push_back(' ');
      space = false;
    }
    out.push_back(c);
  }
  if (out.size() > limit) {
    size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out = out.substr(0, cut) + "...";
  }

  return out;
}

const char *SymbolText::statementEnd(const char *at, const char *end) {
  int depth = 0;
  char last = 0;
  while (at < end) {
    char c = *at;
    if (c == '"' || c == '\'' || c == '`') {
      ++at;
      while (at < end && *at != c) {
        if (*at == '\\') {
          ++at;
        }
        ++at;
      }
      last = c;
      ++at;
      continue;
    }
    if (c == '/' && at + 1 < end && at[1] == '/') {
      while (at < end && *at != '\n') {
        ++at;
      }
      continue;
    }
    if (opens(c)) {
      ++depth;
    } else if (closes(c)) {
      --depth;
      if (depth < 0) {
        return at;
      }
      if (depth == 0 && c == '}') {
        const char *next = at + 1;
        while (next < end && (*next == ' ' || *next == '\t' || *next == '\r')) {
          ++next;
        }
        if (next == end || *next == '\n') {
          return at + 1;
        }
      }
    } else if (c == ';' && depth == 0) {
      return at + 1;
    } else if (c == '\n' && depth == 0) {
      const char *next = at + 1;
      while (next < end && (*next == ' ' || *next == '\t' || *next == '\r' || *next == '\n')) {
        ++next;
      }
      bool more = next < end && continues(*next);
      if (!more && !continues(last)) {
        return at;
      }
    }
    if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
      last = c;
    }
    ++at;
  }

  return end;
}

std::string SymbolText::function(const FunctionDecl &fn) {
  std::string text;
  if (fn.cls) {
    text += modifiers(fn.access, fn.isStatic);
  }
  if (fn.isAsync) {
    text += "async ";
  }
  if (!fn.cls) {
    text += fn.isGenerator ? "function* " : "function ";
  } else if (fn.isGenerator) {
    text += "*";
  }
  text += fn.isConstructor ? std::string("constructor") : fn.name.str();
  if (!fn.typeParams.empty()) {
    text += "<";
    for (size_t i = 0; i < fn.typeParams.size(); ++i) {
      text += (i ? ", " : "") + fn.typeParams[i].name.str();
    }
    text += ">";
  }
  text += "(";
  const FuncSymbol *sym = fn.sym;
  size_t offset =
      sym && sym->params.size() > fn.params.size() ? sym->params.size() - fn.params.size() : 0;
  for (size_t i = 0; i < fn.params.size(); ++i) {
    const Param &param = fn.params[i];
    const Type *type = param.declaredType;
    if (!type && sym && i + offset < sym->params.size()) {
      type = sym->params[i + offset];
    }
    if (type && param.optional && type->isOptional()) {
      type = type->nonOptional();
    }
    text += i ? ", " : "";
    text += param.rest ? "..." : "";
    text += param.label().str();
    text += param.optional || param.defaultValue ? "?" : "";
    text += ": " + typeName(type);
  }
  text += ")";
  if (!fn.isConstructor && sym && sym->ret) {
    text += ": " + typeName(sym->ret);
  }

  return text;
}

std::string SymbolText::field(const ClassField &field) {
  std::string text = modifiers(field.access, field.isStatic);
  if (field.readonly) {
    text += "readonly ";
  }
  text += field.name.str();
  text += field.optional ? "?" : "";
  const Type *type = field.resolved ? field.resolved : field.var ? field.var->type : nullptr;

  return text + ": " + typeName(type);
}

std::string SymbolText::variable(const VarDeclarator &declarator, bool isConst) {
  const Type *type = declarator.sym ? declarator.sym->type : nullptr;

  return (isConst ? "const " : "let ") + declarator.name.str() + ": " + typeName(type);
}

std::string SymbolText::header(const ClassDecl &cls) {
  std::string text = cls.isAbstract ? "abstract class " : "class ";
  text += cls.name.str();
  if (!cls.typeParams.empty()) {
    text += "<";
    for (size_t i = 0; i < cls.typeParams.size(); ++i) {
      text += (i ? ", " : "") + cls.typeParams[i].name.str();
    }
    text += ">";
  }
  if (cls.parent) {
    text += " extends " + cls.parent->name.str();
  } else if (cls.base) {
    text += " extends " + cls.base->name.str();
  }

  return text;
}

}
