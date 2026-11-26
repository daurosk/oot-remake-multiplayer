// SPDX-License-Identifier: GPL-3.0-or-later
/* Freestanding stand-in for <stdio.h>: ENet's protocol.c includes it and uses
   printf only under ENET_DEBUG, which we never define. */
#pragma once
#include <stddef.h>
