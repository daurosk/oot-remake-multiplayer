#include "upnp.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

#include <enet/enet.h>

namespace coop::server::upnp {

namespace {

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

bool starts_with_nocase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lower(text[i]) != lower(prefix[i])) return false;
  }
  return true;
}

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

// The value of header `name` in the lines after the first; nothing if absent.
std::optional<std::string> header_value(std::string_view head, std::string_view name) {
  std::size_t at = head.find('\n');
  while (at != std::string_view::npos && at + 1 < head.size()) {
    const std::size_t end = head.find('\n', at + 1);
    std::string_view line = head.substr(at + 1, end == std::string_view::npos ? std::string_view::npos : end - at - 1);
    const std::size_t colon = line.find(':');
    if (colon != std::string_view::npos && trim(line.substr(0, colon)).size() == name.size() &&
        starts_with_nocase(trim(line.substr(0, colon)), name)) {
      return std::string(trim(line.substr(colon + 1)));
    }
    at = end;
  }
  return std::nullopt;
}

bool parse_port(std::string_view text, std::uint16_t& port) {
  if (text.empty() || text.size() > 5) return false;
  unsigned v = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
    v = v * 10 + static_cast<unsigned>(c - '0');
  }
  if (v == 0 || v > 65535) return false;
  port = static_cast<std::uint16_t>(v);
  return true;
}

bool name_char(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.';
}

// Each element whose local name is `tag`: its inner text, in order.
std::vector<std::string_view> elements(std::string_view xml, std::string_view tag) {
  std::vector<std::string_view> found;
  std::size_t at = 0;
  while ((at = xml.find('<', at)) != std::string_view::npos) {
    std::size_t name_start = at + 1;
    std::size_t name_end = name_start;
    while (name_end < xml.size() && (name_char(xml[name_end]) || xml[name_end] == ':')) ++name_end;
    std::string_view name = xml.substr(name_start, name_end - name_start);
    if (const auto colon = name.rfind(':'); colon != std::string_view::npos) name = name.substr(colon + 1);
    const std::size_t open_end = xml.find('>', name_end);
    if (open_end == std::string_view::npos) break;
    if (name == tag && name_end < xml.size() && (xml[name_end] == '>' || xml[name_end] == ' ' ||
                                                 xml[name_end] == '\t' || xml[name_end] == '\r' ||
                                                 xml[name_end] == '\n')) {
      if (xml[open_end - 1] == '/') {  // <Tag/>
        found.push_back(std::string_view());
        at = open_end;
        continue;
      }
      // The matching close tag: the next "</...tag>" (these elements do not
      // nest inside themselves in a router's description).
      std::size_t close = open_end;
      while ((close = xml.find("</", close)) != std::string_view::npos) {
        std::size_t n0 = close + 2;
        std::size_t n1 = n0;
        while (n1 < xml.size() && (name_char(xml[n1]) || xml[n1] == ':')) ++n1;
        std::string_view cname = xml.substr(n0, n1 - n0);
        if (const auto colon = cname.rfind(':'); colon != std::string_view::npos) cname = cname.substr(colon + 1);
        if (cname == tag) break;
        close = n1;
      }
      if (close == std::string_view::npos) break;
      found.push_back(xml.substr(open_end + 1, close - open_end - 1));
      at = close + 2;
      continue;
    }
    at = open_end;
  }
  return found;
}

std::string escape(std::string_view text) {
  std::string out;
  for (const char c : text) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

std::uint64_t steady_ms() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

bool reply_complete(std::string_view raw) {
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string_view::npos) return false;
  const std::string_view head = raw.substr(0, head_end);
  const std::string_view body = raw.substr(head_end + 4);
  if (auto te = header_value(head, "Transfer-Encoding"); te && starts_with_nocase(*te, "chunked")) {
    return body.size() >= 5 && body.substr(body.size() - 5) == "0\r\n\r\n";
  }
  if (auto cl = header_value(head, "Content-Length")) {
    std::size_t n = 0;
    for (const char c : *cl) {
      if (c < '0' || c > '9') return false;
      n = n * 10 + static_cast<std::size_t>(c - '0');
    }
    return body.size() >= n;
  }
  return false;  // until the router closes
}

// One HTTP exchange over TCP. `local` gets this PC's address on the way to
// the router (the one to forward to).
std::optional<std::string> exchange(const Url& url, const std::string& request, std::uint32_t timeout_ms,
                                    std::string* local) {
  ENetAddress to{};
  if (enet_address_set_host_ip(&to, url.host.c_str()) != 0 && enet_address_set_host(&to, url.host.c_str()) != 0) {
    return std::nullopt;
  }
  to.port = url.port;
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
  if (s == ENET_SOCKET_NULL) return std::nullopt;
  struct Closer {
    ENetSocket s;
    ~Closer() { enet_socket_destroy(s); }
  } closer{s};
  enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
  if (enet_socket_connect(s, &to) != 0) return std::nullopt;
  const std::uint64_t deadline = steady_ms() + timeout_ms;
  auto left = [&]() -> enet_uint32 {
    const std::uint64_t now = steady_ms();
    return now >= deadline ? 0u : static_cast<enet_uint32>(deadline - now);
  };
  enet_uint32 wait = ENET_SOCKET_WAIT_SEND;
  if (enet_socket_wait(s, &wait, left()) != 0 || !(wait & ENET_SOCKET_WAIT_SEND)) return std::nullopt;
  int socket_error = 0;
  if (enet_socket_get_option(s, ENET_SOCKOPT_ERROR, &socket_error) == 0 && socket_error != 0) return std::nullopt;
  if (local != nullptr) {
    ENetAddress mine{};
    char text[64] = {};
    if (enet_socket_get_address(s, &mine) == 0 && enet_address_get_host_ip(&mine, text, sizeof text) == 0) {
      *local = text;
    }
  }
  std::size_t sent = 0;
  while (sent < request.size()) {
    ENetBuffer b;
    b.data = const_cast<char*>(request.data() + sent);
    b.dataLength = request.size() - sent;
    const int n = enet_socket_send(s, nullptr, &b, 1);
    if (n < 0) return std::nullopt;
    if (n == 0) {
      wait = ENET_SOCKET_WAIT_SEND;
      if (left() == 0 || enet_socket_wait(s, &wait, left()) != 0) return std::nullopt;
      continue;
    }
    sent += static_cast<std::size_t>(n);
  }
  std::string raw;
  char data[4096];
  while (raw.size() < (1u << 20)) {
    wait = ENET_SOCKET_WAIT_RECEIVE;
    if (left() == 0 || enet_socket_wait(s, &wait, left()) != 0) break;
    if (!(wait & ENET_SOCKET_WAIT_RECEIVE)) break;
    ENetBuffer b;
    b.data = data;
    b.dataLength = sizeof data;
    const int n = enet_socket_receive(s, nullptr, &b, 1);
    if (n <= 0) break;  // readable and nothing: the router closed it
    raw.append(data, static_cast<std::size_t>(n));
    if (reply_complete(raw)) break;
  }
  if (raw.empty()) return std::nullopt;
  return raw;
}

}  // namespace

std::optional<Url> parse_url(std::string_view text) {
  text = trim(text);
  if (!starts_with_nocase(text, "http://")) return std::nullopt;
  text.remove_prefix(7);
  Url url;
  const std::size_t slash = text.find('/');
  std::string_view authority = text.substr(0, slash);
  url.path = slash == std::string_view::npos ? "/" : std::string(text.substr(slash));
  if (const auto colon = authority.rfind(':'); colon != std::string_view::npos) {
    if (!parse_port(authority.substr(colon + 1), url.port)) return std::nullopt;
    authority = authority.substr(0, colon);
  }
  if (authority.empty()) return std::nullopt;
  url.host = std::string(authority);
  return url;
}

std::optional<Url> resolve_url(const Url& base, std::string_view relative) {
  relative = trim(relative);
  if (relative.empty()) return std::nullopt;
  if (starts_with_nocase(relative, "http://")) return parse_url(relative);
  Url url = base;
  if (relative.front() == '/') {
    url.path = std::string(relative);
  } else {
    const std::size_t dir = base.path.rfind('/');
    url.path = (dir == std::string::npos ? std::string("/") : base.path.substr(0, dir + 1)) + std::string(relative);
  }
  return url;
}

std::string search_request(std::string_view search_target) {
  return "M-SEARCH * HTTP/1.1\r\n"
         "HOST: 239.255.255.250:1900\r\n"
         "MAN: \"ssdp:discover\"\r\n"
         "MX: 2\r\n"
         "ST: " + std::string(search_target) + "\r\n\r\n";
}

std::optional<std::string> search_location(std::string_view answer) {
  if (!starts_with_nocase(answer, "HTTP/1.1 200") && !starts_with_nocase(answer, "HTTP/1.0 200")) return std::nullopt;
  auto location = header_value(answer, "LOCATION");
  if (!location || location->empty()) return std::nullopt;
  return location;
}

std::optional<Service> find_wan_service(std::string_view xml, const Url& description_url) {
  Url base = description_url;
  for (std::string_view b : elements(xml, "URLBase")) {
    if (auto u = parse_url(b)) base = *u;
    break;
  }
  static constexpr std::string_view kTypes[] = {
      "urn:schemas-upnp-org:service:WANIPConnection:2",
      "urn:schemas-upnp-org:service:WANIPConnection:1",
      "urn:schemas-upnp-org:service:WANPPPConnection:1",
  };
  const auto services = elements(xml, "service");
  for (const std::string_view wanted : kTypes) {
    for (const std::string_view block : services) {
      auto type = xml_value(block, "serviceType");
      auto control = xml_value(block, "controlURL");
      if (!type || !control || trim(*type) != wanted) continue;
      if (auto url = resolve_url(base, *control)) return Service{std::string(wanted), *url};
    }
  }
  return std::nullopt;
}

std::string get_request(const Url& url) {
  return "GET " + url.path + " HTTP/1.1\r\nHost: " + url.host + ":" + std::to_string(url.port) +
         "\r\nConnection: close\r\n\r\n";
}

std::string soap_request(const Service& service, std::string_view action,
                         const std::vector<std::pair<std::string, std::string>>& arguments) {
  std::string body =
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:" +
      std::string(action) + " xmlns:u=\"" + service.type + "\">";
  for (const auto& [name, value] : arguments) body += "<" + name + ">" + escape(value) + "</" + name + ">";
  body += "</u:" + std::string(action) + "></s:Body></s:Envelope>\r\n";
  return "POST " + service.control.path + " HTTP/1.1\r\nHost: " + service.control.host + ":" +
         std::to_string(service.control.port) +
         "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + service.type + "#" +
         std::string(action) + "\"\r\nContent-Length: " + std::to_string(body.size()) +
         "\r\nConnection: close\r\n\r\n" + body;
}

std::optional<HttpReply> parse_http_reply(std::string_view raw) {
  if (!starts_with_nocase(raw, "HTTP/1.")) return std::nullopt;
  const std::size_t space = raw.find(' ');
  if (space == std::string_view::npos || space + 4 > raw.size()) return std::nullopt;
  HttpReply reply;
  for (std::size_t i = space + 1; i < space + 4; ++i) {
    if (raw[i] < '0' || raw[i] > '9') return std::nullopt;
    reply.status = reply.status * 10 + (raw[i] - '0');
  }
  const std::size_t head_end = raw.find("\r\n\r\n");
  if (head_end == std::string_view::npos) return std::nullopt;
  const std::string_view head = raw.substr(0, head_end);
  std::string_view body = raw.substr(head_end + 4);
  if (auto te = header_value(head, "Transfer-Encoding"); te && starts_with_nocase(*te, "chunked")) {
    std::string whole;
    for (;;) {
      const std::size_t line_end = body.find("\r\n");
      if (line_end == std::string_view::npos) return std::nullopt;
      std::size_t size = 0;
      std::size_t i = 0;
      for (; i < line_end && std::isxdigit(static_cast<unsigned char>(body[i])) != 0; ++i) {
        const char c = lower(body[i]);
        size = size * 16 + static_cast<std::size_t>(c <= '9' ? c - '0' : c - 'a' + 10);
        if (size > (1u << 20)) return std::nullopt;
      }
      if (i == 0) return std::nullopt;
      body.remove_prefix(line_end + 2);
      if (size == 0) break;
      if (body.size() < size) return std::nullopt;
      whole.append(body.substr(0, size));
      body.remove_prefix((std::min)(body.size(), size + 2));
    }
    reply.body = std::move(whole);
    return reply;
  }
  if (auto cl = header_value(head, "Content-Length")) {
    std::size_t n = 0;
    for (const char c : *cl) {
      if (c < '0' || c > '9') return std::nullopt;
      n = n * 10 + static_cast<std::size_t>(c - '0');
      if (n > (1u << 20)) return std::nullopt;
    }
    if (body.size() < n) return std::nullopt;
    body = body.substr(0, n);
  }
  reply.body = std::string(body);
  return reply;
}

std::optional<std::string> xml_value(std::string_view xml, std::string_view tag) {
  const auto found = elements(xml, tag);
  if (found.empty()) return std::nullopt;
  return std::string(trim(found.front()));
}

int upnp_error(std::string_view body) {
  auto code = xml_value(body, "errorCode");
  if (!code || code->empty() || code->size() > 6) return 0;
  int v = 0;
  for (const char c : *code) {
    if (c < '0' || c > '9') return 0;
    v = v * 10 + (c - '0');
  }
  return v;
}

std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
  std::uint32_t v = 0;
  int parts = 0;
  std::size_t i = 0;
  while (parts < 4) {
    if (i >= text.size() || text[i] < '0' || text[i] > '9') return std::nullopt;
    unsigned part = 0;
    std::size_t digits = 0;
    while (i < text.size() && text[i] >= '0' && text[i] <= '9' && digits < 4) {
      part = part * 10 + static_cast<unsigned>(text[i] - '0');
      ++i;
      ++digits;
    }
    if (part > 255 || digits > 3) return std::nullopt;
    v = (v << 8) | part;
    ++parts;
    if (parts < 4) {
      if (i >= text.size() || text[i] != '.') return std::nullopt;
      ++i;
    }
  }
  if (i != text.size()) return std::nullopt;
  return v;
}

bool is_not_public(std::uint32_t a) {
  return (a >> 24) == 0 || (a >> 24) == 10 || (a >> 24) == 127 || (a & 0xFFF00000u) == 0xAC100000u ||
         (a & 0xFFFF0000u) == 0xC0A80000u || (a & 0xFFC00000u) == 0x64400000u ||
         (a & 0xFFFF0000u) == 0xA9FE0000u;
}

Mapping::Mapping(Config config) : config_(std::move(config)) {}

Mapping::~Mapping() { close(); }

void Mapping::say(const std::string& text) const {
  if (config_.log) config_.log("UPnP: " + text);
}

bool Mapping::add(std::uint32_t lease, std::uint32_t timeout_ms) {
  if (!service_) return false;
  const std::string port = std::to_string(config_.external_port);
  const std::string request = soap_request(
      *service_, "AddPortMapping",
      {{"NewRemoteHost", ""},
       {"NewExternalPort", port},
       {"NewProtocol", "UDP"},
       {"NewInternalPort", std::to_string(config_.internal_port)},
       {"NewInternalClient", internal_},
       {"NewEnabled", "1"},
       {"NewPortMappingDescription", config_.description},
       {"NewLeaseDuration", std::to_string(lease)}});
  auto raw = exchange(service_->control, request, timeout_ms, nullptr);
  auto reply = raw ? parse_http_reply(*raw) : std::nullopt;
  if (!reply) {
    say("the router did not answer the request to forward UDP port " + port);
    return false;
  }
  if (reply->status == 200) {
    lease_ = lease;
    opened_ms_ = steady_ms();
    return true;
  }
  const int error = upnp_error(reply->body);
  if (error == 725 && lease != 0) {
    say("the router only keeps mappings until it is told to remove them; asking for one of those");
    return add(0, timeout_ms);
  }
  if (error == 718) {
    say("the router already forwards UDP port " + port +
        " to another device (or to an old address of this PC). Remove that forward in the router's "
        "page, or use another port");
  } else {
    say("the router refused to forward UDP port " + port + " (HTTP " + std::to_string(reply->status) +
        (error != 0 ? ", UPnP error " + std::to_string(error) : std::string()) + ")");
  }
  return false;
}

bool Mapping::open() {
  if (open_) return true;
  ENetSocket s = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  if (s == ENET_SOCKET_NULL) {
    say("cannot open a network socket to look for the router");
    return false;
  }
  struct Closer {
    ENetSocket s;
    ~Closer() { enet_socket_destroy(s); }
  } closer{s};
  ENetAddress any{};
  any.host = ENET_HOST_ANY;
  any.port = 0;
  enet_socket_bind(s, &any);
  enet_socket_set_option(s, ENET_SOCKOPT_NONBLOCK, 1);
  enet_socket_set_option(s, ENET_SOCKOPT_TTL, 2);
  ENetAddress group{};
  enet_address_set_host_ip(&group, config_.discovery_host.c_str());
  group.port = config_.discovery_port;
  const std::string targets[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                 "urn:schemas-upnp-org:service:WANIPConnection:1",
                                 "urn:schemas-upnp-org:service:WANPPPConnection:1"};
  for (int round = 0; round < 2; ++round) {
    for (const std::string& target : targets) {
      const std::string m = search_request(target);
      ENetBuffer b;
      b.data = const_cast<char*>(m.data());
      b.dataLength = m.size();
      enet_socket_send(s, &group, &b, 1);
    }
  }
  std::vector<std::string> locations;
  const std::uint64_t start = steady_ms();
  std::uint64_t first_answer = 0;
  char data[2048];
  for (;;) {
    const std::uint64_t now = steady_ms();
    if (now - start >= config_.discovery_ms) break;
    if (first_answer != 0 && now - first_answer >= 500) break;  // the others answer about at once
    enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
    enet_socket_wait(s, &wait, static_cast<enet_uint32>((std::min)(std::uint64_t{100}, config_.discovery_ms - (now - start))));
    for (;;) {
      ENetAddress from{};
      ENetBuffer b;
      b.data = data;
      b.dataLength = sizeof data;
      const int n = enet_socket_receive(s, &from, &b, 1);
      if (n <= 0) break;
      if (auto location = search_location(std::string_view(data, static_cast<std::size_t>(n)))) {
        if (std::find(locations.begin(), locations.end(), *location) == locations.end()) {
          locations.push_back(*location);
        }
        if (first_answer == 0) first_answer = steady_ms();
      }
    }
  }
  if (locations.empty()) {
    say("no router answered (UPnP is off on the router, or this PC is not on its network). "
        "Forward the port by hand, or use a VPN or a relay (docs/play-online.md)");
    return false;
  }
  for (const std::string& location : locations) {
    auto url = parse_url(location);
    if (!url) continue;
    auto raw = exchange(*url, get_request(*url), config_.http_timeout_ms, nullptr);
    auto reply = raw ? parse_http_reply(*raw) : std::nullopt;
    if (!reply || reply->status != 200) continue;
    service_ = find_wan_service(reply->body, *url);
    if (!service_) continue;
    // The external address, and on the way this PC's address towards the router.
    raw = exchange(service_->control, soap_request(*service_, "GetExternalIPAddress", {}),
                   config_.http_timeout_ms, &internal_);
    if (raw) {
      if (auto r = parse_http_reply(*raw); r && r->status == 200) {
        external_ = xml_value(r->body, "NewExternalIPAddress").value_or("");
      }
    }
    if (internal_.empty()) continue;
    if (!add(config_.lease_seconds, config_.http_timeout_ms)) return false;
    open_ = true;
    const std::string port = std::to_string(config_.external_port);
    say("the router now forwards UDP port " + port + " to this PC (" + internal_ + ")" +
        (lease_ == 0 ? std::string(" until the server stops (this router only keeps permanent forwards: if the "
                                   "server crashes, remove it in the router's page)")
                     : " for " + std::to_string(lease_ / 60) + " minutes at a time, renewed while the server runs") +
        "; it is removed when the server stops");
    if (const auto ext = parse_ipv4(external_)) {
      if (is_not_public(*ext)) {
        say("but the router's own internet address is " + external_ +
            ", a private or shared (CGNAT) one: the router is behind another one (your provider's), so "
            "nobody can come in through it from the internet. Use a VPN or a relay (docs/play-online.md)");
      } else {
        say("your internet address, for your friends: " + external_ + " (port " + port + ")");
      }
    }
    return true;
  }
  say("a router answered but has no port forwarding service this server can use. Forward the port by "
      "hand, or use a VPN or a relay (docs/play-online.md)");
  service_.reset();
  return false;
}

void Mapping::renew_if_due() {
  if (!open_ || lease_ == 0) return;
  const std::uint64_t half = std::uint64_t{lease_} * 500;
  if (steady_ms() - opened_ms_ < half) return;
  // Blocks the server loop for at most renew_timeout_ms (twice on error 725).
  if (!add(lease_, config_.renew_timeout_ms)) {
    say("could not renew the forward; trying again in 60 seconds");
    opened_ms_ = steady_ms() - half + 60'000;
  }
}

void Mapping::close() {
  if (!open_) return;
  open_ = false;
  const std::string port = std::to_string(config_.external_port);
  auto raw = exchange(service_->control,
                      soap_request(*service_, "DeletePortMapping",
                                   {{"NewRemoteHost", ""}, {"NewExternalPort", port}, {"NewProtocol", "UDP"}}),
                      config_.http_timeout_ms, nullptr);
  auto reply = raw ? parse_http_reply(*raw) : std::nullopt;
  if (reply && reply->status == 200) {
    say("the router no longer forwards UDP port " + port);
  } else {
    say("could not remove the forward of UDP port " + port +
        (lease_ == 0 ? std::string(": remove it in the router's page")
                     : ": the router drops it by itself within " + std::to_string(lease_ / 60) + " minutes"));
  }
}

}  // namespace coop::server::upnp
