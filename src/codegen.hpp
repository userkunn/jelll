#pragma once
// Code generation: native sources, entry glue, lang.sync (IDL), index.html.
#include <string>
#include <vector>

#include "scanner.hpp"

namespace jelll {

struct FieldInfo {
  std::string c_type;    // normalized C type text
  std::string name;
  std::string ts_type;   // "number" | "bigint"
  std::string get_ret;   // C return type of the generated getter
  std::string set_param; // C parameter type of the generated setter
  bool is_ptr = false;
  bool is_i64 = false;
  bool is_float = false;
  std::string ptr_base;        // pointer base type name ("" when not a pointer)
  bool ptr_base_struct = false;// base is a lang.sync struct (typed handle check)
};

struct MethodInfo {
  std::string name;                   // method name without struct prefix
  std::string ts_return;              // "number" | "bigint" | "void"
  std::string c_return;               // C return type text
  std::vector<std::string> ts_params; // "number" | "bigint"
  std::vector<std::string> c_params;  // C parameter type texts
  std::vector<std::string> param_names;
  std::string c_func;                 // e.g. Vector2_length (user body symbol)
  bool ret_is_ptr = false;
  std::string ret_ptr_base;
  bool ret_ptr_base_struct = false;
  std::vector<bool> param_is_ptr;
  std::vector<std::string> param_ptr_base;
  std::vector<bool> param_ptr_base_struct;
};

struct SyncStruct {
  std::string name;
  std::vector<FieldInfo> fields;
  std::vector<MethodInfo> methods;
  std::string file;
  int line = 0;
  int type_id = 0;   // global handle-table type tag (assigned in parse order)
};

struct Generated {
  std::string core_cpp;      // user C/C++ + #line + generated accessors
  std::string core_rs;       // user Rust (verbatim, occurrence order)
  std::string core_zig;      // user Zig (verbatim, occurrence order)
  std::string entry;         // TS/JS entry module source
  std::string entry_name;    // entry.tsx / entry.ts / entry.js
  std::string html;          // dist/index.html
};

struct ScriptMode { enum E { Js, Ts, Tsx }; };

// Parse lang.sync IDL blocks into struct/field/method descriptors. This is
// the ONLY source of sync metadata - C++ code is never scraped.
std::vector<SyncStruct> parse_sync(const std::vector<Block>& sync_blocks);

// Assemble all generated artifacts.
Generated generate(const Program& program, const std::vector<SyncStruct>& structs,
                   ScriptMode::E mode, bool dev_mode, bool threads = false);

}  // namespace jelll
