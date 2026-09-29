
/// 内置语言规则集（与引擎分离：这里是**纯数据**，引擎在 `highlight.cpp`）。
///
/// 每种语言只是一份 `LanguageSpec` 字段填充——与用户在运行时注册自定义语言走**同一台扫描器**，
/// 因此「内置语言」与「自定义语言」能力完全等价（新增内置语言不需要改引擎）。
#include "st/text/highlight.hpp"

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::text {

// ============================ 内置语言规则集 ============================
//
// 规则即数据：这里每种语言就是一份字段填充，没有一行专用代码——
// 这也是"自定义语言"与"内置语言"能力等价的原因（用户注册的规则走同一台扫描器）。
//
// 词表顺序无所谓：`normalize_spec()` 会去重排序（供二分查找），忽略大小写的语言统一小写化。

namespace {

using WordList = std::initializer_list<std::string_view>;

[[nodiscard]] auto spec_of(std::string name, std::initializer_list<std::string_view> aliases,
                           WordList keywords, WordList types, WordList builtins) -> LanguageSpec {
  LanguageSpec spec = make_language(std::move(name));
  with_aliases(spec, aliases);
  with_keywords(spec, keywords);
  with_types(spec, types);
  with_builtins(spec, builtins);
  return spec;
}

}  // namespace

auto builtin_languages() -> std::vector<LanguageSpec> {
  std::vector<LanguageSpec> out;
  const auto add = [&out](LanguageSpec spec) { out.push_back(std::move(spec)); };

  // —— C ——
  {
    LanguageSpec spec = spec_of(
        "c", {"h"},
        WordList{"auto", "break", "case", "const", "continue", "default", "do", "else", "enum",
                 "extern", "for", "goto", "if", "inline", "register", "restrict", "return",
                 "sizeof", "static", "struct", "switch", "typedef", "union", "volatile", "while",
                 "_Bool", "_Generic", "_Static_assert", "_Thread_local"},
        WordList{"bool", "char", "double", "float", "int", "int16_t", "int32_t", "int64_t",
                 "int8_t", "long", "short", "size_t", "ssize_t", "uint16_t", "uint32_t",
                 "uint64_t", "uint8_t", "void", "wchar_t"},
        WordList{"EOF", "NULL", "false", "stderr", "stdin", "stdout", "true"});
    spec.definition_keywords = {"struct", "enum", "union", "typedef"};
    with_c_comments(spec);
    spec.preprocessor = true;
    add(std::move(spec));
  }

  // —— C++ ——
  {
    LanguageSpec spec = spec_of(
        "cpp", {"c++", "cxx", "cc", "hpp", "hxx", "h++"},
        WordList{"alignas", "alignof", "and", "asm", "auto", "break", "case", "catch", "class",
                 "concept", "const", "const_cast", "consteval", "constexpr", "constinit",
                 "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
                 "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern", "for",
                 "friend", "goto", "if", "inline", "mutable", "namespace", "new", "noexcept",
                 "not", "nullptr", "operator", "or", "override", "private", "protected", "public",
                 "register", "reinterpret_cast", "requires", "restrict", "return", "sizeof",
                 "static", "static_assert", "static_cast", "struct", "switch", "template", "this",
                 "thread_local", "throw", "try", "typedef", "typename", "union", "using",
                 "virtual", "volatile", "while"},
        WordList{"array", "bool", "char", "char16_t", "char32_t", "char8_t", "double", "float",
                 "int", "int16_t", "int32_t", "int64_t", "int8_t", "long", "map", "optional",
                 "pair", "size_t", "span", "string", "string_view", "tuple", "uint16_t",
                 "uint32_t", "uint64_t", "uint8_t", "unique_ptr", "unordered_map", "variant",
                 "vector", "void", "wchar_t"},
        WordList{"EOF", "NULL", "false", "stderr", "stdin", "stdout", "std", "true"});
    spec.definition_keywords = {"class", "struct", "enum", "namespace", "concept"};
    spec.string_prefixes = {"R\"(", "u8\"", "L\""};
    with_c_comments(spec);
    spec.preprocessor = true;
    add(std::move(spec));
  }

  // —— Python ——
  {
    LanguageSpec spec = spec_of(
        "python", {"py", "python3", "pyi"},
        WordList{"and", "as", "assert", "async", "await", "break", "case", "class", "continue",
                 "def", "del", "elif", "else", "except", "finally", "for", "from", "global", "if",
                 "import", "in", "is", "lambda", "match", "nonlocal", "not", "or", "pass",
                 "raise", "return", "try", "while", "with", "yield"},
        WordList{"bool", "bytes", "complex", "dict", "float", "frozenset", "int", "list", "object",
                 "set", "str", "tuple", "type"},
        WordList{"False", "None", "NotImplemented", "True", "Ellipsis", "__name__", "self",
                 "super", "print", "len", "range", "enumerate", "zip", "open", "isinstance",
                 "sorted", "reversed", "min", "max", "sum", "abs"});
    spec.definition_keywords = {"def", "class"};
    spec.string_prefixes = {"f\"", "b\"", "r\"", "rb\"", "u\"", "f'", "b'", "r'", "rb'", "u'"};
    with_line_comment(spec, "#");
    spec.triple_quote = true;
    add(std::move(spec));
  }

  // —— Rust ——
  {
    LanguageSpec spec = spec_of(
        "rust", {"rs"},
        WordList{"as", "async", "await", "break", "const", "continue", "crate", "dyn", "else",
                 "enum", "extern", "fn", "for", "if", "impl", "in", "let", "loop", "match", "mod",
                 "move", "mut", "pub", "ref", "return", "self", "static", "struct", "super",
                 "trait", "type", "unsafe", "use", "where", "while", "union"},
        WordList{"bool", "char", "f32", "f64", "i8", "i16", "i32", "i64", "i128", "isize", "str",
                 "u8", "u16", "u32", "u64", "u128", "usize", "String", "Vec", "Option", "Result",
                 "Box", "Rc", "Arc", "HashMap", "HashSet", "BTreeMap"},
        WordList{"false", "None", "Ok", "Err", "Some", "true", "self", "Self", "println",
                 "print", "format", "vec", "panic", "assert", "unwrap"});
    spec.definition_keywords = {"fn", "struct", "enum", "trait", "impl", "mod", "type"};
    spec.attribute_prefix = "#";
    spec.string_prefixes = {"r#\"", "b\"", "r\""};
    with_c_comments(spec);
    add(std::move(spec));
  }

  // —— Go ——
  {
    LanguageSpec spec = spec_of(
        "go", {"golang"},
        WordList{"break", "case", "chan", "const", "continue", "default", "defer", "else",
                 "fallthrough", "for", "func", "go", "goto", "if", "import", "interface", "map",
                 "package", "range", "return", "select", "struct", "switch", "type", "var"},
        WordList{"bool", "byte", "complex64", "complex128", "error", "float32", "float64", "int",
                 "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8", "uint16",
                 "uint32", "uint64", "uintptr", "any"},
        WordList{"append", "cap", "close", "copy", "delete", "false", "iota", "len", "make", "new",
                 "nil", "panic", "print", "println", "recover", "true"});
    spec.definition_keywords = {"func", "type", "struct", "interface"};
    with_c_comments(spec);
    add(std::move(spec));
  }

  // —— JavaScript ——
  {
    LanguageSpec spec = spec_of(
        "javascript", {"js", "mjs", "cjs", "node", "jsx"},
        WordList{"async", "await", "break", "case", "catch", "class", "const", "continue",
                 "debugger", "default", "delete", "do", "else", "export", "extends", "finally",
                 "for", "function", "if", "import", "in", "instanceof", "let", "new", "of",
                 "return", "static", "super", "switch", "this", "throw", "try", "typeof", "var",
                 "void", "while", "with", "yield"},
        WordList{"Array", "Boolean", "Date", "Error", "Function", "JSON", "Map", "Math", "Number",
                 "Object", "Promise", "RegExp", "Set", "String", "Symbol", "WeakMap", "BigInt",
                 "Infinity", "NaN"},
        WordList{"console", "document", "globalThis", "false", "null", "process", "require",
                 "true", "undefined", "window", "module", "exports"});
    spec.definition_keywords = {"function", "class", "const", "let", "var"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    spec.backtick_string = true;
    add(std::move(spec));
  }

  // —— TypeScript ——
  {
    LanguageSpec spec = spec_of(
        "typescript", {"ts", "tsx", "mts", "cts"},
        WordList{"abstract", "any", "as", "asserts", "async", "await", "break", "case", "catch",
                 "class", "const", "continue", "declare", "default", "delete", "do", "else",
                 "enum", "export", "extends", "finally", "for", "from", "function", "if",
                 "implements", "import", "in", "infer", "instanceof", "interface", "is", "keyof",
                 "let", "namespace", "new", "of", "override", "private", "protected", "public",
                 "readonly", "return", "satisfies", "static", "super", "switch", "this", "throw",
                 "try", "type", "typeof", "var", "void", "while", "yield"},
        WordList{"Array", "Boolean", "Date", "Error", "Map", "Never", "Number", "Object",
                 "Partial", "Promise", "Readonly", "Record", "Set", "String", "Symbol", "WeakMap",
                 "any", "bigint", "boolean", "never", "null", "number", "object", "string",
                 "unknown", "void"},
        WordList{"console", "document", "false", "globalThis", "null", "process", "true",
                 "undefined", "window"});
    spec.definition_keywords = {"function", "class", "interface", "type", "enum", "const", "let"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    spec.backtick_string = true;
    add(std::move(spec));
  }

  // —— Java ——
  {
    LanguageSpec spec = spec_of(
        "java", {},
        WordList{"abstract", "assert", "break", "case", "catch", "class", "const", "continue",
                 "default", "do", "else", "enum", "extends", "final", "finally", "for", "goto",
                 "if", "implements", "import", "instanceof", "interface", "native", "new",
                 "package", "private", "protected", "public", "record", "return", "sealed",
                 "static", "strictfp", "super", "switch", "synchronized", "this", "throw",
                 "throws", "transient", "try", "var", "volatile", "while", "yield"},
        WordList{"boolean", "byte", "char", "double", "float", "int", "long", "short", "void",
                 "String", "Integer", "Long", "Double", "Boolean", "Object", "List", "Map", "Set",
                 "Optional", "Stream"},
        WordList{"false", "null", "true", "System", "Math", "Objects", "Arrays", "Collections"});
    spec.definition_keywords = {"class", "interface", "enum", "record"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    add(std::move(spec));
  }

  // —— C# ——
  {
    LanguageSpec spec = spec_of(
        "csharp", {"cs", "c#"},
        WordList{"abstract", "as", "async", "await", "base", "break", "case", "catch", "checked",
                 "class", "const", "continue", "default", "delegate", "do", "else", "enum",
                 "event", "explicit", "extern", "finally", "fixed", "for", "foreach", "get",
                 "goto", "if", "implicit", "in", "init", "interface", "internal", "is", "lock",
                 "namespace", "new", "operator", "out", "override", "params", "partial", "private",
                 "protected", "public", "readonly", "record", "ref", "return", "sealed", "set",
                 "sizeof", "stackalloc", "static", "struct", "switch", "this", "throw", "try",
                 "typeof", "unchecked", "unsafe", "using", "var", "virtual", "volatile", "when",
                 "where", "while", "yield"},
        WordList{"bool", "byte", "char", "decimal", "double", "dynamic", "float", "int", "long",
                 "object", "sbyte", "short", "string", "uint", "ulong", "ushort", "void", "Task",
                 "List", "Dictionary", "IEnumerable"},
        WordList{"false", "null", "true", "Console", "Math", "String", "Convert", "nameof"});
    spec.definition_keywords = {"class", "interface", "struct", "record", "enum", "namespace"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    spec.preprocessor = true;
    add(std::move(spec));
  }

  // —— Kotlin ——
  {
    LanguageSpec spec = spec_of(
        "kotlin", {"kt", "kts"},
        WordList{"abstract", "actual", "annotation", "as", "break", "by", "catch", "class",
                 "companion", "const", "constructor", "continue", "crossinline", "data", "delegate",
                 "do", "dynamic", "else", "enum", "expect", "external", "field", "final", "finally",
                 "for", "fun", "get", "if", "import", "in", "infix", "init", "inline", "inner",
                 "interface", "internal", "is", "lateinit", "noinline", "object", "open", "operator",
                 "out", "override", "package", "private", "protected", "public", "reified",
                 "return", "sealed", "set", "suspend", "tailrec", "this", "throw", "try", "typealias",
                 "val", "var", "vararg", "when", "where", "while"},
        WordList{"Any", "Boolean", "Byte", "Char", "Double", "Float", "Int", "Long", "Nothing",
                 "Short", "String", "Unit", "List", "Map", "Set", "Array"},
        WordList{"false", "it", "null", "super", "true", "println", "print", "require", "check"});
    spec.definition_keywords = {"class", "fun", "object", "interface", "typealias"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    spec.triple_quote = true;
    add(std::move(spec));
  }

  // —— Swift ——
  {
    LanguageSpec spec = spec_of(
        "swift", {},
        WordList{"actor", "as", "associatedtype", "async", "await", "break", "case", "catch",
                 "class", "continue", "default", "defer", "deinit", "do", "else", "enum", "extension",
                 "fallthrough", "fileprivate", "for", "func", "guard", "if", "import", "in", "indirect",
                 "init", "inout", "internal", "is", "lazy", "let", "mutating", "nonisolated", "open",
                 "operator", "override", "private", "protocol", "public", "repeat", "required",
                 "rethrows", "return", "self", "some", "static", "struct", "subscript", "switch",
                 "throw", "throws", "try", "typealias", "var", "where", "while"},
        WordList{"Any", "AnyObject", "Array", "Bool", "Character", "Dictionary", "Double", "Float",
                 "Int", "Never", "Optional", "Set", "String", "Void"},
        WordList{"false", "nil", "print", "super", "true", "assert", "fatalError", "precondition"});
    spec.definition_keywords = {"func", "class", "struct", "enum", "protocol", "extension"};
    spec.attribute_prefix = "@";
    with_c_comments(spec);
    spec.triple_quote = true;
    add(std::move(spec));
  }

  // —— Ruby ——
  {
    LanguageSpec spec = spec_of(
        "ruby", {"rb"},
        WordList{"alias", "and", "begin", "break", "case", "class", "def", "defined?", "do",
                 "else", "elsif", "end", "ensure", "for", "if", "in", "module", "next", "not", "or",
                 "redo", "rescue", "retry", "return", "self", "super", "then", "undef", "unless",
                 "until", "when", "while", "yield"},
        WordList{"Array", "Comparable", "Enumerable", "Float", "Hash", "Integer", "Numeric",
                 "Object", "Range", "String", "Struct", "Symbol"},
        WordList{"false", "nil", "puts", "require", "attr_accessor", "attr_reader", "attr_writer",
                 "lambda", "proc", "true"});
    spec.definition_keywords = {"def", "class", "module"};
    spec.attribute_prefix = "@";
    with_line_comment(spec, "#");
    spec.triple_quote = false;
    add(std::move(spec));
  }

  // —— PHP ——
  {
    LanguageSpec spec = spec_of(
        "php", {},
        WordList{"abstract", "and", "array", "as", "break", "callable", "case", "catch", "class",
                 "clone", "const", "continue", "declare", "default", "do", "echo", "else", "elseif",
                 "empty", "enddeclare", "endfor", "endforeach", "endif", "endswitch", "endwhile",
                 "enum", "extends", "final", "finally", "fn", "for", "foreach", "function", "global",
                 "goto", "if", "implements", "include", "include_once", "instanceof", "insteadof",
                 "interface", "isset", "list", "match", "namespace", "new", "or", "print", "private",
                 "protected", "public", "readonly", "require", "require_once", "return", "static",
                 "switch", "throw", "trait", "try", "unset", "use", "var", "while", "xor", "yield"},
        WordList{"bool", "float", "int", "iterable", "mixed", "never", "object", "string", "void"},
        WordList{"false", "null", "true", "self", "parent", "this", "__construct", "count",
                 "printf", "strlen", "array_map"});
    spec.definition_keywords = {"function", "class", "interface", "trait", "enum"};
    with_c_comments(spec);
    with_line_comment(spec, "//");
    spec.line_comment_alt = "#";
    add(std::move(spec));
  }

  // —— Lua ——
  {
    LanguageSpec spec = spec_of(
        "lua", {},
        WordList{"and", "break", "do", "else", "elseif", "end", "for", "function", "goto", "if",
                 "in", "local", "not", "or", "repeat", "return", "then", "until", "while"},
        WordList{"boolean", "function", "nil", "number", "string", "table", "thread", "userdata"},
        WordList{"false", "nil", "true", "self", "print", "ipairs", "pairs", "require", "type",
                 "tostring", "tonumber", "error", "pcall"});
    spec.definition_keywords = {"function", "local"};
    spec.line_comment = "--";
    with_block_comment(spec, "--[[", "]]");
    add(std::move(spec));
  }

  // —— SQL（忽略大小写）——
  {
    LanguageSpec spec = spec_of(
        "sql", {},
        WordList{"add", "all", "alter", "and", "any", "as", "asc", "begin", "between", "by",
                 "case", "cast", "check", "column", "commit", "constraint", "create", "cross",
                 "database", "default", "delete", "desc", "distinct", "drop", "else", "end",
                 "except", "exists", "foreign", "from", "full", "group", "having", "if", "in",
                 "index", "inner", "insert", "intersect", "into", "is", "join", "key", "left",
                 "like", "limit", "not", "null", "offset", "on", "or", "order", "outer", "primary",
                 "references", "returning", "right", "rollback", "select", "set", "table", "then",
                 "top", "union", "unique", "update", "values", "view", "when", "where", "with"},
        WordList{"bigint", "blob", "boolean", "char", "date", "decimal", "double", "float",
                 "int", "integer", "json", "numeric", "real", "smallint", "text", "time",
                 "timestamp", "uuid", "varchar"},
        WordList{"avg", "coalesce", "count", "current_date", "current_timestamp", "false",
                 "max", "min", "now", "null", "sum", "true"});
    spec.definition_keywords = {"table", "view", "database", "index"};
    spec.line_comment = "--";
    with_block_comment(spec, "/*", "*/");
    spec.case_insensitive = true;
    spec.function_call = true;
    add(std::move(spec));
  }

  // —— HTML ——
  {
    LanguageSpec spec = spec_of(
        "html", {"htm", "xhtml"},
        WordList{"doctype"},
        WordList{},
        WordList{});
    with_block_comment(spec, "<!--", "-->");
    with_tags(spec, "<", ">");
    spec.double_quote = true;
    spec.single_quote = true;
    spec.case_insensitive = true;
    add(std::move(spec));
  }

  // —— XML ——
  {
    LanguageSpec spec = spec_of("xml", {"svg", "plist", "pom"}, WordList{}, WordList{}, WordList{});
    with_block_comment(spec, "<!--", "-->");
    with_tags(spec, "<", ">");
    add(std::move(spec));
  }

  // —— CSS ——
  {
    LanguageSpec spec = spec_of(
        "css", {},
        WordList{"and", "important", "media", "not", "only", "supports"},
        WordList{"em", "px", "rem", "vh", "vw"},
        WordList{"inherit", "initial", "none", "unset", "auto"});
    spec.attribute_prefix = "@";
    spec.key_value_keys = true;
    spec.css_like = true;
    with_block_comment(spec, "/*", "*/");
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— SCSS ——
  {
    LanguageSpec spec = spec_of(
        "scss", {"sass", "less"},
        WordList{"and", "debug", "each", "else", "error", "extend", "for", "function", "if",
                 "import", "include", "mixin", "not", "or", "return", "use", "warn", "while"},
        WordList{"em", "px", "rem", "vh", "vw"},
        WordList{"inherit", "initial", "none", "unset", "auto", "important"});
    spec.attribute_prefix = "@";
    spec.key_value_keys = true;
    spec.css_like = true;
    with_c_comments(spec);
    add(std::move(spec));
  }

  // —— JSON ——
  {
    LanguageSpec spec = spec_of("json", {"jsonc", "jsonl", "geojson"}, WordList{}, WordList{},
                                    WordList{"false", "null", "true"});
    spec.key_value_keys = true;
    spec.single_quote = false;
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— YAML ——
  {
    LanguageSpec spec = spec_of("yaml", {"yml"}, WordList{}, WordList{},
                                    WordList{"false", "null", "true", "yes", "no", "on", "off", "~"});
    spec.key_value_keys = true;
    with_line_comment(spec, "#");
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— TOML ——
  {
    LanguageSpec spec = spec_of("toml", {}, WordList{}, WordList{},
                                    WordList{"false", "true", "inf", "nan"});
    spec.key_value_keys = true;
    with_line_comment(spec, "#");
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— INI / dotenv / conf ——
  {
    LanguageSpec spec = spec_of("ini", {"cfg", "conf", "properties", "env", "dotenv"}, WordList{},
                                    WordList{}, WordList{"false", "true", "yes", "no", "on", "off"});
    spec.key_value_keys = true;
    with_line_comment(spec, "#");
    spec.line_comment_alt = ";";
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— Shell ——
  {
    LanguageSpec spec = spec_of(
        "shell", {"bash", "sh", "zsh", "ksh", "console"},
        WordList{"case", "do", "done", "elif", "else", "esac", "fi", "for", "function", "if",
                 "in", "local", "return", "select", "then", "time", "until", "while"},
        WordList{},
        WordList{"alias", "bg", "cd", "echo", "eval", "exec", "exit", "export", "false", "fg",
                 "grep", "kill", "printf", "pwd", "read", "readonly", "sed", "set", "shift", "source",
                 "sudo", "test", "trap", "true", "umask", "unalias", "unset", "wait"});
    spec.definition_keywords = {"function", "alias"};
    with_line_comment(spec, "#");
    spec.backtick_string = true;
    spec.escape_in_single = false;
    spec.string_prefixes = {"$\"", "$\'"};
    add(std::move(spec));
  }

  // —— Dockerfile ——
  {
    LanguageSpec spec = spec_of(
        "dockerfile", {"docker"},
        WordList{"add", "arg", "cmd", "copy", "entrypoint", "env", "expose", "from", "healthcheck",
                 "label", "maintainer", "onbuild", "run", "shell", "stopsignal", "user", "volume",
                 "workdir"},
        WordList{"alpine", "amd64", "arm64", "debian", "slim", "ubuntu"},
        WordList{"as", "true", "false", "null"});
    with_line_comment(spec, "#");
    spec.case_insensitive = true;
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— Makefile ——
  {
    LanguageSpec spec = spec_of(
        "makefile", {"make", "mk"},
        WordList{"define", "else", "endef", "endif", "export", "ifdef", "ifeq", "ifndef", "ifneq",
                 "include", "override", "unexport", "vpath"},
        WordList{},
        WordList{"addprefix", "addsuffix", "basename", "call", "dir", "error", "filter", "findstring",
                 "firstword", "foreach", "notdir", "patsubst", "shell", "sort", "strip", "subst",
                 "suffix", "warning", "wildcard", "word", "words"});
    with_line_comment(spec, "#");
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— CMake ——
  {
    LanguageSpec spec = spec_of(
        "cmake", {},
        WordList{"else", "elseif", "endforeach", "endfunction", "endif", "endmacro", "endwhile",
                 "foreach", "function", "if", "macro", "while"},
        WordList{},
        WordList{"add_executable", "add_library", "add_subdirectory", "cmake_minimum_required",
                 "configure_file", "enable_testing", "file", "find_package", "include",
                 "include_directories", "install", "list", "message", "option", "project",
                 "set", "string", "target_compile_definitions", "target_compile_options",
                 "target_include_directories", "target_link_libraries", "target_sources", "unset"});
    with_line_comment(spec, "#");
    with_block_comment(spec, "#[[", "]]");
    add(std::move(spec));
  }

  // —— Markdown ——
  {
    LanguageSpec spec = spec_of("markdown", {"md", "mdx", "mkd"}, WordList{}, WordList{}, WordList{});
    spec.markdown_like = true;
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— Diff ——
  {
    LanguageSpec spec = spec_of("diff", {"patch"}, WordList{}, WordList{}, WordList{});
    spec.diff_like = true;
    spec.function_call = false;
    add(std::move(spec));
  }

  // —— Protocol Buffers ——
  {
    LanguageSpec spec = spec_of(
        "protobuf", {"proto", "proto3"},
        WordList{"enum", "extend", "extensions", "import", "map", "message", "oneof", "option",
                 "package", "public", "repeated", "reserved", "returns", "rpc", "service", "stream",
                 "syntax", "to", "weak"},
        WordList{"bool", "bytes", "double", "fixed32", "fixed64", "float", "int32", "int64",
                 "sfixed32", "sfixed64", "sint32", "sint64", "string", "uint32", "uint64"},
        WordList{"false", "true", "max"});
    spec.definition_keywords = {"message", "enum", "service", "rpc", "oneof"};
    with_c_comments(spec);
    add(std::move(spec));
  }

  return out;
}

}  // namespace st::text
