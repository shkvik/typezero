#pragma once

#include "compiler/load/token/token.h"
#include "compiler/program/diagnostics/diagnostics.h"
#include <string>

namespace tz {

// The text of literals: the code units of strings, templates and regexes, their escapes decoded.

// Decode the text of a string literal or a template part into UTF-16 code
// units, the way JavaScript sees it. Return false after reporting a malformed
// escape.
bool decodeStringLiteral(const Token &tok, std::u16string &out, Diagnostics &diags);
bool decodeTemplatePart(const Token &tok, std::u16string &out, Diagnostics &diags);
// The raw text of a template part, as String.raw gives it: escapes stay as
// written, and only line breaks are normalized to "\n".
void decodeTemplateRaw(const Token &tok, std::u16string &out);
// The pattern and the flags of a regex literal, as UTF-16; the escapes stay,
// they are the regex's own.
void decodeRegex(const Token &tok, std::u16string &pattern, std::u16string &flags);

} // namespace tz
