#!/usr/bin/env python3
"""Style gate for the C++ port. See docs/cpp-style.md.

Rules enforced on .h/.cpp files under interfaces/, src/, app/, sdk/ and plugins/:
  no-static-function     no static member/free functions
  no-anonymous-namespace no `namespace {`
  no-free-function       functions are members of a class (main() and extern "C" excepted)
  no-nested-type         no class/struct/union/enum declared inside a class or function
  one-class-per-module   at most one class/struct per header, one header per directory
  header-name            header file name is the snake_case of its class name
  member-prefix          data members of classes (and structs with methods) start with m_
  src-include            src/<pkg>/<module> may only be included by itself or by app/
Files named *_test.cpp are exempt from the function and class rules (TEST macros).
A file whose first 5 lines contain `// style:c-abi` is exempt from every rule
(used only for the plugin C ABI header and extern "C" glue).
"""

import os
import re
import sys

ROOTS = ("interfaces", "src", "app", "sdk", "plugins")
EXTENSIONS = (".h", ".cpp")
ACCESS = ("public", "private", "protected")
SKIP_LEADING = ("using", "typedef", "friend", "static_assert", "template")
CLASS_RE = re.compile(
    r"^(?:template\s*<.*>\s*)?(class|struct|union)\s+(?:\[\[[^\]]*\]\]\s*)?(\w+)"
    r"(?:\s+final)?\s*(?::[^{;()=]*)?$"
)


class Violation:
    def __init__(self, path, line, rule, message):
        self.path = path
        self.line = line
        self.rule = rule
        self.message = message

    def render(self):
        return f"{self.path}:{self.line}: [{self.rule}] {self.message}"


class Tokenizer:
    """Splits C++ into (text, line) tokens, dropping comments and preprocessor lines."""

    def __init__(self, source):
        self.source = source
        self.includes = []

    def tokens(self):
        out = []
        i, n, line = 0, len(self.source), 1
        src = self.source
        at_line_start = True
        while i < n:
            c = src[i]
            if c == "\n":
                line += 1
                at_line_start = True
                i += 1
            elif c in " \t\r":
                i += 1
            elif src.startswith("//", i):
                while i < n and src[i] != "\n":
                    i += 1
            elif src.startswith("/*", i):
                end = src.find("*/", i + 2)
                end = n if end < 0 else end + 2
                line += src.count("\n", i, end)
                i = end
            elif c == "#" and at_line_start:
                i, line = self._skipDirective(i, line)
            elif c == '"' or c == "'":
                i, line = self._skipLiteral(i, line, c)
                out.append(('""', line))
            elif c.isalpha() or c == "_":
                j = i
                while j < n and (src[j].isalnum() or src[j] == "_"):
                    j += 1
                if src.startswith('R"', j - 1) and j < n and src[j] == '"':
                    i, line = self._skipRaw(j, line)
                    out.append(('""', line))
                else:
                    out.append((src[i:j], line))
                    i = j
            elif c.isdigit():
                j = i
                while j < n and (src[j].isalnum() or src[j] in "._'"):
                    j += 1
                out.append((src[i:j], line))
                i = j
            else:
                two = src[i : i + 2]
                if two in ("::", "->", "==", "&&", "||"):
                    out.append((two, line))
                    i += 2
                else:
                    out.append((c, line))
                    i += 1
            if c not in " \t\r\n" and not (c == "#" and at_line_start):
                at_line_start = False
        return out

    def _skipDirective(self, i, line):
        start = i
        n = len(self.source)
        while i < n:
            if self.source[i] == "\n" and self.source[i - 1] != "\\":
                break
            i += 1
        text = self.source[start:i]
        match = re.match(r'#\s*include\s*"([^"]+)"', text)
        if match:
            self.includes.append((match.group(1), line))
        line += self.source.count("\n", start, i)
        return i, line

    def _skipLiteral(self, i, line, quote):
        n = len(self.source)
        i += 1
        while i < n and self.source[i] != quote:
            if self.source[i] == "\\":
                i += 1
            if i < n and self.source[i] == "\n":
                line += 1
            i += 1
        return i + 1, line

    def _skipRaw(self, i, line):
        end_open = self.source.find("(", i)
        delimiter = self.source[i + 1 : end_open]
        close = ")" + delimiter + '"'
        end = self.source.find(close, end_open)
        end = len(self.source) if end < 0 else end + len(close)
        line += self.source.count("\n", i, end)
        return end, line


class Scope:
    def __init__(self, kind, isStruct=False, name=""):
        self.kind = kind  # ns | class | enum | fn | init
        self.isStruct = isStruct
        self.name = name
        self.statements = []  # (tokens, line) of class-level statements
        self.openLine = 0


class FileChecker:
    def __init__(self, path, source):
        self.path = path
        self.source = source
        self.violations = []
        self.isTest = path.endswith("_test.cpp")
        self.classNames = []
        head = "\n".join(source.splitlines()[:5])
        self.exempt = "style:c-abi" in head

    def run(self):
        if self.exempt:
            return self.violations
        tokenizer = Tokenizer(self.source)
        tokens = tokenizer.tokens()
        self._checkIncludes(tokenizer.includes)
        self._scan(tokens)
        self._checkHeaderRules()
        return self.violations

    def _add(self, line, rule, message):
        self.violations.append(Violation(self.path, line, rule, message))

    def _checkIncludes(self, includes):
        for target, line in includes:
            parts = target.split("/")
            if parts[0] != "src" or len(parts) < 3:
                continue
            if self.path.startswith("app/"):
                continue
            own = self.path.split("/")
            if own[:3] != parts[:3]:
                self._add(line, "src-include", f'"{target}" crosses module boundaries')

    def _scan(self, tokens):
        stack = []
        stmt = []
        stmtLine = 0
        parens = 0
        for text, line in tokens:
            if not stmt:
                stmtLine = line
            if text in "([":
                parens += 1
            elif text in ")]":
                parens -= 1
            if text == "{" and parens == 0:
                scope = self._classify(stmt, stack, stmtLine)
                if scope.kind == "init":
                    stmt.append(("{}", line))
                    stack.append(scope)
                    continue
                scope.openLine = stmtLine
                stack.append(scope)
                stmt = []
            elif text == "}" and parens == 0:
                done = stack.pop() if stack else Scope("ns")
                if done.kind == "init":
                    continue
                if done.kind == "class":
                    self._checkClass(done)
                stmt = []
            elif text == ";" and parens == 0:
                self._endStatement(stmt, stack, stmtLine)
                stmt = []
            else:
                stmt.append((text, line))

    def _words(self, stmt):
        return [t for t, _ in stmt]

    def _classify(self, stmt, stack, line):
        words = self._words(stmt)
        joined = " ".join(words)
        inClass = any(s.kind in ("class", "fn") for s in stack)
        if "namespace" in words:
            index = words.index("namespace")
            if index == len(words) - 1 or words[index + 1] in ("[", "__attribute__"):
                self._add(line, "no-anonymous-namespace", "anonymous namespace")
            return Scope("ns")
        if words[:1] == ["extern"] and len(words) == 2:
            return Scope("ns")
        if "enum" in words and "(" not in words:
            if inClass:
                self._add(line, "no-nested-type", "enum declared inside a class or function")
            return Scope("enum")
        spaced = re.sub(r"\s+", " ", joined.replace(" :: ", "::"))
        match = CLASS_RE.match(re.sub(r"\s*<\s*", "<", spaced).replace(" ,", ","))
        if match and "(" not in words:
            if inClass:
                self._add(line, "no-nested-type", f"{match.group(2)} nested in a class or function")
            return Scope("class", isStruct=match.group(1) != "class", name=match.group(2))
        if self._isFunctionBody(words):
            self._checkFunctionScope(stmt, stack, line)
            if stack and stack[-1].kind == "class":
                stack[-1].statements.append((["("], line))
            return Scope("fn")
        return Scope("init")

    def _isFunctionBody(self, words):
        if not words:
            return False
        if words[-1] == ")":
            return True
        if words[-1] == "{}" and "(" in words and ":" in words:
            return True
        tail = ("const", "noexcept", "override", "final", "volatile", "&", "&&", "]", "try")
        if words[-1] in tail and ")" in words:
            return True
        return "->" in words and ")" in words

    def _checkFunctionScope(self, stmt, stack, line):
        if self.isTest or any(s.kind == "fn" for s in stack):
            return
        words = self._words(stmt)
        if "static" in words and "(" in words:
            self._add(line, "no-static-function", "static function")
        if stack and stack[-1].kind == "class":
            return
        head = words[: words.index("(")] if "(" in words else words
        if "::" in head or (head and head[-1] == "main"):
            return
        self._add(line, "no-free-function", f"free function '{head[-1] if head else '?'}'")

    def _endStatement(self, stmt, stack, line):
        words = self._words(stmt)
        if not words:
            return
        while words and (words[0] in ACCESS and words[1:2] == [":"]):
            words = words[2:]
        scope = stack[-1] if stack else Scope("ns")
        if "static" in words and "(" in words and self._beforeAssign(words):
            self._add(line, "no-static-function", "static function declaration")
        if scope.kind == "class":
            scope.statements.append((words, line))
        if "enum" in words and scope.kind == "class":
            self._add(line, "no-nested-type", "enum declared inside a class")
        if scope.kind == "ns" and not self.isTest:
            self._checkForwardClass(words, line)

    def _beforeAssign(self, words):
        for word in words:
            if word == "(":
                return True
            if word in ("=", "{}"):
                return False
        return False

    def _checkForwardClass(self, words, line):
        return

    def _checkClass(self, scope):
        self.classNames.append((scope.name, scope.openLine))
        hasMethods = any("(" in w for w, _ in scope.statements)
        if scope.isStruct and not hasMethods:
            return
        for words, line in scope.statements:
            self._checkMember(words, line)

    def _checkMember(self, words, line):
        if not words or words[0] in SKIP_LEADING or "(" in self._declarator(words):
            return
        if "static" in words and ("constexpr" in words or "const" in words):
            return
        name = self._memberName(self._declarator(words))
        if name and not name.startswith("m_"):
            self._add(line, "member-prefix", f"data member '{name}' must start with m_")

    def _declarator(self, words):
        out = []
        for word in words:
            if word in ("=", "{}", ":"):
                break
            out.append(word)
        return out

    def _memberName(self, words):
        depth = 0
        name = ""
        for word in words:
            if word == "[":
                depth += 1
            elif word == "]":
                depth -= 1
            elif depth == 0 and re.match(r"^[A-Za-z_]\w*$", word):
                name = word
        return name

    def _checkHeaderRules(self):
        if self.isTest or not self.path.endswith(".h"):
            return
        if len(self.classNames) > 1:
            names = ", ".join(n for n, _ in self.classNames)
            self._add(self.classNames[1][1], "one-class-per-module", f"multiple types: {names}")
        if self.classNames:
            name, line = self.classNames[0]
            expected = self._snake(name) + ".h"
            if os.path.basename(self.path) != expected:
                self._add(line, "header-name", f"{name} must live in {expected}")

    def _snake(self, name):
        step = re.sub(r"([A-Z]+)([A-Z][a-z])", r"\1_\2", name)
        step = re.sub(r"([a-z0-9])([A-Z])", r"\1_\2", step)
        return step.lower()


def collectFiles(base):
    found = []
    for root in ROOTS:
        for directory, _, names in os.walk(os.path.join(base, root)):
            for name in sorted(names):
                if name.endswith(EXTENSIONS):
                    found.append(os.path.join(directory, name))
    return sorted(found)


def checkDirectoryHeaders(base, files):
    violations = []
    perDirectory = {}
    for path in files:
        if path.endswith(".h") and not path.endswith("_test.h"):
            perDirectory.setdefault(os.path.dirname(path), []).append(path)
    for directory, headers in perDirectory.items():
        if len(headers) > 1:
            rel = os.path.relpath(directory, base)
            violations.append(Violation(rel, 1, "one-class-per-module", "more than one header"))
    return violations


def checkTree(base):
    violations = []
    files = collectFiles(base)
    for path in files:
        with open(path, encoding="utf-8") as handle:
            source = handle.read()
        rel = os.path.relpath(path, base)
        violations.extend(FileChecker(rel, source).run())
    violations.extend(checkDirectoryHeaders(base, files))
    return violations


def main():
    default = os.environ.get("BUILD_WORKSPACE_DIRECTORY", os.getcwd())
    base = sys.argv[1] if len(sys.argv) > 1 else default
    violations = checkTree(base)
    for violation in violations:
        print(violation.render())
    if violations:
        print(f"{len(violations)} style violation(s)")
        return 1
    print("style ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
