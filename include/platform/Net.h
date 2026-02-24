#pragma once

#include <cstdint>
#include <string>

namespace platform::net {

bool isConnected();
int rssi();
bool getSsid(std::string& out);
bool getLocalIp(std::string& out);
bool resolveHostByName(const char* host, std::string& outIp);

}  // namespace platform::net
