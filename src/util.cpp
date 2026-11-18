#include "util.hpp"

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sys/stat.h>
#include <fstream>

namespace jelll {

namespace fs = std::filesystem;

CommandResult run_command(const std::vector<std::string>& argv,
                          const std::string& log_file,
                          const std::map<std::string, std::string>& env_extra,
                          bool inherit_stdio) {
  CommandResult result;
  if (argv.empty() || argv[0].empty()) {
    result.error = "empty command";
    return result;
  }
  std::vector<std::string> env_kv;
  for (char** e = environ; e && *e; ++e) env_kv.emplace_back(*e);
  for (const auto& [k, v] : env_extra) {
    // Replace an existing variable if present.
    bool replaced = false;
    for (auto& kv : env_kv) {
      if (kv.compare(0, k.size() + 1, k + "=") == 0) {
        kv = k + "=" + v;
        replaced = true;
        break;
      }
    }
    if (!replaced) env_kv.push_back(k + "=" + v);
  }
  std::vector<char*> envp;
  envp.reserve(env_kv.size() + 1);
  for (auto& kv : env_kv) envp.push_back(kv.data());
  envp.push_back(nullptr);

  // M-06: execve needs a concrete path (no PATH lookup), so resolve the
  // program to an absolute path up front.
  std::string exe = argv[0];
  {
    std::error_code ec;
    fs::path ap = fs::absolute(exe, ec);
    if (!ec) exe = ap.string();
  }
  std::vector<std::string> argv_copy = argv;
  argv_copy[0] = exe;
  std::vector<char*> cargv;
  cargv.reserve(argv_copy.size() + 1);
  for (const auto& a : argv_copy) cargv.push_back(const_cast<char*>(a.c_str()));
  cargv.push_back(nullptr);

  int log_fd = -1;
  if (!inherit_stdio && !log_file.empty()) {
    log_fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (log_fd < 0) {
      result.error = "cannot open log file: " + log_file;
      return result;
    }
  }

  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) {
    result.error = std::string("fork failed: ") + std::strerror(errno);
    if (log_fd >= 0) ::close(log_fd);
    return result;
  }
  if (pid == 0) {
    // M-06: the child only calls async-signal-safe syscalls (no stdio locks).
    if (log_fd >= 0) {
      if (::dup2(log_fd, 1) < 0 || ::dup2(log_fd, 2) < 0) ::_exit(127);
      if (log_fd > 2) ::close(log_fd);
    }
    // Do not leak unrelated descriptors (dev-server sockets etc.) into the
    // toolchain processes.
    for (int f = 3; f < 1024; ++f) {
      if (f != log_fd) ::close(f);
    }
    ::execve(exe.c_str(), cargv.data(), envp.data());
    {
      // R-07: after fork() in a threaded process only async-signal-safe calls
      // are legal - snprintf/strerror can block on inherited libc locks. A
      // fixed message is enough; the parent maps exit 127 to a full error.
      static const char kExecFail[] = "jelll: execve failed\n";
      ssize_t ignored = ::write(2, kExecFail, sizeof kExecFail - 1);
      (void)ignored;
    }
    ::_exit(127);
  }
  if (log_fd >= 0) ::close(log_fd);
  int status = 0;
  while (true) {
    pid_t r = ::waitpid(pid, &status, 0);
    if (r == pid) break;
    if (r < 0 && errno == EINTR) continue;
    result.error = "waitpid failed";
    return result;
  }
  result.started = true;
  if (WIFEXITED(status)) {
    result.exit_code = WEXITSTATUS(status);
    if (result.exit_code == 127) {
      result.started = false;
      result.error = "exec failed: " + argv[0];
    }
  } else if (WIFSIGNALED(status)) {
    result.exit_code = 128 + WTERMSIG(status);
    result.error = "terminated by signal " + std::to_string(WTERMSIG(status));
  }
  return result;
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return content;
}

namespace {

// S-07: never follow a final-component symlink when writing; write(2) loop
// with EINTR handling.
bool write_fd_all(int fd, const std::string& content) {
  const char* data = content.data();
  size_t left = content.size();
  while (left > 0) {
    ssize_t n = ::write(fd, data, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

}  // namespace

bool write_file(const std::string& path, const std::string& content) {
  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = write_fd_all(fd, content);
  ::close(fd);
  return ok;
}

bool write_file_atomic(const std::string& path, const std::string& content) {
  std::error_code ec;
  fs::path p(path);
  if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
  std::string tmp = path + ".jelll-tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = write_fd_all(fd, content);
  // S-07: fsync the content, then rename (atomic replace), then best-effort
  // fsync the directory so the rename itself is durable.
  if (ok && ::fsync(fd) != 0) ok = false;
  ::close(fd);
  if (!ok) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  if (p.has_parent_path()) {
    int dfd = ::open(p.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
      ::fsync(dfd);
      ::close(dfd);
    }
  }
  return true;
}

bool safe_exec_owner(const std::string& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode)) return false;
  uid_t me = ::getuid();
  if (st.st_uid != 0 && st.st_uid != me) return false;
  if (st.st_mode & (S_IWGRP | S_IWOTH)) return false;
  return true;
}

std::optional<std::string> find_in_path(const std::string& program) {
  if (program.empty()) return std::nullopt;
  if (program.find('/') != std::string::npos) {
    if (::access(program.c_str(), X_OK) == 0 && safe_exec_owner(program)) return program;
    return std::nullopt;
  }
  const char* path_env = std::getenv("PATH");
  if (!path_env) return std::nullopt;
  std::string paths(path_env);
  size_t start = 0;
  while (start <= paths.size()) {
    size_t end = paths.find(':', start);
    std::string dir = paths.substr(start, end == std::string::npos ? std::string::npos : end - start);
    if (!dir.empty()) {
      std::string candidate = dir + "/" + program;
      if (::access(candidate.c_str(), X_OK) == 0 && safe_exec_owner(candidate)) return candidate;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::nullopt;
}

std::string path_basename(const std::string& path) {
  size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

std::string path_dirname(const std::string& path) {
  size_t pos = path.find_last_of('/');
  if (pos == std::string::npos) return ".";
  if (pos == 0) return "/";
  return path.substr(0, pos);
}

std::string json_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

std::string html_escape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c;
    }
  }
  return out;
}

std::string url_decode(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
        std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
      i += 2;
    } else if (s[i] == '+') {
      out.push_back(' ');
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

std::string c_string_escape(const std::string& s) {
  // L-02: control characters must not survive into generated literals (a
  // newline in a filename would otherwise split the #line directive).
  std::string out;
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\0': out += "\\0"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof buf, "\\x%02x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out.push_back(c);
        }
    }
  }
  return out;
}

std::string sha256_hex(const std::string& path) {
  auto exe = find_in_path("sha256sum");
  if (!exe) throw BuildError("sha256sum not found in PATH", "coreutils をインストールしてください");
  std::string log = path + ".sha256.tmp";
  auto r = run_command({*exe, path}, log);
  auto content = read_file(log);
  std::error_code ec;
  fs::remove(log, ec);
  if (!r.started || r.exit_code != 0 || !content) {
    throw BuildError("sha256sum failed", "ファイルを確認してください: " + path);
  }
  return content->substr(0, 64);
}

}  // namespace jelll
