/* SPDX-License-Identifier: GPL-3.0-or-later
   Hosting and joining from the game's own menu (oot-remake-multiplayer, docs/hookshot.md "Hosting from the game").

   Optional exports of hookshot_coop.dll, found by name (GetProcAddress / dlsym): a module without them is older, and
   the menu then offers no Host or Join. The menu calls them from any thread; each returns at once and the work runs on
   the module's own "coop host" thread, so the menu polls hos_coop_status (cheap: a copy under a lock) each frame.

   Host: the module runs the co-op server inside the game (the same server as coop_server.exe: join password always,
   encrypted traffic, UPnP only when asked, invite code and answer codes), then connects this game to it.
   Join: an invite code goes through the module's own join helper (Join.bat's second window, in-process); a plain
   address connects directly.

   Plain C, fixed-size strings, so both sides' compilers agree on the layout. Every struct starts with its size: a
   caller sets it to sizeof, the module reads only what that covers. */
#ifndef HOS_COOP_HOST_H
#define HOS_COOP_HOST_H
#include <stdint.h>

#define HOS_COOP_HOST_API_VERSION 1u

typedef struct HosCoopHostOptions {
    uint32_t size;          /* sizeof(HosCoopHostOptions) */
    const char *name;       /* this player's nickname, 1 to 16 characters; NULL: coop.ini's */
    const char *password;   /* the join password; NULL or "": a new random one (shown in the status, in the invite) */
    uint16_t port;          /* the game port; 0: 27510. A few after it are tried when it is taken. */
    uint8_t upnp;           /* 1 only after the player said yes to opening the port on the router */
    uint8_t invite;         /* 1: ask a public STUN server for this PC's address and make an invite code */
    uint8_t pvp;            /* 1: players can hurt each other */
    const char *relay_via;  /* a relay "host[:port]"; NULL or "": none */
    const char *relay_key;  /* that relay's host key; NULL: none */
    uint8_t max_players;    /* most players at once, 1 to 16; 0: 16 */
    uint8_t reserved[3];
    uint32_t pose_upload_kbps;  /* the host's upload for other players' poses, kilobits a second; 0: no cap */
} HosCoopHostOptions;

typedef struct HosCoopJoinOptions {
    uint32_t size;          /* sizeof(HosCoopJoinOptions) */
    const char *name;       /* NULL: coop.ini's */
    const char *target;     /* an invite code, or the host's address "host[:port]" */
    const char *password;   /* for an address; an invite code holds its own */
} HosCoopJoinOptions;

enum {
    HOS_COOP_IDLE = 0,       /* nothing hosted or joined */
    HOS_COOP_STARTING = 1,   /* the host thread is starting the server / the helper */
    HOS_COOP_HOSTING = 2,    /* the server listens; invite holds the code once the STUN round is over */
    HOS_COOP_JOINING = 3,    /* trying the host (an invite code: directly first) */
    HOS_COOP_WAIT_HOST = 4,  /* the host could not be reached directly: answer holds the ANSWER CODE for the host */
    HOS_COOP_CONNECTED = 5,  /* the way to the host is open and this game was told to connect */
    HOS_COOP_FAILED = 6      /* message says why; hos_coop_leave, then try again */
};

/* What went wrong, as a number, so the game's own menu can say it in its own words (its message system); message holds
   the same in English for the log. */
enum {
    HOS_COOP_PROBLEM_NONE = 0,
    HOS_COOP_PROBLEM_ALREADY_RUNNING = 1,    /* hosting or joining already: leave first */
    HOS_COOP_PROBLEM_BAD_NAME = 2,           /* longer than 16 characters */
    HOS_COOP_PROBLEM_BAD_PASSWORD = 3,       /* a space, a control character or too long */
    HOS_COOP_PROBLEM_NO_PASSWORD = 4,        /* joining an address needs the host's join password */
    HOS_COOP_PROBLEM_PORT_TAKEN = 5,         /* another program (a server window?) has the port and the next ones */
    HOS_COOP_PROBLEM_NETWORK = 6,            /* the sockets did not start */
    HOS_COOP_PROBLEM_NO_INVITE_CODE = 7,     /* no STUN answer and no router forward: LAN or VPN only (still hosting) */
    HOS_COOP_PROBLEM_UPNP_REFUSED = 8,       /* the router did not open the port (still hosting) */
    HOS_COOP_PROBLEM_RELAY_FAILED = 9,       /* the relay could not be used (still hosting) */
    HOS_COOP_PROBLEM_BAD_CODE = 10,          /* an invite or answer code that is not one, or not for this session */
    HOS_COOP_PROBLEM_NO_TARGET = 11,         /* nothing typed, or an address this PC cannot find */
    HOS_COOP_PROBLEM_HELPER_PORT_TAKEN = 12, /* the join helper's local port is taken (a Join.bat window open?) */
    HOS_COOP_PROBLEM_CLIENT_FAILED = 13,     /* this game's co-op did not start (the log says why) */
    HOS_COOP_PROBLEM_INTERNAL = 14,          /* the host thread stopped on an error (the log says which) */
    HOS_COOP_PROBLEM_SYMMETRIC_NAT = 15      /* a hint: friends may not get through this router (still hosting) */
};

typedef struct HosCoopStatus {
    uint32_t size;           /* the caller sets sizeof(HosCoopStatus) */
    int32_t state;           /* HOS_COOP_* */
    uint16_t port;           /* the port the server listens on (host) */
    uint16_t players;        /* players in the session (host) */
    uint8_t invite_done;     /* the STUN round is over (invite may still be empty: message says why) */
    uint8_t upnp_open;       /* the router forwards the port */
    uint8_t reserved[2];
    char invite[160];        /* the invite code for friends (host) */
    char relay_code[160];    /* "<code>@<relay>" (host with a relay) */
    char answer[160];        /* the ANSWER CODE to send to the host (guest, WAIT_HOST) */
    char password[132];      /* the join password (host); show it only when the player asks */
    char message[256];       /* the last problem or hint, plain English; "" when none */
    int32_t problem;         /* HOS_COOP_PROBLEM_* for message */
    uint32_t stream_kbps;    /* host: other players' samples and poses sent in the last second, kilobits (the upload) */
} HosCoopStatus;

/* The module's exports (extern "C"). */
typedef int (*hos_coop_host_start_fn)(const HosCoopHostOptions *options);   /* 1: started; 0: refused (status) */
typedef int (*hos_coop_join_fn)(const HosCoopJoinOptions *options);         /* 1: started; 0: refused (status) */
typedef int (*hos_coop_host_answer_fn)(const char *answer_code);  /* the host pastes a friend's ANSWER CODE; 0: bad */
typedef void (*hos_coop_leave_fn)(void);                          /* stops what start/join began; safe when idle */
typedef void (*hos_coop_status_fn)(HosCoopStatus *out);            /* fills up to out->size */

#endif
