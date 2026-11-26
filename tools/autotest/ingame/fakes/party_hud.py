"""Fake-game reactions of test_party_hud.py (party-hud-crash-c126): the party HUD's module lines, only on a game whose
boot has COOP_TEST_PARTY_HUD=1 (B as without the pack with B:COOP_TEST_PARTY_HUD_NO_PACK=1). The module makes no
run-time clone (party-hud-crash-c126 88f63dfb1): with the pack the HUD is on, without it hidden. BT_FAKE_PARTY_CRASH=<A|B>
(selftest only): that game logs the game's crash report in its boot's load, as the real crash did.

Every line is the module's own, from its source:
"""
import os

HUD_ON = "coop: party hud: on the game's HUD"                                  # hookshot/src/coop_hookshot.cpp PartyStep
HUD_HIDDEN = "coop: party hud: hidden (the game's HUD has none of ours now; nothing drawn instead)"   # (the same)
HUD_GAUGES_OFF = ("coop: party hud: run-time gauges are off (the game's part clone crashes outside a screen's own "
                  "set-up): players past the pack's slots go to \"+N\"")      # hookshot/src/coop_party_hud.cpp Hud::Find
HUD_NO_PACK = "coop: party hud: test: the pack's panes are not looked for (no pack)"                  # (the same)
HUD_B_HIDDEN = ("coop: party hud: the HUD has no hearts of ours (an older pack): the other players' hearts are hidden "
                "(nothing drawn instead)")                                                         # (the same)
# The game's own crash report (offline/lib/game-crash.ps1's source lines; the f3137026 run's game-out-A.txt :6081).
CRASH = "CRASH: exception 0xc0000005 (read 0xfff43520028, guest 0xffffffff43520028) at longshot.exe+0xdff25ca"


def _found(game):
    """Hud::Find's lines once the status screen is live after a load."""
    if game.party_no_pack:
        game.say(HUD_NO_PACK)
    game.say(HUD_GAUGES_OFF)
    if game.party_no_pack:
        game.say(HUD_B_HIDDEN)
    else:
        game.say(HUD_ON)


def setup(game):
    game.party = game.env_on("COOP_TEST_PARTY_HUD=1")
    game.party_no_pack = game.env_on("COOP_TEST_PARTY_HUD_NO_PACK=1")
    game.party_title = False
    if not game.party:
        return
    if os.environ.get("BT_FAKE_PARTY_CRASH") == game.tag:
        game.say(CRASH)
        return
    _found(game)   # (the boot's save load)


def after(game, m, r):
    """The title and the load after it, as the module logs them (Step: hidden while no live status screen)."""
    if not getattr(game, "party", False):
        return
    if game.at_title and not game.party_title:
        game.party_title = True
        if not game.party_no_pack:
            game.say(HUD_HIDDEN)
    elif not game.at_title and game.party_title:
        game.party_title = False
        _found(game)
