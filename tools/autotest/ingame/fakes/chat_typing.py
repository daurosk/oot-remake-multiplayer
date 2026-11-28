"""Fake-game reactions for typing-blink-0121 (ingame/test_typing_blink.py; owner typing-blink-0121). Hooks: fakegame.py
docstring. (Named chat_typing to load before f1_event.py, whose `key` hook once answered every key; since f1-fake-scope
it takes F1 only and fakegame.builtin_answer answers a key no fake takes.)

What a real game does, each from the module source test_typing_blink.py cites:
- `key` T (coop_chatbox.h kOpenKey, coop_hookshot.cpp TypeKey) opens the chat box: taken. `key` text types
  (hos_coop_char's steps, coop_hookshot.cpp BridgeChars): taken = the characters. `key` Esc closes the box: taken.
- The typed line goes on the banner (coop_hookshot.cpp NativeTyping): one open, "typing on the game's banner" once,
  state 1 then 2 (+0x1a6), holds "ours", page true; the box closed: shut, no page, 2 frames on.
- The state's "banner" (CoopNativeUi::StatsJson) and the lines "coop: native ui: banner: ..." (TakeBannerLines).
BT_FAKE_TYPING=reopen: an open on the banner that shows every 8th key (the code before typing-blink-0121, when a page
lost its page_); =shut: the game shuts the banner every 40 frames and the next step opens it again (each one the
banner going out and in); =type: v0.12.0's Type reset on every key (Banner::Set before 1d8a1b11e). The test must
FAIL with each; it PASSes without.
"""
import os

TYPING_ON = "coop: native ui: typing on the game's banner"


def setup(game):
    game.ct_box = False
    game.ct_keys = 0
    game.ct_said = False
    game.ct_shut_at = None
    game.ct_b = {"opens": 0, "reopens": 0, "shut_by_game": 0, "taken_by_game": 0, "sets": 0, "type_resets": 0, "state": 0,
                 "request": 0, "holds": "shut", "page": False, "held": False}


def _bug(what):
    return os.environ.get("BT_FAKE_TYPING", "") == what


def _line(game, text):
    game.say("coop: native ui: banner: " + text)


def _open(game):
    b = game.ct_b
    b["opens"] += 1
    b["type_resets"] += 1
    was = b["holds"]
    b.update(state=1, request=0, holds="ours", page=True)
    _line(game, "%s -> ours (state 1, request 0, page)" % ("shut" if was != "ours" else "ours"))
    game.later(3, lambda: b.update(state=2) if b["page"] else None)
    if _bug("shut"):
        game.ct_shut_at = game.frame + 40


def state(game, st):
    b = game.ct_b
    if game.ct_shut_at is not None and game.frame >= game.ct_shut_at and b["page"]:
        b["shut_by_game"] += 1      # the game shut it; the next step opens it again
        _line(game, "ours -> shut (state 0, request 0, page)")
        _open(game)
    if not _bug("nostate"):          # (=nostate: a module before typing-blink-0121, no "banner" in its state)
        st["banner"] = dict(b)


def command(game, m):
    if m.get("cmd") != "key":
        return None
    b = game.ct_b
    text, key = m.get("text"), m.get("key")
    if text is not None:
        if not game.ct_box:
            return {"ok": True, "text": text, "taken": 0}
        for _ in text:
            game.ct_keys += 1
            b["sets"] += 1
            if b["holds"] != "ours":
                _open(game)
                if not game.ct_said:
                    game.ct_said = True
                    game.say(TYPING_ON)
            elif _bug("reopen") and game.ct_keys % 8 == 0:
                b["opens"] += 1
                b["state"] = 1
            elif _bug("type"):
                b["type_resets"] += 1
        return {"ok": True, "text": text, "taken": len(text)}
    if key == "T":
        game.ct_box = True
        return {"ok": True, "key": "T", "vk": ord("T"), "taken": True}
    if key == "Esc" and game.ct_box:
        game.ct_box = False
        game.ct_shut_at = None

        def shut():
            b.update(state=0, request=0, holds="shut", page=False)
            _line(game, "ours -> shut (state 0, request 0, our close asked)")
        game.later(2, shut)
        return {"ok": True, "key": "Esc", "vk": 0x1B, "taken": True}
    return None
