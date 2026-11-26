/* ENet platform header for our nn::socket port (switch/net/enet_nx.cpp).
   Found before ENet's own include/enet/unix.h because the build puts this
   shim folder first on the include path; ENet's enet.h includes "enet/unix.h"
   when _WIN32 is not defined. Same types and names as ENet's unix.h. */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef int ENetSocket;
#define ENET_SOCKET_NULL -1

#define ENET_HOST_TO_NET_16(value) ((enet_uint16)__builtin_bswap16((uint16_t)(value)))
#define ENET_HOST_TO_NET_32(value) ((enet_uint32)__builtin_bswap32((uint32_t)(value)))
#define ENET_NET_TO_HOST_16(value) ((enet_uint16)__builtin_bswap16((uint16_t)(value)))
#define ENET_NET_TO_HOST_32(value) ((enet_uint32)__builtin_bswap32((uint32_t)(value)))

typedef struct {
    void *data;
    size_t dataLength;
} ENetBuffer;

#define ENET_CALLBACK
#define ENET_API extern

/* enet_socketset_select is not provided (ENet's host code never calls it). */
typedef struct { uint64_t bits[16]; } ENetSocketSet;
#define ENET_SOCKETSET_EMPTY(sockset) ((void)0)
#define ENET_SOCKETSET_ADD(sockset, socket) ((void)0)
#define ENET_SOCKETSET_REMOVE(sockset, socket) ((void)0)
#define ENET_SOCKETSET_CHECK(sockset, socket) 0
