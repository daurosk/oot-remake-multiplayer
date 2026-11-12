// The router port forward (server/upnp.hpp): parsing a router's answers, and
// a whole open/renew/close against a fake router on 127.0.0.1.

#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <enet/enet.h>

#include "upnp.hpp"

using namespace coop::server::upnp;

namespace {

// Shaped like a real IGD v1 description: the service sits three devices deep.
const char* kDescription = R"(<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<serviceList><service>
<serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>
<controlURL>/ctl/L3F</controlURL>
</service></serviceList>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList>
<service>
<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>
<controlURL>/ctl/PPP</controlURL>
</service>
<service>
<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl/IPConn</controlURL>
</service>
</serviceList>
</device></deviceList>
</device></deviceList>
</device>
</root>)";

}  // namespace

TEST_CASE("upnp: URLs, SSDP answers and the WAN service in a description") {
  auto url = parse_url("http://192.168.1.1:5000/rootDesc.xml");
  REQUIRE(url.has_value());
  CHECK(url->host == "192.168.1.1");
  CHECK(url->port == 5000);
  CHECK(url->path == "/rootDesc.xml");
  CHECK(parse_url("http://10.0.0.1")->path == "/");
  CHECK_FALSE(parse_url("https://10.0.0.1/").has_value());
  CHECK_FALSE(parse_url("http://10.0.0.1:99999/").has_value());
  CHECK(resolve_url(*url, "/ctl/IPConn")->path == "/ctl/IPConn");
  CHECK(resolve_url(*parse_url("http://1.2.3.4/a/desc.xml"), "ctl")->path == "/a/ctl");
  CHECK(resolve_url(*url, "http://192.168.1.1:6000/x")->port == 6000);

  CHECK(search_location("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nlocation: http://192.168.1.1:5000/rootDesc.xml\r\n"
                        "ST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n") ==
        std::string("http://192.168.1.1:5000/rootDesc.xml"));
  CHECK_FALSE(search_location("NOTIFY * HTTP/1.1\r\nLOCATION: http://x/\r\n\r\n").has_value());
  CHECK(search_request("urn:x").find("ST: urn:x\r\n") != std::string::npos);

  auto service = find_wan_service(kDescription, *url);
  REQUIRE(service.has_value());
  CHECK(service->type == "urn:schemas-upnp-org:service:WANIPConnection:1");
  CHECK(service->control.host == "192.168.1.1");
  CHECK(service->control.port == 5000);
  CHECK(service->control.path == "/ctl/IPConn");

  // Only PPP, and a URLBase.
  const std::string ppp =
      "<root><URLBase>http://192.168.0.1:80/</URLBase><service><serviceType>"
      "urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>upnp/ppp</controlURL>"
      "</service></root>";
  service = find_wan_service(ppp, *url);
  REQUIRE(service.has_value());
  CHECK(service->type == "urn:schemas-upnp-org:service:WANPPPConnection:1");
  CHECK(service->control.port == 80);
  CHECK(service->control.path == "/upnp/ppp");
  CHECK_FALSE(find_wan_service("<root><device/></root>", *url).has_value());
}

TEST_CASE("upnp: HTTP replies, SOAP faults and addresses") {
  auto r = parse_http_reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello and more");
  REQUIRE(r.has_value());
  CHECK(r->status == 200);
  CHECK(r->body == "hello");
  r = parse_http_reply("HTTP/1.1 500 Internal Server Error\r\nTRANSFER-ENCODING: chunked\r\n\r\n"
                       "6\r\n<a>12<\r\n5\r\n/a>xy\r\n0\r\n\r\n");
  REQUIRE(r.has_value());
  CHECK(r->status == 500);
  CHECK(r->body == "<a>12</a>xy");
  CHECK_FALSE(parse_http_reply("garbage").has_value());

  const std::string fault =
      "<s:Envelope><s:Body><s:Fault><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
      "<errorCode>725</errorCode><errorDescription>OnlyPermanentLeasesSupported</errorDescription>"
      "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
  CHECK(upnp_error(fault) == 725);
  CHECK(upnp_error("<ok/>") == 0);
  CHECK(xml_value("<u:R xmlns:u=\"x\"><NewExternalIPAddress> 1.2.3.4 </NewExternalIPAddress></u:R>",
                  "NewExternalIPAddress") == std::string("1.2.3.4"));

  const Service s{"urn:schemas-upnp-org:service:WANIPConnection:1", *parse_url("http://10.0.0.1:1900/ctl")};
  const std::string soap = soap_request(s, "AddPortMapping", {{"NewProtocol", "UDP"}, {"D", "a<b"}});
  CHECK(soap.find("POST /ctl HTTP/1.1\r\n") == 0);
  CHECK(soap.find("SOAPAction: \"urn:schemas-upnp-org:service:WANIPConnection:1#AddPortMapping\"") !=
        std::string::npos);
  CHECK(soap.find("<NewProtocol>UDP</NewProtocol>") != std::string::npos);
  CHECK(soap.find("<D>a&lt;b</D>") != std::string::npos);

  CHECK(parse_ipv4("81.2.3.4") == 0x51020304u);
  CHECK_FALSE(parse_ipv4("1.2.3").has_value());
  CHECK_FALSE(parse_ipv4("1.2.3.256").has_value());
  CHECK(is_not_public(*parse_ipv4("100.64.1.2")));   // CGNAT
  CHECK(is_not_public(*parse_ipv4("192.168.0.10")));
  CHECK(is_not_public(*parse_ipv4("172.20.0.1")));
  CHECK(is_not_public(*parse_ipv4("0.0.0.0")));
  CHECK_FALSE(is_not_public(*parse_ipv4("81.2.3.4")));
  CHECK_FALSE(is_not_public(*parse_ipv4("100.128.0.1")));
}

namespace {

// A router on 127.0.0.1: answers M-SEARCH, serves the description and takes
// SOAP actions. Runs on its own thread while the test's Mapping blocks.
struct FakeRouter {
  ENetSocket ssdp = ENET_SOCKET_NULL;
  ENetSocket http = ENET_SOCKET_NULL;
  std::uint16_t ssdp_port = 0;
  std::uint16_t http_port = 0;
  std::atomic<bool> stop{false};
  std::atomic<bool> only_permanent{false};
  std::mutex lock;
  std::vector<std::string> actions;  // "AddPortMapping lease=600", ...
  std::thread thread;

  FakeRouter() {
    ENetAddress local{};
    enet_address_set_host_ip(&local, "127.0.0.1");
    ssdp = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
    REQUIRE(enet_socket_bind(ssdp, &local) == 0);
    ENetAddress got{};
    enet_socket_get_address(ssdp, &got);
    ssdp_port = got.port;
    http = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
    REQUIRE(enet_socket_bind(http, &local) == 0);
    REQUIRE(enet_socket_listen(http, 4) == 0);
    enet_socket_get_address(http, &got);
    http_port = got.port;
    thread = std::thread([this] { run(); });
  }
  ~FakeRouter() {
    stop = true;
    thread.join();
    enet_socket_destroy(ssdp);
    enet_socket_destroy(http);
  }

  static std::string value(const std::string& body, const std::string& tag) {
    const auto a = body.find("<" + tag + ">");
    const auto b = body.find("</" + tag + ">");
    if (a == std::string::npos || b == std::string::npos) return {};
    return body.substr(a + tag.size() + 2, b - a - tag.size() - 2);
  }

  std::string answer(const std::string& request) {
    auto ok = [](const std::string& body) {
      return "HTTP/1.1 200 OK\r\nContent-Type: text/xml\r\nContent-Length: " + std::to_string(body.size()) +
             "\r\nConnection: close\r\n\r\n" + body;
    };
    if (request.rfind("GET /rootDesc.xml", 0) == 0) return ok(kDescription);
    const auto action_at = request.find("#");
    const auto action_end = request.find('"', action_at);
    const std::string action = request.substr(action_at + 1, action_end - action_at - 1);
    if (action == "GetExternalIPAddress") {
      return ok("<s:Envelope><s:Body><u:GetExternalIPAddressResponse><NewExternalIPAddress>81.2.3.4"
                "</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>");
    }
    std::string line = action;
    if (action == "AddPortMapping") {
      const std::string lease = value(request, "NewLeaseDuration");
      line += " lease=" + lease + " port=" + value(request, "NewExternalPort") + " to=" +
              value(request, "NewInternalClient") + ":" + value(request, "NewInternalPort") + " " +
              value(request, "NewProtocol");
      if (only_permanent && lease != "0") {
        std::lock_guard<std::mutex> g(lock);
        actions.push_back(line + " refused");
        const std::string fault = "<s:Envelope><s:Body><s:Fault><detail><UPnPError><errorCode>725</errorCode>"
                                  "</UPnPError></detail></s:Fault></s:Body></s:Envelope>";
        return "HTTP/1.1 500 Internal Server Error\r\nContent-Length: " + std::to_string(fault.size()) +
               "\r\n\r\n" + fault;
      }
    }
    {
      std::lock_guard<std::mutex> g(lock);
      actions.push_back(line);
    }
    return ok("<s:Envelope><s:Body/></s:Envelope>");
  }

  void run() {
    char data[8192];
    while (!stop) {
      ENetSocketSet read;
      ENET_SOCKETSET_EMPTY(read);
      ENET_SOCKETSET_ADD(read, ssdp);
      ENET_SOCKETSET_ADD(read, http);
      const ENetSocket top = ssdp > http ? ssdp : http;
      if (enet_socketset_select(top, &read, nullptr, 20) <= 0) continue;
      if (ENET_SOCKETSET_CHECK(read, ssdp)) {
        ENetAddress from{};
        ENetBuffer b;
        b.data = data;
        b.dataLength = sizeof data;
        if (enet_socket_receive(ssdp, &from, &b, 1) > 0) {
          const std::string reply = "HTTP/1.1 200 OK\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n"
                                    "LOCATION: http://127.0.0.1:" + std::to_string(http_port) +
                                    "/rootDesc.xml\r\n\r\n";
          ENetBuffer out;
          out.data = const_cast<char*>(reply.data());
          out.dataLength = reply.size();
          enet_socket_send(ssdp, &from, &out, 1);
        }
      }
      if (ENET_SOCKETSET_CHECK(read, http)) {
        ENetSocket c = enet_socket_accept(http, nullptr);
        if (c == ENET_SOCKET_NULL) continue;
        std::string request;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (std::chrono::steady_clock::now() < deadline) {
          enet_uint32 wait = ENET_SOCKET_WAIT_RECEIVE;
          if (enet_socket_wait(c, &wait, 100) != 0 || !(wait & ENET_SOCKET_WAIT_RECEIVE)) continue;
          ENetBuffer b;
          b.data = data;
          b.dataLength = sizeof data;
          const int n = enet_socket_receive(c, nullptr, &b, 1);
          if (n <= 0) break;
          request.append(data, static_cast<std::size_t>(n));
          const auto head = request.find("\r\n\r\n");
          if (head == std::string::npos) continue;
          const auto cl = request.find("Content-Length: ");
          if (cl == std::string::npos || cl > head) break;
          const std::size_t length = std::stoul(request.substr(cl + 16));
          if (request.size() >= head + 4 + length) break;
        }
        const std::string reply = answer(request);
        ENetBuffer out;
        out.data = const_cast<char*>(reply.data());
        out.dataLength = reply.size();
        enet_socket_send(c, nullptr, &out, 1);
        enet_socket_shutdown(c, ENET_SOCKET_SHUTDOWN_WRITE);
        enet_socket_destroy(c);
      }
    }
  }

  std::vector<std::string> seen() {
    std::lock_guard<std::mutex> g(lock);
    return actions;
  }
};

struct EnetLife {
  EnetLife() { REQUIRE(enet_initialize() == 0); }
  ~EnetLife() { enet_deinitialize(); }
};

Config fake_config(const FakeRouter& router, std::vector<std::string>& log) {
  Config c;
  c.internal_port = 27510;
  c.external_port = 27510;
  c.discovery_host = "127.0.0.1";
  c.discovery_port = router.ssdp_port;
  c.discovery_ms = 1500;
  c.log = [&log](const std::string& text) { log.push_back(text); };
  return c;
}

}  // namespace

TEST_CASE("upnp loopback: the forward is added with a lease and removed on close") {
  EnetLife enet;
  FakeRouter router;
  std::vector<std::string> log;
  {
    Mapping mapping(fake_config(router, log));
    REQUIRE(mapping.open());
    CHECK(mapping.is_open());
    CHECK(mapping.external_address() == "81.2.3.4");
    CHECK(mapping.internal_address() == "127.0.0.1");
    mapping.renew_if_due();  // not due yet
  }  // the destructor removes it
  const auto actions = router.seen();
  REQUIRE(actions.size() == 2);
  CHECK(actions[0] == "AddPortMapping lease=600 port=27510 to=127.0.0.1:27510 UDP");
  CHECK(actions[1] == "DeletePortMapping");
  bool said_forward = false;
  bool said_address = false;
  for (const auto& line : log) {
    said_forward = said_forward || line.find("now forwards UDP port 27510") != std::string::npos;
    said_address = said_address || line.find("81.2.3.4") != std::string::npos;
  }
  CHECK(said_forward);
  CHECK(said_address);
}

TEST_CASE("upnp loopback: a router with only permanent mappings gets one") {
  EnetLife enet;
  FakeRouter router;
  router.only_permanent = true;
  std::vector<std::string> log;
  Mapping mapping(fake_config(router, log));
  REQUIRE(mapping.open());
  mapping.close();
  CHECK_FALSE(mapping.is_open());
  const auto actions = router.seen();
  REQUIRE(actions.size() == 3);
  CHECK(actions[0].find("lease=600") != std::string::npos);
  CHECK(actions[0].find("refused") != std::string::npos);
  CHECK(actions[1].find("lease=0") != std::string::npos);
  CHECK(actions[2] == "DeletePortMapping");
}

TEST_CASE("upnp loopback: no router answers, so nothing is opened and it says why") {
  EnetLife enet;
  // A port with nobody on it.
  ENetSocket quiet = enet_socket_create(ENET_SOCKET_TYPE_DATAGRAM);
  ENetAddress local{};
  enet_address_set_host_ip(&local, "127.0.0.1");
  REQUIRE(enet_socket_bind(quiet, &local) == 0);
  ENetAddress got{};
  enet_socket_get_address(quiet, &got);
  std::vector<std::string> log;
  Config c;
  c.internal_port = 27510;
  c.external_port = 27510;
  c.discovery_host = "127.0.0.1";
  c.discovery_port = got.port;
  c.discovery_ms = 300;
  c.log = [&log](const std::string& text) { log.push_back(text); };
  Mapping mapping(c);
  CHECK_FALSE(mapping.open());
  enet_socket_destroy(quiet);
  REQUIRE(log.size() == 1);
  CHECK(log[0].find("no router answered") != std::string::npos);
}
