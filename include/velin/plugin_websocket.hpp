// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nlohmann/json.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace velin {
using Json = nlohmann::json;
struct SocketEvent { std::string event; Json args; };

// RFC 6455 server. All callbacks are returned by poll() on the caller's thread.
// The caller must enforce plugin network permissions before accepting a host
// other than loopback. TLS termination, if needed, belongs to an external proxy.
class PluginWebSocketServer final {
 public:
  PluginWebSocketServer();
  ~PluginWebSocketServer();
  PluginWebSocketServer(const PluginWebSocketServer&)=delete;
  PluginWebSocketServer& operator=(const PluginWebSocketServer&)=delete;
  // options.port is required (0 requests an ephemeral port). host defaults to
  // 127.0.0.1; explicit numeric IPv4/IPv6 hosts are supported. Optional keys:
  // path, allowedOrigins, protocols, maxMessageBytes, maximumConnections.
  void create(const Json& options);
  void start();
  void stop();
  void sendText(std::string connection,std::string text,std::string token);
  std::vector<SocketEvent> poll();
  [[nodiscard]] std::uint16_t port() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace velin
