/* SPDX-License-Identifier: GPL-3.0-or-later
   (oot-remake-multiplayer's patch 0027) A co-op build and Longshot's crash reports and statistics (rc.6's #983).

   A co-op build is not Longshot's to support, so it sends nothing to Longshot's services and never says it is a Longshot
   release (owner's decision, 2026-11-17: crashes go through co-op's own /bugreport and collect-logs):
   - host/hos_crash_dialog.c: no address is ever read (find_addresses), so neither Report automatically nor the silent send
     of longshot_crash.ini is offered; the report's release flag is always 0 ("No (a local or modified build)");
   - Report manually still saves and copies the report, and opens HOS_COOP_CRASH_REPORT_PAGE below, not Longshot's issue
     form (host/hos_crash_dialog.h's words say so);
   - gpu/vk/perf_report.cpp: perf_report_init never starts, so no performance summary is measured or written;
   - nx2/addresses.ini: every address empty, so the build's stamp has no crash_reports= and the sender has nowhere to send.

   The report co-op's players save and send to us still leaves co-op's own log lines out: the module logs through hos_log
   (patch 0001's bridge_log), so the run's log holds the server's address and the players' nicknames. Both readers of the
   log (crash_lines and read_log) ask hos_coop_crash_private first and skip such a line.

   A line as hos_log writes it: "[hos <thread>] [<level>] <text>" (the level tag only when not info); the module's own level
   word ("warning: ", "error: ", "critical: ", hookshot/compat/common/logging.h) may lead the text. Co-op's text begins with
   "coop:" (the module, patch 0001's bridge) or "coop voice:" (patch 0009). Plain C. */
#ifndef HOS_COOP_CRASH_H
#define HOS_COOP_CRASH_H
#include <string.h>

#define HOS_COOP_CRASH_REPORT_PAGE "https://github.com/daurosk/oot-remake-multiplayer/blob/main/docs/setup.md#reporting-a-problem"

static inline const char *hos_coop_crash_skip_tag(const char *p) {   /* (one "[...] " at p, when there is one) */
    if (*p != '[') return p;
    const char *end = strchr(p, ']');
    if (!end) return p;
    for (p = end + 1; *p == ' '; p++) {}
    return p;
}

static inline int hos_coop_crash_private(const char *line) {
    const char *p = hos_coop_crash_skip_tag(hos_coop_crash_skip_tag(line));
    static const char *const levels[] = {"warning: ", "error: ", "critical: "};
    for (size_t i = 0; i < sizeof levels / sizeof *levels; i++)
        if (!strncmp(p, levels[i], strlen(levels[i]))) {
            p += strlen(levels[i]);
            break;
        }
    return !strncmp(p, "coop:", 5) || !strncmp(p, "coop voice:", 11);
}
#endif
