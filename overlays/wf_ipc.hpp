#pragma once

// wf_ipc.hpp - wayfire's IPC transport, header-only: the length-prefixed JSON
// framing, a persistent request/response channel, and an event-stream socket
// subscribed to every event. The wire format is a u32 little-endian byte count
// followed by that many bytes of JSON, both directions.
//
// This exists so a second module speaking to wayfire does not carry a second
// copy of the framing. NOTE: overlays/taskbar.cpp still has the older inline
// copy of these same primitives (it predates this header) and should migrate
// to it, as its own commit -- mixing that refactor into a feature would put two
// unrelated changes behind one message. Until it does, taskbar.cpp is the one
// place the duplication still lives; do not add a third.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <json/json.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace waybar::wf {

// The socket wayfire's ipc plugin listens on: an explicit config key wins, then
// the environment the compositor exports, then the default name. The default is
// a last resort for a session that lost the variable, not the normal path.
inline std::string resolve_socket(const Json::Value& cfg) {
  if (cfg.isObject() && cfg.isMember("socket") && cfg["socket"].isString()) {
    return cfg["socket"].asString();
  }
  if (const char* env = std::getenv("WAYFIRE_SOCKET")) return env;
  return "/run/user/" + std::to_string(getuid()) +
         "/wayfire-wayland-1-.socket";
}

inline int connect_socket(const std::string& path) {
  int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket() failed");
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    throw std::runtime_error("socket path too long");
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    std::ostringstream ss;
    ss << "connect() failed: " << std::strerror(errno);
    throw std::runtime_error(ss.str());
  }
  return fd;
}

inline void write_all(int fd, const char* p, size_t n) {
  size_t off = 0;
  while (off < n) {
    // MSG_NOSIGNAL: a compositor that went away must surface as a write error
    // to be reconnected, never as SIGPIPE killing the whole bar.
    ssize_t w = ::send(fd, p + off, n - off, MSG_NOSIGNAL);
    if (w <= 0) throw std::runtime_error("socket write failed");
    off += static_cast<size_t>(w);
  }
}

inline std::string read_exact(int fd, size_t n) {
  std::string out;
  out.resize(n);
  size_t off = 0;
  while (off < n) {
    ssize_t r = ::read(fd, out.data() + off, n - off);
    if (r <= 0) throw std::runtime_error("socket closed");
    off += static_cast<size_t>(r);
  }
  return out;
}

inline void write_msg(int fd, const std::string& method,
                      const Json::Value& data) {
  Json::Value msg;
  msg["method"] = method;
  msg["data"] = data.isObject() ? data : Json::Value(Json::objectValue);
  Json::StreamWriterBuilder wb;
  wb["indentation"] = "";
  const std::string payload = Json::writeString(wb, msg);
  const uint32_t n = static_cast<uint32_t>(payload.size());
  const uint8_t hdr[4] = {static_cast<uint8_t>(n & 0xff),
                          static_cast<uint8_t>((n >> 8) & 0xff),
                          static_cast<uint8_t>((n >> 16) & 0xff),
                          static_cast<uint8_t>((n >> 24) & 0xff)};
  write_all(fd, reinterpret_cast<const char*>(hdr), 4);
  write_all(fd, payload.data(), payload.size());
}

// Read one framed message and parse it. Does NOT inspect it for an "error"
// member: an event stream carries no errors, and only the caller knows whether
// one is fatal.
inline Json::Value read_msg(int fd) {
  const std::string hdr = read_exact(fd, 4);
  const uint32_t len = static_cast<uint8_t>(hdr[0]) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(hdr[1]))
                        << 8) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(hdr[2]))
                        << 16) |
                       (static_cast<uint32_t>(static_cast<uint8_t>(hdr[3]))
                        << 24);
  const std::string body = read_exact(fd, len);
  Json::CharReaderBuilder rb;
  Json::Value root;
  std::string errs;
  std::istringstream iss(body);
  if (!Json::parseFromStream(rb, iss, &root, &errs)) {
    throw std::runtime_error("invalid json response: " + errs);
  }
  return root;
}

// Open an event-stream connection subscribed to EVERY event.
//
// Subscribing to everything rather than a curated list is deliberate: wayfire's
// watch rejects the whole request if any named event is unknown ("Event not
// found"), which makes a named list a version landmine across compositor
// upgrades. A bare subscribe cannot be rejected; the caller filters.
inline int open_event_stream(const std::string& path) {
  int fd = connect_socket(path);
  try {
    write_msg(fd, "window-rules/events/watch", Json::Value(Json::objectValue));
  } catch (...) {
    ::close(fd);
    throw;
  }
  return fd;
}

// A persistent request/response channel. One connection is opened on first use
// and reused; a transport fault costs one transparent reconnect rather than a
// lost call. An application-level {"error": ...} THROWS without re-sending, so
// a mutating call can never be silently repeated.
class Client {
 public:
  explicit Client(std::string path) : path_(std::move(path)) {}
  ~Client() { close(); }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  Json::Value call(const std::string& method, const Json::Value& data) {
    Json::Value root;
    bool got = false;
    for (int attempt = 0; attempt < 2 && !got; ++attempt) {
      try {
        if (fd_ < 0) fd_ = connect_socket(path_);
        write_msg(fd_, method, data);
        root = read_msg(fd_);
        got = true;
      } catch (const std::runtime_error&) {
        close();
        if (attempt == 1) throw;
      }
    }
    if (root.isObject() && root.isMember("error")) {
      throw std::runtime_error(root["error"].asString());
    }
    return root;
  }

 private:
  std::string path_;
  int fd_ = -1;
};

// Tolerant integer read: wayfire sends numbers in whichever JSON int type fits.
inline int64_t j_i64(const Json::Value& obj, const char* key, int64_t def) {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  if (v.isInt64()) return v.asInt64();
  if (v.isUInt64()) return static_cast<int64_t>(v.asUInt64());
  if (v.isNumeric()) return static_cast<int64_t>(v.asDouble());
  return def;
}

inline std::string j_str(const Json::Value& obj, const char* key,
                         const std::string& def = "") {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  return v.isString() ? v.asString() : def;
}

// Accepts an int as well as a bool: some wayfire fields are emitted as 0/1.
inline bool j_bool(const Json::Value& obj, const char* key, bool def) {
  if (!obj.isObject() || !obj.isMember(key)) return def;
  const auto& v = obj[key];
  if (v.isBool()) return v.asBool();
  if (v.isInt()) return v.asInt() != 0;
  return def;
}

}  // namespace waybar::wf
