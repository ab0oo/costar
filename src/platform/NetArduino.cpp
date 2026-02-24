#include "platform/Net.h"

#include <WiFi.h>

namespace platform::net {

bool isConnected() { return WiFi.status() == WL_CONNECTED; }

int rssi() { return WiFi.RSSI(); }

bool getSsid(std::string& out) {
  const String ssid = WiFi.SSID();
  if (ssid.isEmpty()) {
    out.clear();
    return false;
  }
  out = ssid.c_str();
  return true;
}

bool getLocalIp(std::string& out) {
  if (WiFi.status() != WL_CONNECTED) {
    out.clear();
    return false;
  }
  out = WiFi.localIP().toString().c_str();
  return !out.empty();
}

bool resolveHostByName(const char* host, std::string& outIp) {
  if (host == nullptr || *host == '\0') {
    outIp.clear();
    return false;
  }
  IPAddress resolved;
  if (!WiFi.hostByName(host, resolved)) {
    outIp.clear();
    return false;
  }
  outIp = resolved.toString().c_str();
  return !outIp.empty();
}

}  // namespace platform::net
