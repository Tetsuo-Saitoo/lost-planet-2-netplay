#pragma once

#include <cstdint>
#include <string>

namespace lp2net {
// Ask the router (Windows UPnP NAT API) to forward this UDP port to us. Runs on a background thread;
// repeated calls for the same port are ignored. onPublicIp fires once if the router reports its WAN address.
void UpnpMapUdp(uint16_t port);
void UpnpSetPublicIpCallback(void (*cb)(const std::string& ip));
void UpnpUnmapAll();
std::string UpnpStatus();
}  // namespace lp2net
