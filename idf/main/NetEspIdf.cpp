#include "platform/Net.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>

#include "esp_netif.h"
#include "esp_wifi.h"

namespace platform::net {

bool isConnected() {
  wifi_ap_record_t apInfo = {};
  return esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK;
}

int rssi() {
  wifi_ap_record_t apInfo = {};
  if (esp_wifi_sta_get_ap_info(&apInfo) == ESP_OK) {
    return static_cast<int>(apInfo.rssi);
  }
  return 0;
}

bool getSsid(std::string& out) {
  wifi_ap_record_t apInfo = {};
  if (esp_wifi_sta_get_ap_info(&apInfo) != ESP_OK) {
    out.clear();
    return false;
  }
  out.assign(reinterpret_cast<const char*>(apInfo.ssid));
  return !out.empty();
}

bool getLocalIp(std::string& out) {
  esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif == nullptr) {
    out.clear();
    return false;
  }
  esp_netif_ip_info_t ipInfo = {};
  if (esp_netif_get_ip_info(netif, &ipInfo) != ESP_OK) {
    out.clear();
    return false;
  }
  char buf[16] = {0};
  if (inet_ntop(AF_INET, &ipInfo.ip.addr, buf, sizeof(buf)) == nullptr) {
    out.clear();
    return false;
  }
  out.assign(buf);
  return true;
}

bool resolveHostByName(const char* host, std::string& outIp) {
  if (host == nullptr || *host == '\0') {
    outIp.clear();
    return false;
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  addrinfo* results = nullptr;
  const int rc = getaddrinfo(host, nullptr, &hints, &results);
  if (rc != 0 || results == nullptr) {
    outIp.clear();
    return false;
  }
  const sockaddr_in* addr = reinterpret_cast<const sockaddr_in*>(results->ai_addr);
  char buf[16] = {0};
  const char* ok = inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
  freeaddrinfo(results);
  if (ok == nullptr) {
    outIp.clear();
    return false;
  }
  outIp.assign(buf);
  return true;
}

}  // namespace platform::net
