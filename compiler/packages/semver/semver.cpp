#include "compiler/packages/semver/semver.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"

namespace tz::pkg {

namespace {

bool parseNumber(llvm::StringRef text, uint64_t &out) {
  if (text.empty() || text.size() > 15 || !llvm::all_of(text, llvm::isDigit))
    return false;
  return !text.getAsInteger(10, out);
}

bool isNumeric(llvm::StringRef id) { return !id.empty() && llvm::all_of(id, llvm::isDigit); }

bool parsePre(llvm::StringRef text, std::vector<std::string> &out) {
  llvm::SmallVector<llvm::StringRef, 4> ids;
  text.split(ids, '.');
  for (llvm::StringRef id : ids) {
    if (id.empty() || !llvm::all_of(id, [](char c) { return llvm::isAlnum(c) || c == '-'; }))
      return false;
    out.push_back(id.str());
  }
  return true;
}

// "1", "1.2", "1.x", "1.2.3-beta", "*": the leading components that are
// numbers; the rest are wildcards or missing.
struct Partial {
  uint64_t parts[3] = {0, 0, 0};
  unsigned count = 0;
  std::vector<std::string> pre;

  Version version() const {
    Version v;
    v.major = parts[0];
    v.minor = parts[1];
    v.patch = parts[2];
    if (count == 3)
      v.pre = pre;
    return v;
  }
};

bool parsePartial(llvm::StringRef text, Partial &out) {
  text.consume_front("=");
  text.consume_front("v");
  text = text.split('+').first;
  auto [core, pre] = text.split('-');
  if (core.size() != text.size() && !parsePre(pre, out.pre))
    return false;
  llvm::SmallVector<llvm::StringRef, 3> parts;
  core.split(parts, '.');
  if (parts.size() > 3)
    return false;
  bool wild = false;
  for (llvm::StringRef part : parts) {
    if (part == "x" || part == "X" || part == "*") {
      wild = true;
      continue;
    }
    if (wild || !parseNumber(part, out.parts[out.count]))
      return false;
    ++out.count;
  }
  // A prerelease belongs to a full version: "1.2-beta" means nothing.
  return out.pre.empty() || out.count == 3;
}

// The lowest version with these leading components: what "<1.2" excludes.
Version floorOf(uint64_t major, uint64_t minor, uint64_t patch) {
  Version v;
  v.major = major;
  v.minor = minor;
  v.patch = patch;
  v.pre = {"0"};
  return v;
}

// The first version after the components a partial gives: "1.2" -> 1.3.0-0.
Version after(const Partial &p) {
  if (p.count == 1)
    return floorOf(p.parts[0] + 1, 0, 0);
  if (p.count == 2)
    return floorOf(p.parts[0], p.parts[1] + 1, 0);
  return floorOf(p.parts[0], p.parts[1], p.parts[2] + 1);
}

} // namespace

bool Version::parse(llvm::StringRef text, Version &out) {
  Partial p;
  if (!parsePartial(text.trim(), p) || p.count != 3)
    return false;
  out = p.version();
  return true;
}

int Version::compare(const Version &o) const {
  if (major != o.major)
    return major < o.major ? -1 : 1;
  if (minor != o.minor)
    return minor < o.minor ? -1 : 1;
  if (patch != o.patch)
    return patch < o.patch ? -1 : 1;
  // A release is above its prereleases.
  if (pre.empty() || o.pre.empty())
    return pre.empty() == o.pre.empty() ? 0 : (pre.empty() ? 1 : -1);
  for (size_t i = 0; i < pre.size() && i < o.pre.size(); ++i) {
    const std::string &a = pre[i], &b = o.pre[i];
    if (a == b)
      continue;
    bool an = isNumeric(a), bn = isNumeric(b);
    if (an && bn) {
      if (a.size() != b.size())
        return a.size() < b.size() ? -1 : 1;
      return a < b ? -1 : 1;
    }
    if (an != bn)
      return an ? -1 : 1; // numbers come first
    return a < b ? -1 : 1;
  }
  if (pre.size() == o.pre.size())
    return 0;
  return pre.size() < o.pre.size() ? -1 : 1;
}

std::string Version::str() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
  for (size_t i = 0; i < pre.size(); ++i)
    s += (i ? "." : "-") + pre[i];
  return s;
}

bool Range::parse(llvm::StringRef text, Range &out, std::string &error) {
  out = Range();
  out.text_ = text.trim().str();
  llvm::SmallVector<llvm::StringRef, 2> alternatives;
  llvm::StringRef(out.text_).split(alternatives, "||");
  for (llvm::StringRef alternative : alternatives) {
    Set set;
    if (!parseSet(alternative.trim(), set, error))
      return false;
    out.sets_.push_back(std::move(set));
  }
  return true;
}

bool Range::parseSet(llvm::StringRef text, Set &out, std::string &error) {
  using Op = Comparator::Op;
  auto add = [&](Op op, Version v, bool synthetic = false) {
    out.push_back({op, std::move(v), synthetic});
  };
  // The words, with an operator joined to the version after it: ">= 1.2".
  llvm::SmallVector<llvm::StringRef, 4> pieces;
  text.split(pieces, ' ', -1, /*KeepEmpty=*/false);
  std::vector<std::string> words;
  for (size_t i = 0; i < pieces.size(); ++i) {
    std::string word = pieces[i].trim().str();
    if (word.empty())
      continue;
    bool bareOperator = word == "<" || word == "<=" || word == ">" || word == ">=" ||
                        word == "=" || word == "^" || word == "~";
    if (bareOperator && i + 1 < pieces.size())
      word += pieces[++i].trim().str();
    words.push_back(word);
  }
  auto bad = [&](llvm::StringRef word) {
    error = "'" + word.str() + "' is not a version or a range of versions";
    return false;
  };

  if (words.empty()) // "" is any version, like "*"
    return true;
  if (words.size() == 3 && words[1] == "-") {
    Partial low, high;
    if (!parsePartial(words[0], low))
      return bad(words[0]);
    if (!parsePartial(words[2], high))
      return bad(words[2]);
    add(Op::GreaterEq, low.version());
    if (high.count == 3)
      add(Op::LessEq, high.version());
    else if (high.count > 0)
      add(Op::Less, after(high), true);
    return true;
  }

  for (llvm::StringRef word : words) {
    Op op = Op::Equal;
    char kind = 0; // '^' or '~'
    if (word.consume_front(">="))
      op = Op::GreaterEq;
    else if (word.consume_front("<="))
      op = Op::LessEq;
    else if (word.consume_front(">"))
      op = Op::Greater;
    else if (word.consume_front("<"))
      op = Op::Less;
    else if (word.consume_front("^"))
      kind = '^';
    else if (word.consume_front("~>") || word.consume_front("~"))
      kind = '~';
    Partial p;
    if (!parsePartial(word, p))
      return bad(word);
    Version v = p.version();
    uint64_t major = p.parts[0], minor = p.parts[1];

    if (kind == '^') {
      if (p.count == 0)
        continue;
      add(Op::GreaterEq, v);
      if (p.count == 1 || major > 0)
        add(Op::Less, floorOf(major + 1, 0, 0), true);
      else if (p.count == 2 || minor > 0)
        add(Op::Less, floorOf(0, minor + 1, 0), true);
      else
        add(Op::Less, floorOf(0, 0, p.parts[2] + 1), true);
      continue;
    }
    if (kind == '~') {
      if (p.count == 0)
        continue;
      add(Op::GreaterEq, v);
      add(Op::Less, p.count == 1 ? floorOf(major + 1, 0, 0) : floorOf(major, minor + 1, 0), true);
      continue;
    }
    switch (op) {
    case Op::Equal:
      if (p.count == 3) {
        add(Op::Equal, v);
      } else if (p.count > 0) {
        add(Op::GreaterEq, v);
        add(Op::Less, after(p), true);
      }
      break;
    case Op::Greater:
      if (p.count == 3)
        add(Op::Greater, v);
      else if (p.count == 0)
        add(Op::Less, floorOf(0, 0, 0), true); // nothing
      else {
        Version next = after(p);
        next.pre.clear();
        add(Op::GreaterEq, next);
      }
      break;
    case Op::GreaterEq:
      if (p.count > 0)
        add(Op::GreaterEq, v);
      break;
    case Op::Less:
      if (p.count == 3)
        add(Op::Less, v);
      else
        add(Op::Less, floorOf(p.parts[0], p.parts[1], 0), true);
      break;
    case Op::LessEq:
      if (p.count == 3)
        add(Op::LessEq, v);
      else if (p.count > 0)
        add(Op::Less, after(p), true);
      break;
    }
  }
  return true;
}

bool Range::test(const Set &set, const Version &version) {
  for (const Comparator &c : set) {
    int order = version.compare(c.version);
    bool ok = false;
    switch (c.op) {
    case Comparator::Less: ok = order < 0; break;
    case Comparator::LessEq: ok = order <= 0; break;
    case Comparator::Greater: ok = order > 0; break;
    case Comparator::GreaterEq: ok = order >= 0; break;
    case Comparator::Equal: ok = order == 0; break;
    }
    if (!ok)
      return false;
  }
  if (!version.isPrerelease())
    return true;
  // A prerelease only where the range names one of the same version.
  for (const Comparator &c : set)
    if (!c.synthetic && c.version.isPrerelease() && c.version.major == version.major &&
        c.version.minor == version.minor && c.version.patch == version.patch)
      return true;
  return false;
}

bool Range::satisfies(const Version &version) const {
  for (const Set &set : sets_)
    if (test(set, version))
      return true;
  return false;
}

} // namespace tz::pkg
