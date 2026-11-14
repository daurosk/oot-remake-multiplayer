#include "local_net.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <fstream>
#include <sstream>
#include <string>
#endif

#include <algorithm>

namespace coop::server::nat {

std::uint32_t mask_of(unsigned prefix_length) {
  if (prefix_length == 0) return 0;
  if (prefix_length >= 32) return 0xFFFFFFFFu;
  return 0xFFFFFFFFu << (32 - prefix_length);
}

bool gateway_on_link(const LocalNet& net) {
  return net.gateway != 0 && net.gateway != net.ip && (net.gateway & net.mask) == (net.ip & net.mask);
}

std::uint32_t broadcast_of(const LocalNet& net) {
  if (net.ip == 0 || (~net.mask) < 3u) return 0;  // /31, /32: no broadcast address
  return net.ip | ~net.mask;
}

#ifdef _WIN32

std::vector<LocalNet> local_networks() {
  std::vector<LocalNet> out;
  ULONG size = 16 * 1024;
  std::vector<unsigned char> buffer;
  ULONG result = ERROR_BUFFER_OVERFLOW;
  for (int tries = 0; tries < 3 && result == ERROR_BUFFER_OVERFLOW; ++tries) {
    buffer.resize(size);
    result = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_INCLUDE_GATEWAYS | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
  }
  if (result != NO_ERROR) return out;
  DWORD best = 0;
  const bool have_best = GetBestInterface(0, &best) == NO_ERROR;  // toward 0.0.0.0: the default route
  for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); a; a = a->Next) {
    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
    std::uint32_t gateway = 0;
    for (auto* g = a->FirstGatewayAddress; g; g = g->Next) {
      const sockaddr* s = g->Address.lpSockaddr;
      if (s && s->sa_family == AF_INET) {
        gateway = ntohl(reinterpret_cast<const sockaddr_in*>(s)->sin_addr.s_addr);
        if (gateway) break;
      }
    }
    for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
      const sockaddr* s = u->Address.lpSockaddr;
      if (!s || s->sa_family != AF_INET) continue;
      LocalNet net;
      net.ip = ntohl(reinterpret_cast<const sockaddr_in*>(s)->sin_addr.s_addr);
      net.mask = mask_of(u->OnLinkPrefixLength);
      net.gateway = gateway;
      net.default_route = have_best && a->IfIndex == best;
      if (net.ip && (net.ip >> 24) != 127) out.push_back(net);
    }
  }
  return out;
}

#else

namespace {

struct DefaultRoute {
  std::string iface;
  std::uint32_t gateway = 0;
  unsigned long metric = 0;
};

// Default routes from /proc/net/route (Linux).
std::vector<DefaultRoute> default_routes() {
  std::vector<DefaultRoute> out;
  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);  // the header
  while (std::getline(routes, line)) {
    std::istringstream fields(line);
    std::string iface, destination, gateway, flags, refcnt, use, metric;
    if (!(fields >> iface >> destination >> gateway >> flags >> refcnt >> use >> metric) || destination != "00000000") {
      continue;
    }
    try {
      // Written in the kernel's (little-endian on PCs) byte order.
      const auto raw = static_cast<std::uint32_t>(std::stoul(gateway, nullptr, 16));
      out.push_back({iface, ntohl(raw), std::stoul(metric)});
    } catch (...) {
    }
  }
  return out;
}

}  // namespace

std::vector<LocalNet> local_networks() {
  std::vector<LocalNet> out;
  ifaddrs* list = nullptr;
  if (getifaddrs(&list) != 0) return out;
  const std::vector<DefaultRoute> routes = default_routes();
  const DefaultRoute* best = nullptr;
  for (const DefaultRoute& r : routes) {
    if (!best || r.metric < best->metric) best = &r;
  }
  for (ifaddrs* i = list; i; i = i->ifa_next) {
    if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
    if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
    LocalNet net;
    net.ip = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr);
    if (i->ifa_netmask) net.mask = ntohl(reinterpret_cast<const sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
    for (const DefaultRoute& r : routes) {
      if (r.iface == i->ifa_name && r.gateway) {
        net.gateway = r.gateway;
        break;
      }
    }
    net.default_route = best && best->iface == i->ifa_name;
    if (net.ip) out.push_back(net);
  }
  freeifaddrs(list);
  return out;
}

#endif

}  // namespace coop::server::nat
