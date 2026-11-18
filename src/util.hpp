#pragma once
// JELLL V5 Linux rewrite - common utilities.
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace jelll {

// Diagnostic error carrying the exact manual error string plus context.
struct BuildError : std::runtime_error {
  std::string hint;
  std::string file;
  int line = 0;
  explicit BuildError(std::string message, std::string hint_ = "", std::string file_ = "", int line_ = 0)
      : std::runtime_error(std::move(message)), hint(std::move(hint_)), file(std::move(file_)), line(line_) {}
};

struct CommandResult {
  bool started = false;   // false: the executable could not be started
  int exit_code = -1;     // exit status when started and exited normally
  std::string error;      // spawn failure reason
};

// Run argv[0] (must be an absolute path) with no shell. stdout/stderr are
// appended to log_file (created/truncated) unless inherit_stdio is true.
// env_extra entries are merged over the current environment.
CommandResult run_command(const std::vector<std::string>& argv,
                          const std::string& log_file,
                          const std::map<std::string, std::string>& env_extra = {},
                          bool inherit_stdio = false);

std::optional<std::string> read_file(const std::string& path);
bool write_file(const std::string& path, const std::string& content);
// Write to a temp file then rename() over the destination.
bool write_file_atomic(const std::string& path, const std::string& content);

std::optional<std::string> find_in_path(const std::string& program);

// S-03: an executable is only trusted when its owner is root or the current
// user and it is not group/other-writable.
bool safe_exec_owner(const std::string& path);
std::string path_basename(const std::string& path);
std::string path_dirname(const std::string& path);

std::string json_escape(const std::string& s);
std::string html_escape(const std::string& s);
std::string url_decode(const std::string& s);
std::string c_string_escape(const std::string& s);  // for #line file names

std::string sha256_hex(const std::string& path);  // via sha256sum(1), throws BuildError

}  // namespace jelll
