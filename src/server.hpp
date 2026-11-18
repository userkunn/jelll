#pragma once
// Static dev server with COOP/COEP and live-reload status endpoint.
// Handlers run in a bounded thread pool (8 concurrent clients); stalled or
// slow clients cannot wedge the accept loop.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>

namespace jelll {

// Build status shared between the watcher thread and request handlers.
// All accessors are internally synchronized (handlers are multi-threaded).
struct ServerStatus {
  void set_ok(uint64_t new_id) {
    std::lock_guard<std::mutex> lk(mu_);
    id_ = new_id;
    error_.clear();
  }
  void set_error(std::string msg) {
    std::lock_guard<std::mutex> lk(mu_);
    error_ = std::move(msg);
  }
  std::pair<uint64_t, std::string> snapshot() {
    std::lock_guard<std::mutex> lk(mu_);
    return {id_, error_};
  }

 private:
  std::mutex mu_;
  uint64_t id_ = 0;    // plain state; the mutex is the single synchronizer
  std::string error_;  // last build error ("" = ok)
};

// Start serving dist_dir on 127.0.0.1:port in a background thread.
// Returns false if the port cannot be bound. isolated=true adds the
// COOP/COEP headers required for SharedArrayBuffer (--threads builds).
bool start_dev_server(const std::string& dist_dir, int port, ServerStatus& status,
                      bool isolated = false);

// Stop a server started by start_dev_server: closes the listening socket,
// wakes the accept loop and lets in-flight handlers drain (bounded by the
// 2s/5s socket timeouts). Safe to call when no server is running.
void stop_dev_server();

}  // namespace jelll
