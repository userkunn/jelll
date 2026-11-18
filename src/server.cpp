#include "server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "util.hpp"

namespace jelll {

namespace fs = std::filesystem;

namespace {

std::string content_type(const std::string& path) {
  auto dot = path.find_last_of('.');
  std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
  if (ext == "html") return "text/html; charset=utf-8";
  if (ext == "js" || ext == "mjs") return "text/javascript; charset=utf-8";
  if (ext == "css") return "text/css; charset=utf-8";
  if (ext == "json") return "application/json; charset=utf-8";
  if (ext == "wasm") return "application/wasm";
  if (ext == "map") return "application/json; charset=utf-8";
  if (ext == "svg") return "image/svg+xml";
  if (ext == "png") return "image/png";
  if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
  if (ext == "txt") return "text/plain; charset=utf-8";
  return "application/octet-stream";
}

void send_all(int fd, const std::string& data) {
  size_t sent = 0;
  while (sent < data.size()) {
    ssize_t n = ::write(fd, data.data() + sent, data.size() - sent);
    if (n <= 0) return;
    sent += static_cast<size_t>(n);
  }
}

void send_response(int fd, int status, const std::string& reason, const std::string& type,
                   const std::string& body, bool isolated) {
  std::ostringstream h;
  h << "HTTP/1.1 " << status << " " << reason << "\r\n"
    << "Content-Type: " << type << "\r\n"
    << "Content-Length: " << body.size() << "\r\n";
  if (isolated) {
    h << "Cross-Origin-Opener-Policy: same-origin\r\n"
      << "Cross-Origin-Embedder-Policy: require-corp\r\n"
      << "Cross-Origin-Resource-Policy: same-origin\r\n";
  }
  h << "Cache-Control: no-store\r\n"
    << "X-Content-Type-Options: nosniff\r\n"
    << "Connection: close\r\n\r\n";
  send_all(fd, h.str());
  send_all(fd, body);
}

// M-05: RAII socket ownership - a single close point that cannot leak even
// when an exception unwinds the handler.
struct FdGuard {
  int fd;
  explicit FdGuard(int f) : fd(f) {}
  ~FdGuard() { if (fd >= 0) ::close(fd); }
  FdGuard(const FdGuard&) = delete;
  FdGuard& operator=(const FdGuard&) = delete;
};

// S-05 (DNS rebinding): only loopback Host names are served.
bool host_ok(const std::string& request) {
  size_t h = request.find("\r\nHost: ");
  size_t val = std::string::npos;
  if (request.rfind("Host: ", 0) == 0) val = 6;
  else if (h != std::string::npos) val = h + 8;
  if (val == std::string::npos) return false;
  size_t e = request.find("\r\n", val);
  std::string host = request.substr(val, e == std::string::npos ? std::string::npos : e - val);
  if (auto c = host.find(':'); c != std::string::npos) host.resize(c);
  return host == "127.0.0.1" || host == "localhost" || host == "[::1]";
}

void handle_client(int fd, const std::string& dist_dir, ServerStatus& status, bool isolated) {
  FdGuard guard(fd);
  char buf[8192];
  size_t total = 0;
  std::string request;
  while (total < sizeof buf) {
    ssize_t n = ::read(fd, buf + total, sizeof buf - total);
    if (n <= 0) break;
    // R-04: TCP may split the request across reads - append exactly the new
    // bytes (the old code re-appended buf[0..n) and corrupted split requests).
    request.append(buf + total, static_cast<size_t>(n));
    total += static_cast<size_t>(n);
    if (request.find("\r\n\r\n") != std::string::npos) break;
  }
  if (request.find("\r\n\r\n") == std::string::npos) {
    // Header never completed (stalled or truncated client). SO_RCVTIMEO caps
    // the wait; reject promptly rather than holding the socket (Slowloris).
    if (!request.empty())
      send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "incomplete request", isolated);
    return;
  }
  if (request.rfind("GET ", 0) != 0) {
    send_response(fd, 405, "Method Not Allowed", "text/plain; charset=utf-8", "method not allowed", isolated);
    return;
  }
  if (!host_ok(request)) {
    send_response(fd, 421, "Misdirected Request", "text/plain; charset=utf-8",
                  "invalid Host header", isolated);
    return;
  }
  size_t sp = request.find(' ');
  size_t sp2 = request.find(' ', sp + 1);
  std::string raw_path = request.substr(sp + 1, sp2 - sp - 1);
  size_t q = raw_path.find('?');
  if (q != std::string::npos) raw_path = raw_path.substr(0, q);
  std::string path = url_decode(raw_path);

  if (path == "/__jelll__/status") {
    auto [sid, serr] = status.snapshot();
    std::ostringstream body;
    body << "{\"id\":" << sid << ",\"error\":\"" << json_escape(serr) << "\"}";
    send_response(fd, 200, "OK", "application/json; charset=utf-8", body.str(), isolated);
    return;
  }

  // Sanitize: reject traversal, absolute escapes and NUL bytes.
  if (path.find("..") != std::string::npos || path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos) {
    send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "bad path", isolated);
    return;
  }
  if (path.empty() || path == "/") path = "/index.html";
  // Strip ALL leading slashes: URLs like "//etc/passwd" would otherwise make
  // path.substr(1) absolute and fs::path::operator/ would REPLACE the base
  // (local file inclusion). Then verify the canonical result stays inside
  // dist_dir, which also catches symlink escapes.
  size_t first = path.find_first_not_of('/');
  std::string rel = (first == std::string::npos) ? "index.html" : path.substr(first);
  if (rel.empty()) rel = "index.html";
  std::error_code ec;
  fs::path full = fs::weakly_canonical(fs::path(dist_dir) / rel, ec);
  fs::path base = fs::weakly_canonical(dist_dir, ec);
  if (ec || (full != base && full.string().rfind(base.string() + "/", 0) != 0)) {
    send_response(fd, 400, "Bad Request", "text/plain; charset=utf-8", "bad path", isolated);
    return;
  }
  if (!fs::is_regular_file(full, ec)) {
    send_response(fd, 404, "Not Found", "text/plain; charset=utf-8", "not found", isolated);
    return;
  }
  auto content = read_file(full.string());
  if (!content) {
    send_response(fd, 500, "Internal Server Error", "text/plain; charset=utf-8", "read error", isolated);
    return;
  }
  send_response(fd, 200, "OK", content_type(full.string()), *content, isolated);
}

std::atomic<bool> g_stop{false};
std::atomic<int> g_listen_fd{-1};
// L-05: in-flight handler count so stop_dev_server() can actually drain.
std::atomic<int> g_in_flight{0};
std::mutex g_drain_mu;
std::condition_variable g_drain_cv;

}  // namespace

void stop_dev_server() {
  g_stop.store(true);
  int fd = g_listen_fd.exchange(-1);
  if (fd >= 0) {
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
  }
  // L-05: let in-flight handlers drain before the caller may destroy the
  // shared ServerStatus (bounded by the 2s/5s socket timeouts).
  std::unique_lock<std::mutex> lk(g_drain_mu);
  g_drain_cv.wait_for(lk, std::chrono::seconds(7),
                      [] { return g_in_flight.load() == 0; });
}

bool start_dev_server(const std::string& dist_dir, int port, ServerStatus& status, bool isolated) {
  g_stop.store(false);
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int reuse = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    ::close(fd);
    return false;
  }
  if (::listen(fd, 16) != 0) {
    ::close(fd);
    return false;
  }
  // Bounded concurrency: up to 8 clients are served in parallel. Beyond that
  // the accept loop pauses and the kernel backlog (16) absorbs bursts, so a
  // stalled or slow client can never wedge the server (Slowloris), while the
  // thread count stays capped (no unbounded thread spawning).
  struct Pool {
    std::mutex mu;
    std::condition_variable cv;
    int in_flight = 0;
  };
  auto pool = std::make_shared<Pool>();
  g_listen_fd.store(fd);
  std::thread([fd, dist_dir, &status, isolated, pool]() {
    while (!g_stop.load()) {
      // Reserve a slot BEFORE accept(): overflow connections wait in the
      // kernel accept queue (backlog), never as accepted-but-unhandled
      // sockets whose read timeouts have not started yet. The reservation
      // is held across accept() so the cap of 8 stays exact.
      {
        std::unique_lock<std::mutex> lk(pool->mu);
        pool->cv.wait(lk, [&] { return pool->in_flight < 8; });
        ++pool->in_flight;
      }
      int client = ::accept(fd, nullptr, nullptr);
      if (client < 0) {
        // Release the reservation. Transient errors keep looping; a stopped
        // or broken listening socket ends the thread (no busy spin).
        {
          std::lock_guard<std::mutex> lk(pool->mu);
          --pool->in_flight;
        }
        pool->cv.notify_one();
        if (g_stop.load()) break;
        if (errno == EINTR || errno == EMFILE || errno == ENOBUFS ||
            errno == ECONNABORTED) {
          continue;
        }
        break;
      }
      // Bound stalled clients: headers must arrive within 2s, writes within 5s.
      struct timeval rcv = {2, 0}, snd = {5, 0};
      ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof rcv);
      ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof snd);
      // M-05: never let an escaping exception kill the whole dev server, and
      // never leak the client slot/fd if thread creation itself fails.
      try {
        std::thread([dist_dir, &status, isolated, client, pool]() {
          g_in_flight.fetch_add(1);
          try {
            handle_client(client, dist_dir, status, isolated);
          } catch (...) {
            // keep the server alive; the socket is closed by FdGuard
          }
          g_in_flight.fetch_sub(1);
          {
            std::lock_guard<std::mutex> lk(pool->mu);
            --pool->in_flight;
          }
          pool->cv.notify_one();
          g_drain_cv.notify_all();
        }).detach();
      } catch (const std::system_error&) {
        ::close(client);
        {
          std::lock_guard<std::mutex> lk(pool->mu);
          --pool->in_flight;
        }
        pool->cv.notify_one();
        continue;
      }
    }
  }).detach();
  return true;
}

}  // namespace jelll
