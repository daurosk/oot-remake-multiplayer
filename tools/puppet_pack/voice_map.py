"""Rules for the Character Select voices that the puppet pack builds on the player's PC.

Each pickable character speaks with clips the game already has for that character (or a fitting or
default voice type), never with Link's. Nothing here holds sound: only names, lengths and loudness that
voices.py measures in the player's own game files. check() is the gate: the build stops when a clip
breaks a rule, so a wrong mapping never reaches a game.

The rules (user decisions, 2026-11-23):
- Every one of Link's voice clips gets a stand-in for every character but Link. None is left silent and
  none keeps Link's voice.
- Frequent events (sword swings, rolls, jumps, climbs, hits taken, lifting) are no longer than Link's
  longest clip for that event plus MARGIN_S.
- Loudness matches Link's clips for the same event (their median RMS, within LOUDER_DB louder or
  QUIETER_DB quieter).
- Only short, wordless clip kinds: no laughs, cries or speech lines (KINDS lists the allowed ones).
- An event gets as many clips as Link has for it, so the game rotates them as it does Link's.
- People fall back to a neutral voice of their sex and age; creatures fall back to a creature voice.
- An event takes its own class from any voice of the chain before another class (2026-11-23); a clip
  longer than the cap is cut around its loudest part.
- The cow (2026-11-23): its roar for big attacks, its angry moo for attacks, its hurt moo for hits, its sad moo
  for death (ATTACK_BIG and DIE, classes only the cow's clips have; for everyone else they stay empty).
"""
import re
from dataclasses import dataclass

MARGIN_S = 0.15
LOUDER_DB = 1.0
QUIETER_DB = 3.0
LINK = "Link"
# Every Link model in the mod's characters.json (pickable or not): they keep Link's own clips (decision 2026-11-23).
LINK_VARIANTS = frozenset({"Link", "Retro Link", "Link (opening)", "Link (adult)", "Dark Link"})

# Link's events (the part of Vo_Pl<Age>_<event>[_NN] before the number) that play often in play.
FREQUENT = frozenset("""
SwordAttack_S SwordAttack_M SwordAttack_B BigSwordAttack_S BigSwordAttack_M BigSwordAttack_B
JumpAttack BS_JumpAttack H_JumpAttack RotaryCut_S RotaryCut_B RollAttack SideStep BackFlip Jump_S Jump_B
JumpCliffs Climb ClimbJump ClimbSt ReachingTop GrabFall GrabJump Damage_S Damage_M Damage_B
RollAttackDamage_S RollAttackDamage_B lifting Swing ThrowingBom ThrowingNuts Shield_S Shield_B HookShot
Hammer Boomerang BoomerangC ZeroBullet ZeroTool
""".split())

# Wordless clip kinds by class. An NPC clip's kind is the last part of NV_<type>_<kind><NN>; an enemy
# archive's clip is classed by ENEMY_RULES on its asset name.
KINDS = {
    "ATTACK": frozenset("WeaponSwingS AttackS AttackL AttackShot DashAttack FeintAttack FeintAttackB FeintAttackC "
                        "ThrowMagicBall ThrowMagicBallL ReflectMagicBall Throw Yell".split()),
    "EFFORT": frozenset("Jump JumpEd SideStep LiftedupR ThrownR Guard Dig Scoop AttackSCarge AttackLCharge AttackCharge "
                        "DashAttackCharge ChargeMagicBallL ActiveBarrier ActiveBarrierB ActiveBarrierC Recovery".split()),
    "DAMAGE": frozenset("Painful Damage SwordDamage MagicBallDamage LightArrowDamage LightArrowSecondDamage Collide "
                        "Stunned Rebounded Unconscious ReflectDamage Down Die".split()),
    "SURPRISE": frozenset("SurpriseS SurpriseM SurpriseL Surprise SurpriseSi PanicSurprise Notice Scare".split()),
    "BREATH": frozenset("Ease Stretch Wakep Sigh Tired Run Cough".split()),
}
CLASSES = tuple(KINDS) + ("ATTACK_BIG", "DIE")

ENEMY_RULES = [
    ("DAMAGE", r"(DamageVoice|Voice_Damage|StunVoice|DamagedAngry|Voice_AfterDamage|Body_Damage|DeadVoice|DieVoice)"),
    ("ATTACK", r"(AttackVoice(?!_Delay)|Voice_Attack\d|WeaponAttack\w*Voice|AttackSuccessVoice|AttackEndVoice|"
               r"Voice_SwordAttack_[SMB]|Voice_JumpAttack|Voice_ChargeAttack|Voice_RollAttack|Voice_SwordCharge|"
               r"WeaponDoubleAttackVoice|NV_Cow_Angry)"),
    ("EFFORT", r"(JumpVoice|Voice_Jump\d|LandVoice|DodgeVoice|Voice_SideStep|Voice_BackFlip|Voice_Shield|GuardVoice|"
               r"Voice_Guard\d|Voice_Turn|ProvokeVoice|RepelledVoice|RepelledLongVoice|ParryVoice|FaintEndVoice|"
               r"StunEndVoice|RecoveryVoice|MissingVoice)"),
    ("SURPRISE", r"(NoticeVoice|AppearVoice|DiscoverVoice|FindVoice|Voice_AttackPre|PreAttackVoice)"),
    ("ATTACK_BIG", r"SE_Animal_Cow_Roar_"),
    ("DIE", r"NV_Cow_Sad"),
    ("BREATH", r"(WaitVoice|IdleVoice|BreathVoice|Voice_Wait|NV_Cow_(?!Angry))"),
]

# The class Link's other events take, first match wins; anything left (Wait, cutscene lines, Nod) is BREATH.
_EVENT_RULES = [
    ("DAMAGE", r"Damage|Death|Dying|Flail|Scream|WaterInBubble|liftingImpossible|StandDesperately|FallSuddenly|"
               r"EscapedBreath|HeavyBbreathing|Electric|Poison"),
    ("ATTACK", r"Attack|Swing|Hammer|Throw|Boomerang|Bullet|ZeroTool|ZeroMasterSword|Sword_O|RotaryCut|Nuts|Bom|"
               r"Shoot|HookShot|Spurs"),
    ("EFFORT", r"Jump|Climb|Push|lifting|LiftUp|Grab|Crawl|Horse|Shoes|Plug|Release|Squat|StandUp|Emerge|Diving|"
               r"ReachingTop|RC_Store|Charge|Block|Shield|SideStep|BackFlip|TreasureBox|TresureBox|SlidingSlope|"
               r"WaveArms|Return\d"),
    ("SURPRISE", r"Surprised|WakeUp|Dm_|Npc_Dream"),
]

# Preference order of classes per class of Link's event: the first class a voice of the chain has fills the slot.
# ATTACK_BIG and DIE have only the cow's clips; for everyone else ATTACK_BIG and DIE events fall to ATTACK and DAMAGE.
PREFS = {
    "ATTACK_BIG": ("ATTACK_BIG", "ATTACK", "EFFORT", "SURPRISE", "DAMAGE", "BREATH", "DIE"),
    "ATTACK": ("ATTACK", "ATTACK_BIG", "EFFORT", "SURPRISE", "DAMAGE", "BREATH", "DIE"),
    "EFFORT": ("EFFORT", "ATTACK", "SURPRISE", "BREATH", "DAMAGE", "ATTACK_BIG", "DIE"),
    "DIE": ("DIE", "DAMAGE", "SURPRISE", "EFFORT", "BREATH", "ATTACK", "ATTACK_BIG"),
    "DAMAGE": ("DAMAGE", "SURPRISE", "EFFORT", "BREATH", "ATTACK", "DIE", "ATTACK_BIG"),
    "SURPRISE": ("SURPRISE", "BREATH", "EFFORT", "DAMAGE", "ATTACK", "ATTACK_BIG", "DIE"),
    "BREATH": ("BREATH", "SURPRISE", "EFFORT", "DAMAGE", "ATTACK", "ATTACK_BIG", "DIE"),
}
_SHIELD = ("EFFORT", "SURPRISE", "ATTACK", "BREATH", "DAMAGE", "ATTACK_BIG", "DIE")
# Link's events that try the cow's own classes first (their event_class stays ATTACK and DAMAGE).
_BIG_ATTACK = re.compile(r"^(SwordAttack_B|BigSwordAttack_B|JumpAttack|BS_JumpAttack|H_JumpAttack|RotaryCut_B)$")
_DIE = re.compile(r"^(Death|WaterDeath)$")


def event_of(link_name):
    """'Vo_PlAdult_SwordAttack_S_03' -> 'SwordAttack_S'; None for a name that is not one of Link's voice clips."""
    m = re.match(r"Vo_Pl(?:Child|Adult)_(.+?)(?:_(\d+))?$", link_name)
    return m.group(1) if m else None


def event_class(event):
    """The class of clip that stands in for one of Link's events."""
    for c, rx in _EVENT_RULES:
        if re.search(rx, event):
            return c
    return "BREATH"


def prefs(event):
    """Classes to try for one of Link's events, best first; every class is in it, so no slot stays empty."""
    if event.startswith("Shield_"):
        return _SHIELD
    if _BIG_ATTACK.match(event):
        return PREFS["ATTACK_BIG"]
    if _DIE.match(event):
        return PREFS["DIE"]
    return PREFS[event_class(event)]


def own(event):
    """The event's own classes: the ones taken from any voice of the chain before another class is tried. Its
    event_class, after the cow's ATTACK_BIG or DIE where prefs(event) puts one first."""
    p = prefs(event)
    c = "EFFORT" if p is _SHIELD else event_class(event)
    return p[:p.index(c) + 1]


def kind_of(clip_name):
    """'NV_Biggoron_SurpriseS02' -> 'SurpriseS'; 'NV_X_ApothecaryOldWoman_Sigh00' -> 'Sigh'."""
    m = re.match(r"NV_.+_([A-Za-z]+?)\d*$", clip_name)
    return m.group(1) if m else None


def clip_class(clip_name):
    """The class of a game clip, or None when it may not be used (speech, laughs, cries, unknown)."""
    if clip_name.startswith("NV_") and not clip_name.startswith("NV_Cow_"):
        if "_Dm_" in clip_name or clip_name.endswith("_Sp") or re.search(r"_Sp\d*$", clip_name):
            return None
        k = kind_of(clip_name)
        for c, s in KINDS.items():
            if k in s:
                return c
        return None
    if "Delay" in clip_name or "Body_Attack" in clip_name:
        return None
    for c, rx in ENEMY_RULES:
        if re.search(rx, clip_name):
            return c
    return None


def cap_s(event, link_longest):
    """The longest a clip may be for this event, or None when the event has no cap."""
    return link_longest + MARGIN_S if event in FREQUENT else None


@dataclass
class Clip:
    voice: str           # voice type, e.g. 'Custom_Saria' or 'Hylia_Woman_1'
    age: str             # 'Child' or 'Adult'
    link_name: str       # Link's asset name this clip stands in for (its rotation slot)
    source: str          # the game's clip it was made from, e.g. 'NV_Custom_Saria_SurpriseS00'
    length: float        # seconds, after trimming
    link_longest: float  # seconds, Link's longest clip for the same event
    rms_db: float        # loudness after the gain
    link_rms_db: float   # loudness of Link's clips for the same event (median)


def check_clips(clips, link_names):
    """Problems in the built clips, one line each; an empty list means every rule holds.
    link_names: {age: set of Link's voice clip names}; each voice must stand in for every one of them."""
    bad = []
    seen = {}
    for c in clips:
        ev = event_of(c.link_name)
        where = "%s %s %s (%s)" % (c.voice, c.age, c.link_name, c.source)
        if ev is None or c.link_name not in link_names.get(c.age, ()):
            bad.append("%s: not one of Link's voice clips" % where)
            continue
        seen.setdefault((c.voice, c.age), set()).add(c.link_name)
        if c.source.startswith("Vo_Pl") or is_link_voice(c.source):
            bad.append("%s: Link's own voice" % where)
        elif is_link_voice(c.voice):
            bad.append("%s: an archive made from Link's voice" % where)
        elif clip_class(c.source) is None:
            bad.append("%s: clip kind not allowed (speech, laugh, cry or unknown)" % where)
        cap = cap_s(ev, c.link_longest)
        if cap is not None and c.length > cap + 1e-6:
            bad.append("%s: %.2f s is longer than Link's %.2f s + %.2f s" % (where, c.length, c.link_longest, MARGIN_S))
        if c.rms_db > c.link_rms_db + LOUDER_DB:
            bad.append("%s: %.1f dB louder than Link's" % (where, c.rms_db - c.link_rms_db))
        if c.rms_db < c.link_rms_db - QUIETER_DB:
            bad.append("%s: %.1f dB quieter than Link's" % (where, c.link_rms_db - c.rms_db))
    for (voice, age), names in sorted(seen.items()):
        missing = sorted(link_names[age] - names)
        if missing:
            bad.append("%s %s: %d of Link's clips have no stand-in (would play Link's), e.g. %s"
                       % (voice, age, len(missing), missing[0]))
    return bad


@dataclass
class Character:
    name: str
    chain: tuple         # voice types, first fills, later ones only where the first cannot
    default: str         # neutral voice of the same sex and age, or a creature voice for a creature
    creature: bool = False


def is_link_voice(voice):
    """True for Link's own clips and any voice made from them (the player's banks, Dark Link's)."""
    return voice == LINK or "Link" in voice or voice.startswith(("Player", "Vo_Pl"))


def check_map(characters, voices, creature_voices):
    """Problems in the character -> voice table. voices: the voice types that were built (each with at
    least one usable clip); creature_voices: the ones that are creature sounds. A chain voice with no usable
    clip is passed over; the default must have clips, so every character has a voice."""
    bad = []
    for ch in characters:
        if not ch.chain and ch.default == LINK:
            if ch.name not in LINK_VARIANTS:
                bad.append("%s: keeps Link's voice but is not one of Link's models" % ch.name)
            continue   # Link's models: Link's own clips
        near = [v for v in ch.chain + (ch.default,) if is_link_voice(v)]
        if near:
            bad.append("%s: keeps Link's voice (%s)" % (ch.name, ", ".join(near)))
        elif ch.default not in voices:
            bad.append("%s: default voice %s was not built" % (ch.name, ch.default))
        if ch.creature and ch.default not in creature_voices:
            bad.append("%s: a creature with the person's voice %s as its default" % (ch.name, ch.default))
        if not ch.creature and ch.default in creature_voices:
            bad.append("%s: a person with the creature voice %s as its default" % (ch.name, ch.default))
    return bad


def check(characters, voices, creature_voices, clips, link_names):
    """Raise ValueError naming every broken rule; the build stops there."""
    bad = check_map(characters, voices, creature_voices) + check_clips(clips, link_names)
    if bad:
        raise ValueError("Character Select voices break a rule:\n  " + "\n  ".join(bad))
