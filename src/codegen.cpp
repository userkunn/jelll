#include "codegen.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>
#include <sstream>

#include "util.hpp"

namespace jelll {

namespace {

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

const std::set<std::string> kTsReserved = {"constructor", "free", "fromPtr",
                                           "liveCount", "slotCount", "_ptr"};

// A sync struct becomes `globalThis.<name>`, so names owned by the runtime,
// the glue, or core JS built-ins must be rejected (they would be clobbered).
const std::set<std::string> kForbiddenGlobals = {
    "native", "sharedBuffer", "sharedMemory", "createJelllModule", "globalThis",
    "window", "document", "console", "fetch", "module", "exports", "require",
    "Object", "Function", "Boolean", "Number", "BigInt", "String", "Symbol",
    "Array", "Math", "JSON", "Promise", "Error", "TypeError", "RangeError",
    "RegExp", "Date", "Map", "Set", "WeakMap", "WeakSet", "Proxy", "Reflect",
    "WebAssembly", "ArrayBuffer", "Uint8Array", "DataView", "Intl", "eval"};

bool is_ident(const std::string& s) {
  if (s.empty() || !(std::isalpha(static_cast<unsigned char>(s[0])) || s[0] == '_')) return false;
  for (char c : s) {
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
  }
  return true;
}

std::string read_ident(const std::string& s, size_t& i) {
  if (i >= s.size() || !(std::isalpha(static_cast<unsigned char>(s[i])) || s[i] == '_')) return "";
  size_t b = i;
  while (i < s.size() && (std::isalnum(static_cast<unsigned char>(s[i])) || s[i] == '_')) ++i;
  return s.substr(b, i - b);
}

void skip_ws(const std::string& s, size_t& i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
}

int line_of(const std::string& text, size_t off, int base_line) {
  int n = base_line;
  for (size_t k = 0; k < off && k < text.size(); ++k) {
    if (text[k] == '\n') ++n;
  }
  return n;
}

struct IType {
  std::string c;         // C type text (fields and prototypes)
  std::string ts;        // "number" | "bigint"
  std::string get_ret;   // getter return / setter parameter type
  std::string set_param;
  bool is_ptr = false;
  bool is_i64 = false;
  bool is_float = false;
  std::string ptr_base;          // pointer base type name
  bool ptr_base_struct = false;  // base is a lang.sync struct
};

// Strict IDL type: a known base identifier with optional trailing '*'s.
IType parse_idl_type(const std::string& raw, const std::set<std::string>& struct_names,
                     const std::string& file, int line) {
  std::string t = trim(raw);
  int stars = 0;
  while (!t.empty() && t.back() == '*') {
    ++stars;
    t.pop_back();
    t = trim(t);
  }
  IType it;
  auto fail = [&]() {
    throw BuildError("Unsupported type '" + trim(raw) + "'",
                     "サポート型: int / int32 / uint / uint32 / int64 / uint64 / "
                     "float / double / bool とポインタ (Type*)、戻り値のみ void",
                     file, line);
  };
  if (!is_ident(t)) fail();
  if (stars > 0) {
    // R-01: the only pointer form the IDL accepts is ONE pointer to a
    // lang.sync struct (`Foo*`). Non-struct pointers (int*, void*, ...) and
    // multi-pointers (Foo**, int**) are structurally rejected: they would be
    // type-erased into the handle scheme and allow arbitrary memory writes.
    if (stars != 1 || struct_names.count(t) == 0) {
      throw BuildError("Unsupported type '" + trim(raw) + "'",
                       "ポインタは lang.sync で定義済み struct の単一ポインタ (<Struct>*) のみ"
                       "使用できます。int*/void* などの非構造体ポインタと多重ポインタ (**) は"
                       "サポートされません",
                       file, line);
    }
    it.c = t + "*";
    it.ts = "number";
    it.get_ret = it.set_param = "unsigned int";
    it.is_ptr = true;
    it.ptr_base = t;
    it.ptr_base_struct = true;
    return it;
  }
  if (t == "int" || t == "int32") {
    it.c = "int";
    it.get_ret = it.set_param = "int";
  } else if (t == "uint" || t == "uint32") {
    it.c = "unsigned int";
    it.get_ret = it.set_param = "unsigned int";
  } else if (t == "int64") {
    it.c = "long long";
    it.get_ret = it.set_param = "long long";
    it.ts = "bigint";
    it.is_i64 = true;
    return it;
  } else if (t == "uint64") {
    it.c = "unsigned long long";
    it.get_ret = it.set_param = "unsigned long long";
    it.ts = "bigint";
    it.is_i64 = true;
    return it;
  } else if (t == "float") {
    it.c = it.get_ret = it.set_param = "float";
    it.is_float = true;
  } else if (t == "double") {
    it.c = it.get_ret = it.set_param = "double";
    it.is_float = true;
  } else if (t == "bool") {
    it.c = "bool";
    it.get_ret = it.set_param = "unsigned char";
  } else {
    fail();
  }
  it.ts = "number";
  return it;
}

// Split at top-level ';' (parens and brackets tracked; braces are illegal
// inside a struct body and rejected by the caller).
std::vector<std::pair<size_t, std::string>> split_idl_stmts(
    const std::string& body, const std::string& file, int base_line,
    const std::string& masked_for_line, size_t body_off) {
  // Returns (offset of the statement inside `body`, statement text). `body_off`
  // is the offset of `body` inside `masked_for_line`, so error lines resolve to
  // the exact statement, not the struct header.
  std::vector<std::pair<size_t, std::string>> stmts;
  std::string cur;
  size_t cur_off = 0;
  int paren = 0, bracket = 0;
  for (size_t k = 0; k < body.size(); ++k) {
    char c = body[k];
    if (c == '(') ++paren;
    else if (c == ')') --paren;
    else if (c == '[') ++bracket;
    else if (c == ']') --bracket;
    else if (c == '{' || c == '}') {
      throw BuildError("Unsupported construct '" + std::string(1, c) + "'",
                       "構造体本体にネストした { } は書けません", file,
                       line_of(masked_for_line, body_off + k, base_line));
    }
    if (c == ';' && paren == 0 && bracket == 0) {
      stmts.emplace_back(cur_off, cur);
      cur.clear();
      cur_off = k + 1;
    } else {
      cur.push_back(c);
    }
  }
  if (!trim(cur).empty()) {
    throw BuildError("Unsupported field declaration", "';' が不足しています", file,
                     line_of(masked_for_line, body_off + cur_off, base_line));
  }
  return stmts;
}

}  // namespace

std::vector<SyncStruct> parse_sync(const std::vector<Block>& sync_blocks) {
  struct RawField {
    std::string type_raw, name;
    int line;
  };
  struct RawMethod {
    std::string name, ret_raw;
    std::vector<std::pair<std::string, std::string>> params;  // (type, name)
    int line;
  };
  struct RawStruct {
    std::string name;
    std::vector<RawField> fields;
    std::vector<RawMethod> methods;
    std::string file;
    int line;
  };

  std::vector<RawStruct> raws;
  std::set<std::string> struct_names;

  for (const auto& block : sync_blocks) {
    std::string masked = mask_source(block.body, "sync");
    const int base_line = block.line + 1;
    size_t i = 0;
    while (true) {
      skip_ws(masked, i);
      if (i >= masked.size()) break;
      size_t kw = i;
      std::string word = read_ident(masked, i);
      if (word != "struct") {
        std::string what = word.empty() ? std::string(1, masked[i]) : word;
        throw BuildError("Unsupported construct '" + what + "'",
                         "'struct <Name> { ... }' の形式で記述してください", block.file,
                         line_of(masked, kw, base_line));
      }
      skip_ws(masked, i);
      size_t name_off = i;
      std::string name = read_ident(masked, i);
      if (!is_ident(name)) {
        throw BuildError("Unsupported struct name '" + name + "'",
                         "構造体名は識別子で記述してください", block.file,
                         line_of(masked, name_off, base_line));
      }
      if (kTsReserved.count(name)) {
        throw BuildError("Struct name '" + name + "' is reserved",
                         "TS クラスの予約名 (constructor/free/fromPtr/liveCount/slotCount/_ptr) は使用できません",
                         block.file, line_of(masked, name_off, base_line));
      }
      if (kForbiddenGlobals.count(name)) {
        throw BuildError("Struct name '" + name + "' collides with a JS global",
                         "グローバルオブジェクト (native/sharedBuffer/Object/Math など) と同名の構造体は"
                         "生成クラスが JS ランタイムを破壊するため使用できません",
                         block.file, line_of(masked, name_off, base_line));
      }
      if (struct_names.count(name)) {
        throw BuildError("Duplicate struct '" + name + "'",
                         "同名の構造体が複数定義されています", block.file,
                         line_of(masked, name_off, base_line));
      }
      skip_ws(masked, i);
      if (i >= masked.size() || masked[i] != '{') {
        throw BuildError("Unsupported struct declaration",
                         "struct <Name> { ... } の形式で記述してください", block.file,
                         line_of(masked, i, base_line));
      }
      size_t body_open = i;
      size_t depth = 0, j = i;
      for (; j < masked.size(); ++j) {
        if (masked[j] == '{') ++depth;
        else if (masked[j] == '}' && --depth == 0) break;
      }
      if (j >= masked.size()) {
        throw BuildError("Unterminated struct body",
                         "struct の '}' が閉じていません", block.file,
                         line_of(masked, body_open, base_line));
      }
      std::string body = masked.substr(body_open + 1, j - body_open - 1);
      i = j + 1;
      skip_ws(masked, i);
      if (i < masked.size() && masked[i] == ';') ++i;  // optional trailing ';'

      RawStruct rs;
      rs.name = name;
      rs.file = block.file;
      rs.line = line_of(masked, name_off, base_line);
      struct_names.insert(name);

      for (const auto& [soff, stmt] :
           split_idl_stmts(body, block.file, base_line, masked, body_open + 1)) {
        std::string st = trim(stmt);
        if (st.empty()) continue;
        int ln = line_of(masked, body_open + 1 + soff, base_line);
        size_t pos = st.find('(');
        if (pos == std::string::npos) {
          // Field: `<Type> <name>`
          size_t end = st.find_last_not_of(" \t");
          size_t begin = end;
          while (begin != std::string::npos &&
                 (std::isalnum(static_cast<unsigned char>(st[begin])) || st[begin] == '_')) {
            if (begin == 0) break;
            --begin;
          }
          if (begin != std::string::npos && !(std::isalnum(static_cast<unsigned char>(st[begin])) || st[begin] == '_')) {
            ++begin;
          }
          std::string fname = st.substr(begin, end - begin + 1);
          std::string ftype = trim(st.substr(0, begin));
          if (!is_ident(fname) || ftype.empty()) {
            throw BuildError("Unsupported field declaration '" + st + "'",
                             "型 名前; の形式で記述してください", block.file, ln);
          }
          if (kTsReserved.count(fname)) {
            throw BuildError("Field name '" + fname + "' is reserved",
                             "TS クラスの予約名 (constructor/free/fromPtr/liveCount/slotCount/_ptr) は使用できません",
                             block.file, ln);
          }
          for (const auto& f : rs.fields) {
            if (f.name == fname) {
              throw BuildError("Duplicate field '" + fname + "'",
                               "同名フィールドが複数あります", block.file, ln);
            }
          }
          rs.fields.push_back(RawField{ftype, fname, ln});
        } else {
          // Method: `<name>(<Type> <param>, ...) -> <Ret>`
          size_t b = pos;
          while (b > 0 && std::isspace(static_cast<unsigned char>(st[b - 1]))) --b;
          size_t name_begin = b;
          while (name_begin > 0 &&
                 (std::isalnum(static_cast<unsigned char>(st[name_begin - 1])) || st[name_begin - 1] == '_')) {
            --name_begin;
          }
          std::string mname = st.substr(name_begin, b - name_begin);
          if (!is_ident(mname)) {
            throw BuildError("Unsupported method declaration '" + st + "'",
                             "メソッド名(型 パラメータ, ...) -> 戻り値型; の形式で記述してください",
                             block.file, ln);
          }
          size_t close = st.find(')', pos);
          if (close == std::string::npos) {
            throw BuildError("Unsupported method declaration '" + st + "'",
                             "')' がありません", block.file, ln);
          }
          std::string params_raw = st.substr(pos + 1, close - pos - 1);
          std::string tail = trim(st.substr(close + 1));
          if (tail.rfind("->", 0) != 0) {
            throw BuildError("Unsupported method declaration '" + st + "'",
                             "-> 戻り値型; を指定してください", block.file, ln);
          }
          std::string ret_raw = trim(tail.substr(2));
          if (ret_raw.empty()) {
            throw BuildError("Unsupported method declaration '" + st + "'",
                             "-> 戻り値型; を指定してください", block.file, ln);
          }
          RawMethod rm;
          rm.name = mname;
          rm.ret_raw = ret_raw;
          rm.line = ln;
          // Parameters: `<Type> <name>` split at top-level commas.
          std::string cur;
          int paren = 0;
          std::vector<std::string> plist;
          for (char c : params_raw) {
            if (c == '(') ++paren;
            else if (c == ')') --paren;
            if (c == ',' && paren == 0) {
              plist.push_back(cur);
              cur.clear();
            } else {
              cur.push_back(c);
            }
          }
          if (!trim(cur).empty()) plist.push_back(cur);
          for (const auto& p : plist) {
            std::string pt = trim(p);
            if (pt.empty()) continue;
            if (pt == "void" && plist.size() == 1 && rm.params.empty()) continue;  // (void)
            size_t pe = pt.find_last_not_of(" \t");
            size_t pb = pe;
            while (pb != std::string::npos &&
                   (std::isalnum(static_cast<unsigned char>(pt[pb])) || pt[pb] == '_')) {
              if (pb == 0) break;
              --pb;
            }
            if (pb != std::string::npos && !(std::isalnum(static_cast<unsigned char>(pt[pb])) || pt[pb] == '_')) {
              ++pb;
            }
            std::string pname = pt.substr(pb, pe - pb + 1);
            std::string ptype = trim(pt.substr(0, pb));
            if (!is_ident(pname) || ptype.empty()) {
              throw BuildError("Unsupported parameter '" + pt + "'",
                               "型 名前; の形式で記述してください", block.file, ln);
            }
            if (kTsReserved.count(pname)) {
              throw BuildError("Parameter name '" + pname + "' is reserved",
                               "予約名は使用できません", block.file, ln);
            }
            rm.params.emplace_back(ptype, pname);
          }
          rs.methods.push_back(std::move(rm));
        }
      }
      raws.push_back(std::move(rs));
    }
  }

  // Resolve types (now that every struct name is known) and enforce the
  // remaining collisions. Both the raw C API (T_alloc, T_<method>, ...) and
  // the exported handle API (T_alloc_h, T_<method>_h, ...) reserve names.
  std::vector<SyncStruct> out;
  for (const auto& rs : raws) {
    SyncStruct s;
    s.name = rs.name;
    s.file = rs.file;
    s.line = rs.line;
    std::set<std::string> generated = {
        rs.name + "_alloc", rs.name + "_free", rs.name + "_alloc_h", rs.name + "_free_h",
        rs.name + "_live_count", rs.name + "_slot_count"};
    auto add_sym = [&](const std::string& sym, const std::string& who, int ln) {
      if (generated.count(sym)) {
        throw BuildError("Name '" + who + "' collides with generated symbol '" + sym + "'",
                         "自動生成名 (alloc/free/live_count/slot_count/get_*/set_* と _h 版) と"
                         "衝突します。名前を変更してください",
                         rs.file, ln);
      }
      generated.insert(sym);
    };
    for (const auto& rf : rs.fields) {
      FieldInfo f;
      IType it = parse_idl_type(rf.type_raw, struct_names, rs.file, rf.line);
      f.c_type = it.c;
      f.name = rf.name;
      f.ts_type = it.ts;
      f.get_ret = it.get_ret;
      f.set_param = it.set_param;
      f.is_ptr = it.is_ptr;
      f.is_i64 = it.is_i64;
      f.is_float = it.is_float;
      f.ptr_base = it.ptr_base;
      f.ptr_base_struct = it.ptr_base_struct;
      add_sym(rs.name + "_get_" + rf.name, rf.name, rf.line);
      add_sym(rs.name + "_get_" + rf.name + "_h", rf.name, rf.line);
      add_sym(rs.name + "_set_" + rf.name, rf.name, rf.line);
      add_sym(rs.name + "_set_" + rf.name + "_h", rf.name, rf.line);
      s.fields.push_back(std::move(f));
    }
    for (const auto& rm : rs.methods) {
      if (kTsReserved.count(rm.name)) {
        throw BuildError("Method name '" + rm.name + "' is reserved",
                         "TS クラスの予約名 (constructor/free/fromPtr/liveCount/slotCount/_ptr) は使用できません",
                         rs.file, rm.line);
      }
      for (const auto& f : s.fields) {
        if (f.name == rm.name) {
          throw BuildError("Method '" + rm.name + "' collides with field '" + f.name + "'",
                           "フィールドと同名のメソッドは TS クラスで衝突します", rs.file, rm.line);
        }
      }
      MethodInfo m;
      m.name = rm.name;
      m.c_func = rs.name + "_" + rm.name;
      for (const auto& existing : s.methods) {
        if (existing.name == m.name) {
          throw BuildError("Duplicate method '" + m.c_func + "'",
                           "同名メソッドの多重定義はサポートされません", rs.file, rm.line);
        }
      }
      add_sym(m.c_func, rm.name, rm.line);
      add_sym(m.c_func + "_h", rm.name, rm.line);
      if (trim(rm.ret_raw) == "void") {
        m.ts_return = "void";
        m.c_return = "void";
      } else {
        IType rt = parse_idl_type(rm.ret_raw, struct_names, rs.file, rm.line);
        m.ts_return = rt.ts;
        m.c_return = rt.c;
        m.ret_is_ptr = rt.is_ptr;
        m.ret_ptr_base = rt.ptr_base;
        m.ret_ptr_base_struct = rt.ptr_base_struct;
      }
      for (const auto& [ptype, pname] : rm.params) {
        IType pt = parse_idl_type(ptype, struct_names, rs.file, rm.line);
        m.ts_params.push_back(pt.ts);
        m.c_params.push_back(pt.c);
        m.param_names.push_back(pname);
        m.param_is_ptr.push_back(pt.is_ptr);
        m.param_ptr_base.push_back(pt.ptr_base);
        m.param_ptr_base_struct.push_back(pt.ptr_base_struct);
      }
      s.methods.push_back(std::move(m));
    }
    s.type_id = static_cast<int>(out.size()) + 1;
    out.push_back(std::move(s));
  }
  return out;
}

namespace {

std::string ts_ann(const std::string& t, ScriptMode::E mode) {
  if (mode == ScriptMode::Js) return "";
  return ": " + t;
}

std::string line_directive(const std::string& file, int line) {
  return "#line " + std::to_string(line) + " \"" + c_string_escape(file) + "\"\n";
}

std::string gen_struct_c_code(const SyncStruct& s, bool threads,
                              const std::map<std::string, int>& type_ids) {
  // Handle-based generation (red-team C-01..C-05, C-07, C-08):
  //  * JS never sees raw pointers - every JS-facing entry takes an opaque
  //    handle (slot index + generation) resolved through the authoritative
  //    slot table. Forged cells, UAF, double frees and cross-type pointers
  //    are rejected structurally.
  //  * The raw C API (T_alloc/T_free/T_get_*/T_set_*/user method bodies) keeps
  //    spec signatures for lang.c, validates slab membership + liveness
  //    before any dereference, and is NOT exported to JS.
  std::ostringstream o;
  const std::string& n = s.name;
  const std::string cell = "__jelll_cell_" + n;
  const std::string slab = "__jelll_slab_" + n;
  const std::string ty = std::to_string(s.type_id);
  std::string lock = threads ? "  std::lock_guard<std::mutex> __jelll_lk(__jelll_mtx);\n" : "";
  auto type_of = [&](const std::string& base, bool base_struct) -> std::string {
    if (!base_struct) return "0";
    auto it = type_ids.find(base);
    return it == type_ids.end() ? "0" : std::to_string(it->second);
  };
  o << "// ---- JELLL lang.sync: " << n << " (handle-based slab pool) ----\n";
  o << "struct " << slab << ";\n";
  o << "struct " << cell << " {\n";
  o << "  alignas(" << n << ") unsigned char raw[sizeof(" << n << ")];\n";
  o << "  " << cell << "* next_free;\n";
  o << "  " << slab << "* owner;\n";
  o << "  int idx;\n";
  o << "  unsigned hidx;\n";
  o << "  " << n << "* item() { return std::launder(reinterpret_cast<" << n << "*>(raw)); }\n";
  o << "};\n";
  o << "struct " << slab << " {\n";
  o << "  " << slab << " *next_all, *prev_all, *next_partial, *prev_partial;\n";
  o << "  " << cell << "* free_head;\n";
  o << "  int free_count;\n";
  o << "  alignas(" << cell << ") unsigned char cells[256 * sizeof(" << cell << ")];\n";
  o << "};\n";
  o << "static " << slab << "* __jelll_all_" << n << " = 0;\n";
  o << "static " << slab << "* __jelll_partial_" << n << " = 0;\n";
  o << "static " << slab << "* __jelll_cached_" << n << " = 0;\n";
  o << "static int __jelll_live_" << n << " = 0;\n";
  o << "static int __jelll_slots_" << n << " = 0;\n";
  o << "static " << cell << "* __jelll_cell_at_" << n << "(" << slab << "* s, int i) {\n";
  o << "  return (" << cell << "*)(void*)(s->cells + (size_t)i * sizeof(" << cell << "));\n";
  o << "}\n";
  // Pure arithmetic pre-check: no dereference happens before validation.
  o << "static bool __jelll_inbounds_" << n << "(const void* q, size_t need) {\n";
  o << "  uintptr_t p = (uintptr_t)q;\n";
  o << "  uintptr_t heap_end = (uintptr_t)__builtin_wasm_memory_size(0) * 65536u;\n";
  o << "  return p >= 16u && (p % alignof(" << cell << ")) == 0 && need <= heap_end && p <= heap_end - need;\n";
  o << "}\n";
  // Slab membership + liveness. The slab list (not the caller) decides what is
  // a real cell; the slot table (not the cell) decides what is alive.
  o << "static " << cell << "* __jelll_live_cell_" << n << "(const void* q) {\n";
  o << "  if (!q || !__jelll_inbounds_" << n << "(q, sizeof(" << n << "))) return 0;\n";
  o << "  uintptr_t p = (uintptr_t)q;\n";
  o << "  for (" << slab << "* s = __jelll_all_" << n << "; s; s = s->next_all) {\n";
  o << "    uintptr_t b = (uintptr_t)(void*)s->cells;\n";
  o << "    uintptr_t e = b + 256 * sizeof(" << cell << ");\n";
  o << "    if (p < b || p >= e) continue;\n";
  o << "    if ((p - b) % sizeof(" << cell << ") != 0) return 0;\n";
  o << "    " << cell << "* c = (" << cell << "*)(void*)p;\n";
  o << "    unsigned i = c->hidx;\n";
  o << "    if (i >= __jelll_htab_used || __jelll_htab[i].cell != (void*)c) return 0;  // dead\n";
  o << "    return c;\n";
  o << "  }\n";
  o << "  return 0;\n";
  o << "}\n";
  o << "static unsigned __jelll_handle_of_" << n << "(const void* q) {\n";
  o << "  " << cell << "* c = __jelll_live_cell_" << n << "(q);\n";
  o << "  if (!c) return 0;\n";
  o << "  unsigned i = c->hidx;\n";
  o << "  if (__jelll_htab[i].cell != (void*)c) return 0;\n";
  o << "  return (__jelll_htab[i].gen << 12) | i;\n";
  o << "}\n";
  // ---- allocator (impl functions assume the lock is held in --threads) ----
  o << "static " << cell << "* __jelll_alloc_impl_" << n << "(void) {\n";
  o << "  " << slab << "* s = __jelll_partial_" << n << ";\n";
  o << "  if (!s) {\n";
  o << "    s = __jelll_cached_" << n << ";\n";
  o << "    __jelll_cached_" << n << " = 0;\n";
  o << "    if (s) { s->prev_partial = 0; s->next_partial = __jelll_partial_" << n << ";\n";
  o << "      if (__jelll_partial_" << n << ") __jelll_partial_" << n << "->prev_partial = s;\n";
  o << "      __jelll_partial_" << n << " = s; }\n";
  o << "  }\n";
  o << "  if (!s) {\n";
  o << "    s = (" << slab << "*)std::malloc(sizeof(" << slab << "));\n";
  o << "    if (!s) return 0;\n";
  o << "    ::new ((void*)s) " << slab << "{};\n";
  o << "    s->free_count = 256;\n";
  o << "    s->free_head = 0;\n";
  o << "    s->prev_partial = 0;\n";
  o << "    s->next_partial = 0;\n";
  o << "    for (int i = 255; i >= 0; --i) {\n";
  o << "      " << cell << "* c = ::new ((void*)&s->cells[(size_t)i * sizeof(" << cell << ")]) " << cell << "{};\n";
  o << "      c->owner = s; c->idx = i;\n";
  o << "      c->hidx = __jelll_hslot_new();\n";
  o << "      if (c->hidx == 0xffffffffu) {  // handle table exhausted (65536 cells)\n";
  o << "        for (int k = 255; k > i; --k) __jelll_hslot_release(__jelll_cell_at_" << n << "(s, k)->hidx);\n";
  o << "        s->~" << slab << "(); std::free((void*)s);\n";
  o << "        return 0;\n";
  o << "      }\n";
  o << "      __jelll_htab[c->hidx].type = " << ty << ";\n";
  o << "      c->next_free = s->free_head;\n";
  o << "      s->free_head = c;\n";
  o << "    }\n";
  o << "    __jelll_slots_" << n << " += 256;\n";
  o << "    s->next_all = __jelll_all_" << n << ";\n";
  o << "    s->prev_all = 0;\n";
  o << "    if (__jelll_all_" << n << ") __jelll_all_" << n << "->prev_all = s;\n";
  o << "    __jelll_all_" << n << " = s;\n";
  o << "    s->next_partial = __jelll_partial_" << n << ";\n";
  o << "    if (__jelll_partial_" << n << ") __jelll_partial_" << n << "->prev_partial = s;\n";
  o << "    __jelll_partial_" << n << " = s;\n";
  o << "  }\n";
  o << "  " << cell << "* c = s->free_head;\n";
  o << "  s->free_head = c->next_free;\n";
  o << "  --s->free_count;\n";
  o << "  if (s->free_count == 0) {\n";
  o << "    if (s->prev_partial) s->prev_partial->next_partial = s->next_partial;\n";
  o << "    else __jelll_partial_" << n << " = s->next_partial;\n";
  o << "    if (s->next_partial) s->next_partial->prev_partial = s->prev_partial;\n";
  o << "  }\n";
  o << "  unsigned i = c->hidx;\n";
  o << "  // R-05: arm keeps the slot's generation monotonic (never reset). At the\n";
  o << "  // ceiling the slot retires and the cell continues on a VIRGIN slot.\n";
  o << "  i = __jelll_slot_arm(i, " << ty << ");\n";
  o << "  if (i == 0xffffffffu) {  // no safe slot left: roll the cell back\n";
  o << "    c->next_free = s->free_head;\n  s->free_head = c;\n  ++s->free_count;\n";
  o << "    if (s->free_count == 1) {\n";
  o << "      s->prev_partial = 0;\n  s->next_partial = __jelll_partial_" << n << ";\n";
  o << "      if (__jelll_partial_" << n << ") __jelll_partial_" << n << "->prev_partial = s;\n";
  o << "      __jelll_partial_" << n << " = s;\n";
  o << "    }\n";
  o << "    return 0;\n";
  o << "  }\n";
  o << "  c->hidx = i;\n";
  o << "  __jelll_htab[i].cell = c;\n";
  o << "  __jelll_htab[i].pins = 0;\n  __jelll_htab[i].freeing = 0;\n";
  o << "  ++__jelll_live_" << n << ";\n";
  o << "  ::new ((void*)c->raw) " << n << "();\n";
  o << "  return c;\n";
  o << "}\n";
  o << "static void __jelll_free_impl_" << n << "(" << n << "* self) {\n";
  o << "  " << cell << "* c = __jelll_live_cell_" << n << "((const void*)self);\n";
  o << "  if (!c) return;  // wild / foreign / already-freed pointer: reject\n";
  o << "  " << slab << "* s = c->owner;\n";
  o << "  __jelll_htab[c->hidx].cell = 0;  // all handles for this cell die now\n";
  o << "  self->~" << n << "();\n";
  o << "  bool was_full = (s->free_count == 0);\n";
  o << "  c->next_free = s->free_head;\n";
  o << "  s->free_head = c;\n";
  o << "  ++s->free_count;\n";
  o << "  if (was_full) {\n";
  o << "    s->prev_partial = 0;\n";
  o << "    s->next_partial = __jelll_partial_" << n << ";\n";
  o << "    if (__jelll_partial_" << n << ") __jelll_partial_" << n << "->prev_partial = s;\n";
  o << "    __jelll_partial_" << n << " = s;\n";
  o << "  }\n";
  o << "  --__jelll_live_" << n << ";\n";
  o << "  if (s->free_count == 256) {  // empty slab: keep one warm, release the rest\n";
  o << "    if (s->prev_partial) s->prev_partial->next_partial = s->next_partial;\n";
  o << "    else __jelll_partial_" << n << " = s->next_partial;\n";
  o << "    if (s->next_partial) s->next_partial->prev_partial = s->prev_partial;\n";
  o << "    if (!__jelll_cached_" << n << ") {\n";
  o << "      __jelll_cached_" << n << " = s;\n";
  o << "    } else {\n";
  o << "      if (s->prev_all) s->prev_all->next_all = s->next_all;\n";
  o << "      else __jelll_all_" << n << " = s->next_all;\n";
  o << "      if (s->next_all) s->next_all->prev_all = s->prev_all;\n";
  o << "      for (int k = 0; k < 256; ++k) {\n";
  o << "        __jelll_hslot_release(__jelll_cell_at_" << n << "(s, k)->hidx);\n";
  o << "      }\n";
  o << "      __jelll_slots_" << n << " -= 256;\n";
  o << "      s->~" << slab << "();\n";
  o << "      std::free((void*)s);\n";
  o << "    }\n";
  o << "  }\n";
  o << "}\n";
  // ---- raw C API (lang.c interop; not exported to JS) ----
  o << "extern \"C\" {\n";
  o << n << "* " << n << "_alloc(void) {\n" << lock;
  o << "  " << cell << "* c = __jelll_alloc_impl_" << n << "();\n";
  o << "  return c ? c->item() : 0;\n}\n";
  o << "void " << n << "_free(" << n << "* self) {\n" << lock;
  o << "  __jelll_free_impl_" << n << "(self);\n}\n";
  o << "int " << n << "_live_count(void) {\n" << lock << "  return __jelll_live_" << n << ";\n}\n";
  o << "int " << n << "_slot_count(void) {\n" << lock << "  return __jelll_slots_" << n << ";\n}\n";
  for (const auto& f : s.fields) {
    std::string getter_ret = f.is_ptr ? "unsigned int" : f.get_ret;
    std::string setter_param = f.is_ptr ? "unsigned int" : f.set_param;
    // raw getter (C callers; validated but trust-oriented)
    o << getter_ret << " " << n << "_get_" << f.name << "(" << n << "* self) {\n" << lock;
    o << "  " << cell << "* c = __jelll_live_cell_" << n << "((const void*)self);\n";
    if (f.is_ptr) {
      o << "  return c ? (unsigned int)(uintptr_t)(void*)c->item()->" << f.name << " : 0u;\n}\n";
    } else {
      o << "  return c ? (" << getter_ret << ")c->item()->" << f.name << " : (" << getter_ret << ")0;\n}\n";
    }
    // raw setter
    o << "void " << n << "_set_" << f.name << "(" << n << "* self, " << setter_param << " v) {\n" << lock;
    o << "  " << cell << "* c = __jelll_live_cell_" << n << "((const void*)self);\n";
    if (f.is_ptr) {
      o << "  if (c) c->item()->" << f.name << " = (decltype(c->item()->" << f.name << "))(uintptr_t)v;\n}\n";
    } else if (f.is_float) {
      o << "  if (c) c->item()->" << f.name << " = v;\n}\n";
    } else {
      o << "  if (c) c->item()->" << f.name << " = (" << f.get_ret << ")v;\n}\n";
    }
    // handle getter (JS)
    o << getter_ret << " " << n << "_get_" << f.name << "_h(unsigned h) {\n" << lock;
    o << "  void* p = __jelll_resolve(h, " << ty << ");\n";
    if (f.is_ptr) {
      std::string mapper = f.ptr_base_struct ? ("__jelll_handle_of_" + f.ptr_base) : "__jelll_handle_of_any";
      o << "  return p ? " << mapper << "((const void*)(((" << n << "*)p)->" << f.name << ")) : 0u;\n}\n";
    } else {
      o << "  return p ? (" << getter_ret << ")((" << n << "*)p)->" << f.name << " : (" << getter_ret << ")0;\n}\n";
    }
    // handle setter (JS)
    o << "void " << n << "_set_" << f.name << "_h(unsigned h, " << setter_param << " v) {\n" << lock;
    o << "  void* p = __jelll_resolve(h, " << ty << ");\n";
    if (f.is_ptr) {
      std::string want = type_of(f.ptr_base, f.ptr_base_struct);
      o << "  if (!p) return;\n";
      o << "  void* q = v ? __jelll_resolve(v, " << want << ") : 0;\n";
      o << "  ((" << n << "*)p)->" << f.name << " = (decltype(((" << n << "*)p)->" << f.name << "))q;\n}\n";
    } else if (f.is_float) {
      o << "  if (p) ((" << n << "*)p)->" << f.name << " = v;\n}\n";
    } else {
      o << "  if (p) ((" << n << "*)p)->" << f.name << " = (" << f.get_ret << ")v;\n}\n";
    }
  }
  // ---- method thunks: handle in, user body called with raw pointers ----
  // R-02: receiver and pointer args are PINNED for the whole user call;
  // R-10: pointer returns are mapped back through __jelll_handle_of_*.
  for (const auto& m : s.methods) {
    std::string h_ret = m.ret_is_ptr ? "unsigned int" : m.c_return;
    o << h_ret << " " << m.c_func << "_h(unsigned self_h";
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      o << ", " << (m.param_is_ptr[k] ? "unsigned int" : m.c_params[k]) << " " << m.param_names[k];
    }
    o << ") {\n";
    o << "  " << n << "* self;\n";
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      if (m.param_is_ptr[k]) {
        o << "  " << m.c_params[k] << " " << m.param_names[k] << "_p = 0;\n";
        o << "  unsigned " << m.param_names[k] << "_idx = 0xffffffffu;\n";
      }
    }
    o << "  unsigned __jelll_self_idx = 0xffffffffu;\n";
    o << "  {\n" << lock;
    o << "    self = (" << n << "*)__jelll_pin(self_h, " << ty << ");\n";
    o << "    if (self) __jelll_self_idx = self_h & 0xfffu;\n";
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      if (m.param_is_ptr[k]) {
        std::string want = type_of(m.param_ptr_base[k], m.param_ptr_base_struct[k]);
        o << "    " << m.param_names[k] << "_p = (" << m.c_params[k] << ")__jelll_pin("
          << m.param_names[k] << ", " << want << ");\n";
        o << "    if (" << m.param_names[k] << "_p) " << m.param_names[k]
          << "_idx = " << m.param_names[k] << " & 0xfffu;\n";
      }
    }
    o << "  }\n";
    std::string unpin_all =
        "  {\n" + lock + "    if (__jelll_self_idx != 0xffffffffu) __jelll_unpin(__jelll_self_idx);\n";
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      if (m.param_is_ptr[k]) {
        unpin_all += "    if (" + m.param_names[k] + "_idx != 0xffffffffu) __jelll_unpin(" +
                     m.param_names[k] + "_idx);\n";
      }
    }
    unpin_all += "  }\n";
    std::string args;
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      args += ", " + (m.param_is_ptr[k] ? m.param_names[k] + "_p" : m.param_names[k]);
    }
    if (m.ts_return == "void") {
      o << "  if (!self) {" << unpin_all << "    return;\n  }\n";
      o << "  " << m.c_func << "(self" << args << ");\n";
      o << unpin_all;
    } else if (m.ret_is_ptr) {
      // R-10: the raw pointer return must become a typed handle.
      std::string mapper = "__jelll_handle_of_" + m.ret_ptr_base;
      o << "  if (!self) {" << unpin_all << "    return 0u;\n  }\n";
      o << "  " << m.c_return << " r = " << m.c_func << "(self" << args << ");\n";
      o << "  unsigned __jelll_rh;\n";
      o << "  {\n" << lock << "    __jelll_rh = " << mapper << "((const void*)r);\n";
      for (size_t k = 0; k < m.param_names.size(); ++k) {
        if (m.param_is_ptr[k]) {
          o << "    if (" << m.param_names[k] << "_idx != 0xffffffffu) __jelll_unpin("
            << m.param_names[k] << "_idx);\n";
        }
      }
      o << "    if (__jelll_self_idx != 0xffffffffu) __jelll_unpin(__jelll_self_idx);\n  }\n";
      o << "  return __jelll_rh;\n";
    } else {
      std::string zero = "(" + m.c_return + ")0";
      o << "  if (!self) {" << unpin_all << "    return " << zero << ";\n  }\n";
      o << "  " << m.c_return << " r = " << m.c_func << "(self" << args << ");\n";
      o << unpin_all;
      o << "  return r;\n";
    }
    o << "}\n";
  }
  // ---- exported handle API: alloc/free ----
  o << "unsigned int " << n << "_alloc_h(void) {\n" << lock;
  o << "  " << cell << "* c = __jelll_alloc_impl_" << n << "();\n";
  o << "  if (!c) return 0;\n";
  o << "  unsigned i = c->hidx;\n";
  o << "  return (__jelll_htab[i].gen << 12) | i;\n}\n";
  o << "void " << n << "_free_h(unsigned h) {\n";
  // R-02: drain in-flight pins before destruction; refuse new pins meanwhile.
  o << "  int __jelll_owned = 0;\n";
  o << "  for (;;) {\n";
  o << "    void* p = 0;\n";
  o << "    int go;\n";
  o << "    {\n" << lock;
  o << "      go = __jelll_free_begin(h, " << ty << ", &p, &__jelll_owned);\n";
  o << "    }\n";
  o << "    if (!go) {\n";
  o << "      std::this_thread::yield();\n";
  o << "      continue;\n";
  o << "    }\n";
  o << "    if (!p) return;\n";
  o << "    {\n" << lock;
  o << "      // R-02: destroy exactly once - revalidate slot index + generation\n";
  o << "      // under the lock right before the destructor runs, so a recycled\n";
  o << "      // cell can never be destroyed through a stale destroy ticket.\n";
  o << "      unsigned __jelll_idx = h & 0xfffu;\n";
  o << "      unsigned __jelll_gen = h >> 12;\n";
  o << "      if (__jelll_idx < __jelll_htab_used && __jelll_htab[__jelll_idx].cell == p &&\n";
  o << "          __jelll_htab[__jelll_idx].gen == __jelll_gen &&\n";
  o << "          __jelll_htab[__jelll_idx].type == " << ty << ") {\n";
  o << "        __jelll_free_impl_" << n << "((" << n << "*)p);\n";
  o << "      }\n";
  o << "    }\n";
  o << "    return;\n";
  o << "  }\n}\n";
  o << "}  // extern \"C\"\n";
  return o.str();
}

std::string gen_struct_ts(const SyncStruct& s, ScriptMode::E mode) {
  // TS wrapper: `_ptr` holds an opaque HANDLE (never a raw address). All
  // native calls go through the exported *_h entry points, which validate
  // slot + generation + type before touching memory (red-team C-02/C-04).
  std::ostringstream o;
  const std::string& n = s.name;
  std::string num = mode == ScriptMode::Js ? "" : ": number";
  std::string cls_ret = mode == ScriptMode::Js ? "" : ": " + n;
  std::string void_ret = mode == ScriptMode::Js ? "" : ": void";
  std::string check = "    if (!this._ptr) throw new Error(\"[JELLL] " + n + ": 解放済みインスタンスです\");\n";
  o << "// ---- JELLL lang.sync: " << n << " ----\n";
  o << "globalThis." << n << " = class " << n << " {\n";
  o << "  _ptr" << num << " = 0;\n";
  o << "  constructor() {\n";
  o << "    this._ptr = native." << n << "_alloc_h();\n";
  o << "    if (!this._ptr) throw new Error(\"[JELLL] " << n + ": メモリ確保に失敗しました\");\n";
  o << "  }\n";
  o << "  static liveCount()" << num << " { return native." << n << "_live_count(); }\n";
  o << "  static slotCount()" << num << " { return native." << n << "_slot_count(); }\n";
  o << "  static fromPtr(ptr" << num << ")" << cls_ret << " {\n";
  o << "    if (typeof ptr !== \"number\" || !Number.isInteger(ptr) || ptr <= 0 || ptr > 0xffffffff)\n";
  o << "      throw new Error(\"[JELLL] " << n + ": 不正なハンドルです\");\n";
  o << "    const obj = Object.create(" << n << ".prototype);\n";
  o << "    obj._ptr = ptr;\n";
  o << "    return obj;\n  }\n";
  o << "  free()" << void_ret << " {\n";
  o << "    if (this._ptr) { native." << n << "_free_h(this._ptr); this._ptr = 0; }\n  }\n";
  for (const auto& f : s.fields) {
    std::string t = ts_ann(f.ts_type, mode);
    o << "  get " << f.name << "()" << t << " {\n" << check
      << "    return native." << n << "_get_" << f.name << "_h(this._ptr);\n  }\n";
    o << "  set " << f.name << "(v" << t << ") {\n" << check
      << "    native." << n << "_set_" << f.name << "_h(this._ptr, v);\n  }\n";
  }
  for (const auto& m : s.methods) {
    std::string params;
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      if (k) params += ", ";
      params += m.param_names[k] + ts_ann(m.ts_params[k], mode);
    }
    std::string ret = m.ts_return == "void" ? void_ret : ts_ann(m.ts_return, mode);
    o << "  " << m.name << "(" << params << ")" << ret << " {\n" << check;
    o << "    ";
    if (m.ts_return != "void") o << "return ";
    o << "native." << m.c_func << "_h(this._ptr";
    for (const auto& pn : m.param_names) o << ", " << pn;
    o << ");\n  }\n";
  }
  o << "};\n";
  return o.str();
}

std::string gen_struct_decl(const SyncStruct& s) {
  std::ostringstream o;
  o << "  class " << s.name << " {\n";
  o << "    constructor();\n";
  o << "    static liveCount(): number;\n";
  o << "    static slotCount(): number;\n";
  o << "    static fromPtr(ptr: number): " << s.name << ";\n";
  o << "    free(): void;\n";
  for (const auto& f : s.fields) {
    o << "    " << f.name << ": " << f.ts_type << ";\n";
  }
  for (const auto& m : s.methods) {
    o << "    " << m.name << "(";
    for (size_t k = 0; k < m.param_names.size(); ++k) {
      if (k) o << ", ";
      o << m.param_names[k] << ": " << m.ts_params[k];
    }
    o << "): " << m.ts_return << ";\n";
  }
  o << "  };\n";
  return o.str();
}

}  // namespace

Generated generate(const Program& program, const std::vector<SyncStruct>& structs,
                   ScriptMode::E mode, bool dev_mode, bool threads) {
  Generated g;

  // ---- core.cpp: generated IDL code first, then user blocks ----
  // Layout: struct declarations + slab allocator/accessors + `extern "C"`
  // method prototypes + link references, followed by the user's lang.c code.
  // The prototypes give user method bodies C linkage automatically (no
  // mangling, `extern "C"` optional on the user side); the link references
  // make a missing method body fail at link time instead of at runtime.
  {
    std::ostringstream o;
    o << "// Generated by JELLL V5. Do not edit.\n";
    o << "#include <cstdint>\n#include <cstdlib>\n#include <cstring>\n#include <new>\n";
    o << "#include <thread>\n";
    if (threads) {
      o << "#include <mutex>\nstatic std::mutex __jelll_mtx;\n";
    }

    std::set<std::string> sync_names;
    for (const auto& s : structs) sync_names.insert(s.name);
    std::vector<const Block*> cblocks;
    for (const auto& b : program.blocks) {
      if (b.lang != "c") continue;
      cblocks.push_back(&b);
      // Legacy syntax guard: the C++ scraping era marker must be gone.
      if (b.body.find("@jelll-sync") != std::string::npos) {
        throw BuildError("Legacy @jelll-sync syntax",
                         "@jelll-sync は廃止されました。lang.sync { ... } ブロックへ移行してください"
                         "（構造体・メソッド宣言は lang.sync、メソッド本体は lang.c に記述）",
                         b.file, b.line + 1);
      }
      // A sync struct must not be redefined in lang.c.
      std::string masked = mask_source(b.body, "c");
      for (const auto& name : sync_names) {
        size_t p = 0;
        while ((p = masked.find(name, p)) != std::string::npos) {
          bool word_ok =
              (p == 0 || !(std::isalnum(static_cast<unsigned char>(masked[p - 1])) || masked[p - 1] == '_')) &&
              (p + name.size() >= masked.size() ||
               !(std::isalnum(static_cast<unsigned char>(masked[p + name.size()])) || masked[p + name.size()] == '_'));
          if (word_ok) {
            size_t back = p;
            while (back > 0 && std::isspace(static_cast<unsigned char>(masked[back - 1]))) --back;
            bool is_def = false;
            for (const char* kw : {"struct", "class"}) {
              size_t wl = std::strlen(kw);
              if (back >= wl && masked.compare(back - wl, wl, kw) == 0 &&
                  (back == wl || !(std::isalnum(static_cast<unsigned char>(masked[back - wl - 1])) ||
                                   masked[back - wl - 1] == '_'))) {
                size_t fwd = p + name.size();
                while (fwd < masked.size() && std::isspace(static_cast<unsigned char>(masked[fwd]))) ++fwd;
                if (fwd < masked.size() && masked[fwd] == '{') is_def = true;
              }
            }
            if (is_def) {
              int ln = b.line + 1;
              for (size_t k = 0; k < p && k < masked.size(); ++k) {
                if (masked[k] == '\n') ++ln;
              }
              throw BuildError("Redefinition of sync struct '" + name + "'",
                               "lang.sync で定義済みの構造体を lang.c で再定義しないでください。"
                               "メソッド本体のみを記述してください",
                               b.file, ln);
            }
          }
          p += name.size();
        }
      }
    }

    o << "#line 1 \"jelll-generated\"\n";
    // Handle registry (red-team C-02 + regression R-05): one global slot
    // table shared by every struct. A handle packs a 20-bit generation over a
    // 12-bit slot index: (gen << 12 | idx). 4096 slots, ~1M generations per
    // slot; a slot whose generation saturates is RETIRED (never reused) and
    // its cell continues under a fresh slot, so stale handles can never be
    // revived and long-running alloc/free loops cannot leak cells (R-05).
    // resolve() is the ONLY gateway from JS-visible values to pointers: it
    // checks liveness, generation AND the exact type tag (type 0 no longer
    // means "any" - R-01), so forged/stale/cross-type handles are rejected.
    o << "struct __jelll_hslot { void* cell; unsigned gen; unsigned type; unsigned next_free; unsigned pins; unsigned freeing; };\n";
    o << "static __jelll_hslot* __jelll_htab = 0;\n";
    o << "static unsigned __jelll_htab_cap = 0;\n";
    o << "static unsigned __jelll_htab_used = 0;\n";
    o << "static unsigned __jelll_hslot_free = 0xffffffffu;\n";
    o << "static unsigned __jelll_hslot_new_virgin(void) {\n";
    o << "  // R-05: only NEVER-USED slots are handed out here. Their generation\n";
    o << "  // starts at 0 exactly once (fresh storage) and is never reset later.\n";
    o << "  if (__jelll_htab_used >= 4096u) return 0xffffffffu;\n";
    o << "  if (__jelll_htab_used >= __jelll_htab_cap) {\n";
    o << "    unsigned nc = __jelll_htab_cap ? __jelll_htab_cap * 2 : 512;\n";
    o << "    if (nc > 4096u) nc = 4096u;\n";
    o << "    __jelll_hslot* nt = (__jelll_hslot*)std::realloc(__jelll_htab, (size_t)nc * sizeof(__jelll_hslot));\n";
    o << "    if (!nt) return 0xffffffffu;\n";
    o << "    for (unsigned i = __jelll_htab_cap; i < nc; ++i) {\n";
    o << "      nt[i].cell = 0; nt[i].gen = 0; nt[i].type = 0; nt[i].next_free = 0xffffffffu;\n";
    o << "      nt[i].pins = 0; nt[i].freeing = 0;\n";
    o << "    }\n";
    o << "    __jelll_htab = nt;\n";
    o << "    __jelll_htab_cap = nc;\n";
    o << "  }\n";
    o << "  unsigned i = __jelll_htab_used++;\n";
    o << "  __jelll_htab[i].cell = 0;\n";
    o << "  __jelll_htab[i].next_free = 0xffffffffu;\n";
    o << "  __jelll_htab[i].pins = 0;\n";
    o << "  __jelll_htab[i].freeing = 0;\n";
    o << "  return i;\n";
    o << "}\n";
    o << "static unsigned __jelll_hslot_new(void) {\n";
    o << "  if (__jelll_hslot_free != 0xffffffffu) {\n";
    o << "    unsigned i = __jelll_hslot_free;\n";
    o << "    __jelll_hslot_free = __jelll_htab[i].next_free;\n";
    o << "    __jelll_htab[i].cell = 0;\n";
    o << "    __jelll_htab[i].pins = 0;\n";
    o << "    __jelll_htab[i].freeing = 0;\n";
    o << "    // R-05: generation is KEPT across free-list reuse - never reset.\n";
    o << "    return i;\n";
    o << "  }\n";
    o << "  if (__jelll_htab_used >= 4096u) return 0xffffffffu;\n";
    o << "  if (__jelll_htab_used >= __jelll_htab_cap) {\n";
    o << "    unsigned nc = __jelll_htab_cap ? __jelll_htab_cap * 2 : 512;\n";
    o << "    if (nc > 4096u) nc = 4096u;\n";
    o << "    __jelll_hslot* nt = (__jelll_hslot*)std::realloc(__jelll_htab, (size_t)nc * sizeof(__jelll_hslot));\n";
    o << "    if (!nt) return 0xffffffffu;\n";
    o << "    for (unsigned i = __jelll_htab_cap; i < nc; ++i) {\n";
    o << "      nt[i].cell = 0; nt[i].gen = 0; nt[i].type = 0; nt[i].next_free = 0xffffffffu;\n";
    o << "      nt[i].pins = 0; nt[i].freeing = 0;\n";
    o << "    }\n";
    o << "    __jelll_htab = nt;\n";
    o << "    __jelll_htab_cap = nc;\n";
    o << "  }\n";
    o << "  unsigned i = __jelll_htab_used++;\n";
    o << "  __jelll_htab[i].cell = 0;\n";
    o << "  __jelll_htab[i].next_free = 0xffffffffu;\n";
    o << "  __jelll_htab[i].pins = 0;\n";
    o << "  __jelll_htab[i].freeing = 0;\n";
    o << "  return i;\n";
    o << "}\n";
    o << "static void __jelll_hslot_release(unsigned i) {\n";
    o << "  if (i >= __jelll_htab_used) return;\n";
    o << "  __jelll_hslot& s = __jelll_htab[i];\n";
    o << "  if (!s.cell && s.type == 0) return;  // already released\n";
    o << "  s.cell = 0;\n  s.type = 0;\n  s.pins = 0;\n  s.freeing = 0;\n";
    o << "  s.next_free = __jelll_hslot_free;\n";
    o << "  __jelll_hslot_free = i;\n";
    o << "}\n";
    o << "// R-05: a slot whose generation can no longer be bumped is retired\n";
    o << "// permanently - it rejoins NOTHING, so its old handles stay dead.\n";
    o << "static void __jelll_hslot_retire(unsigned i) {\n";
    o << "  if (i >= __jelll_htab_used) return;\n";
    o << "  __jelll_hslot& s = __jelll_htab[i];\n";
    o << "  s.cell = 0;\n  s.type = 0;\n  s.pins = 0;\n  s.freeing = 0;\n";
    o << "  s.next_free = 0xffffffffu;  // never on the free list again\n";
    o << "}\n";
    o << "// R-05: arm slot i for the next incarnation of a cell. The generation\n";
    o << "// is BUMPED from its kept value and NEVER initialized/reset. A slot at\n";
    o << "// the 20-bit ceiling is retired forever and the cell is re-armed on a\n";
    o << "// VIRGIN slot only (old handles can never match a new incarnation).\n";
    o << "// Returns the armed index, or 0xffffffff when no safe slot is left.\n";
    o << "static unsigned __jelll_slot_arm(unsigned i, unsigned type) {\n";
    o << "  for (;;) {\n";
    o << "    unsigned g = __jelll_htab[i].gen + 1;\n";
    o << "    if (g <= 0xfffffu) {\n";
    o << "      __jelll_htab[i].gen = g;\n";
    o << "      __jelll_htab[i].type = type;\n";
    o << "      __jelll_htab[i].pins = 0;\n";
    o << "      __jelll_htab[i].freeing = 0;\n";
    o << "      return i;\n";
    o << "    }\n";
    o << "    __jelll_hslot_retire(i);\n";
    o << "    i = __jelll_hslot_new_virgin();\n";
    o << "    if (i == 0xffffffffu) return i;\n";
    o << "  }\n";
    o << "}\n";
    o << "static void* __jelll_resolve(unsigned h, unsigned type) {\n";
    o << "  unsigned idx = h & 0xfffu;\n";
    o << "  unsigned gen = h >> 12;\n";
    o << "  if (gen == 0 || idx >= __jelll_htab_used) return 0;\n";
    o << "  __jelll_hslot* s = &__jelll_htab[idx];\n";
    o << "  if (!s->cell || s->freeing || s->gen != gen) return 0;\n";
    o << "  if (type == 0 || s->type != type) return 0;  // typed gateway only (R-01)\n";
    o << "  return s->cell;\n";
    o << "}\n";
    o << "// R-02: lifetime pinning. A method thunk pins its receiver (and any\n";
    o << "// pointer arguments) across the whole user-body call; free_h() marks\n";
    o << "// the slot as draining and destroys it only after the last pin drops,\n";
    o << "// so a concurrent free can never pull the payload from under a call.\n";
    o << "static void* __jelll_pin(unsigned h, unsigned type) {\n";
    o << "  void* p = __jelll_resolve(h, type);\n";
    o << "  if (!p) return 0;\n";
    o << "  __jelll_htab[h & 0xfffu].pins++;\n";
    o << "  return p;\n";
    o << "}\n";
    o << "static void __jelll_unpin(unsigned idx) {\n";
    o << "  if (idx < __jelll_htab_used && __jelll_htab[idx].pins) __jelll_htab[idx].pins--;\n";
    o << "}\n";
    o << "// free_h drain step: returns 1 when the slot is ours to destroy.\n";
    o << "// Looks the slot up WITHOUT __jelll_resolve so that a re-entered\n";
    o << "// drain still finds its own freeing slot.\n";
    o << "// R-02: LIVE -> DRAINING state transition, taken ONCE under the mutex.\n";
    o << "// The first free caller exclusively owns the destroy; every later free\n";
    o << "// of the same handle sees DRAINING and returns immediately with nothing.\n";
    o << "// *owned remembers OUR ownership across pin-drain retries.\n";
    o << "static int __jelll_free_begin(unsigned h, unsigned type, void** out, int* owned) {\n";
    o << "  unsigned idx = h & 0xfffu;\n";
    o << "  unsigned gen = h >> 12;\n";
    o << "  *out = 0;\n";
    o << "  if (gen == 0 || idx >= __jelll_htab_used) return 1;  // nothing to do\n";
    o << "  __jelll_hslot* s = &__jelll_htab[idx];\n";
    o << "  if (!s->cell || s->gen != gen || type == 0 || s->type != type) return 1;\n";
    o << "  if (!*owned) {\n";
    o << "    if (s->freeing) return 1;  // R-02: another free already owns the destroy\n";
    o << "    s->freeing = 1;  // claim: no new pins from here on\n";
    o << "    *owned = 1;\n";
    o << "  }\n";
    o << "  if (s->pins == 0) { *out = s->cell; return 1; }\n";
    o << "  return 0;  // in-flight calls must drain first\n";
    o << "}\n";
    for (const auto& s : structs) {
      o << "static unsigned __jelll_handle_of_" << s.name << "(const void*);\n";
    }
    std::map<std::string, int> type_ids;
    for (const auto& s : structs) type_ids[s.name] = s.type_id;
    for (const auto& s : structs) {
      o << "// ---- lang.sync: " << s.name << " ----\n";
      o << "struct " << s.name << " {\n";
      for (const auto& f : s.fields) o << "  " << f.c_type << " " << f.name << ";\n";
      o << "};\n";
    }
    if (!structs.empty()) {
      o << "extern \"C\" {\n";
      for (const auto& s : structs) {
        for (const auto& m : s.methods) {
          o << m.c_return << " " << m.c_func << "(" << s.name << "* self";
          for (size_t k = 0; k < m.param_names.size(); ++k) {
            o << ", " << m.c_params[k] << " " << m.param_names[k];
          }
          o << ");\n";
        }
      }
      o << "}\n";
    }
    for (const auto& s : structs) o << gen_struct_c_code(s, threads, type_ids);
    if (!structs.empty()) {
      o << "namespace {\n";
      for (const auto& s : structs) {
        for (const auto& m : s.methods) {
          o << "__attribute__((used)) static auto __jelll_link_" << m.c_func
            << " = &" << m.c_func << ";\n";
        }
      }
      o << "}  // namespace\n";
    }
    for (const auto* b : cblocks) {
      o << line_directive(b->file, b->line + 1);
      o << b->body << "\n";
    }
    g.core_cpp = o.str();
  }

  // ---- core.rs / core.zig ----
  {
    std::ostringstream o;
    for (const auto& b : program.blocks) {
      if (b.lang != "rs") continue;
      o << "// ---- from " << b.file << ":" << b.line << " ----\n" << b.body << "\n";
    }
    g.core_rs = o.str();
  }
  {
    std::ostringstream o;
    for (const auto& b : program.blocks) {
      if (b.lang != "zig") continue;
      o << "// ---- from " << b.file << ":" << b.line << " ----\n" << b.body << "\n";
    }
    g.core_zig = o.str();
  }

  // ---- entry module ----
  bool typed = mode != ScriptMode::Js;
  std::ostringstream e;
  e << "// Generated by JELLL V5. Do not edit.\n";
  e << "import createJelllModule from \"./native.mjs\";\n\n";
  if (typed) {
    e << "declare global {\n";
    e << "  var native: Record<string, (...args: any[]) => any>;\n";
    e << "  var sharedBuffer: ArrayBufferLike;\n";
    e << "  var sharedMemory: WebAssembly.Memory;\n";
    for (const auto& s : structs) e << gen_struct_decl(s);
    e << "}\n";
    e << "export {};\n\n";
  }
  e << R"JS(const __jelll_wasmUrl = new URL("native.wasm", import.meta.url).href;
let __jelll_memory = null;
let __jelll_exports = null;

function __jelll_err(m) {
  return new Error("[JELLL] " + m);
}

async function __jelll_boot() {
  let rejectBoot = () => {};
  const bootError = new Promise((_, reject) => { rejectBoot = reject; });
  const factory = createJelllModule({
    locateFile: (p) => new URL(p, import.meta.url).href,
    instantiateWasm(imports, receive) {
      const run = async () => {
        let result;
        if (typeof WebAssembly.instantiateStreaming === "function") {
          try {
            const resp = await fetch(__jelll_wasmUrl, { credentials: "same-origin" });
            if (!resp.ok) throw __jelll_err("native.wasm を取得できません (HTTP " + resp.status + ")");
            result = await WebAssembly.instantiateStreaming(resp, imports);
          } catch {
            const resp = await fetch(__jelll_wasmUrl, { credentials: "same-origin" });
            if (!resp.ok) throw __jelll_err("native.wasm を取得できません (HTTP " + resp.status + ")");
            result = await WebAssembly.instantiate(await resp.arrayBuffer(), imports);
          }
        } else {
          const resp = await fetch(__jelll_wasmUrl, { credentials: "same-origin" });
          if (!resp.ok) throw __jelll_err("native.wasm を取得できません (HTTP " + resp.status + ")");
          result = await WebAssembly.instantiate(await resp.arrayBuffer(), imports);
        }
        __jelll_exports = result.instance.exports;
        __jelll_memory = (imports && imports.env && imports.env.memory) ||
          result.instance.exports.memory || null;
        receive(result.instance, result.module);
      };
      run().catch((e) => rejectBoot(e));
      return {};
    },
  });
  await Promise.race([factory, bootError]);
  const memory = __jelll_memory;
  if (!memory) throw __jelll_err("WASM メモリーを取得できませんでした");
  globalThis.sharedMemory = memory;
  // sharedBuffer always reflects the *current* memory buffer (it is replaced
  // when the heap grows).
  Object.defineProperty(globalThis, "sharedBuffer", {
    get: () => memory.buffer,
    configurable: true,
  });
  const nativeObject = {};
  const exportsObject = __jelll_exports || {};
  for (const k of Object.keys(exportsObject)) {
    if (typeof exportsObject[k] === "function") nativeObject[k] = exportsObject[k];
  }
  for (const k of Object.keys(nativeObject)) {
    if (k[0] !== "_" && !Object.prototype.hasOwnProperty.call(nativeObject, "_" + k)) {
      nativeObject["_" + k] = nativeObject[k];
    }
  }
  globalThis.native = nativeObject;
}

)JS";
  for (const auto& s : structs) e << gen_struct_ts(s, mode);
  e << "\nawait __jelll_boot();\n\n// ---- user script blocks (occurrence order) ----\n";
  for (const auto& b : program.blocks) {
    if (b.lang != "js" && b.lang != "ts" && b.lang != "tsx") continue;
    e << "// ---- from " << b.file << ":" << b.line << " (" << b.lang << ") ----\n" << b.body << "\n";
  }
  g.entry = e.str();
  g.entry_name = mode == ScriptMode::Tsx ? "entry.tsx" : (mode == ScriptMode::Ts ? "entry.ts" : "entry.js");

  // ---- index.html ----
  std::ostringstream h;
  h << "<!doctype html>\n<html lang=\"ja\">\n<head>\n<meta charset=\"utf-8\">\n";
  h << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
  h << "<title>JELLL App</title>\n<style>\n";
  for (const auto& b : program.blocks) {
    if (b.lang == "css") h << b.body << "\n";
  }
  h << "</style>\n</head>\n<body>\n";
  for (const auto& b : program.blocks) {
    if (b.lang == "html") h << b.body << "\n";
  }
  h << "<script type=\"module\" src=\"bundle.js\"></script>\n";
  if (dev_mode) {
    h << R"HTML(<script>
(() => {
  let id = null;
  setInterval(async () => {
    try {
      const r = await fetch("/__jelll__/status", { cache: "no-store" });
      const j = await r.json();
      if (id === null) { id = j.id; return; }
      if (j.id !== id && !j.error) location.reload();
    } catch {}
  }, 500);
})();
</script>
)HTML";
  }
  h << "</body>\n</html>\n";
  g.html = h.str();
  return g;
}

}  // namespace jelll
