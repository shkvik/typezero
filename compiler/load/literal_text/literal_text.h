#pragma once

#include "compiler/load/token/token.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include <string>

namespace tz {

bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags);
bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags);
void decodeTemplateRaw(const Token &tok, std::u16string &out);
void decodeRegex(const Token &tok, std::u16string &pattern, std::u16string &flags);

}
