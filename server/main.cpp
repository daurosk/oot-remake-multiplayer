#include <cstdio>

#include "coop/version.hpp"

int main() {
  if (!coop::enet_starts()) {
    std::fprintf(stderr, "coop_server: ENet failed to start\n");
    return 1;
  }
  std::printf("coop_server: placeholder, nothing to serve yet\n");
  return 0;
}
