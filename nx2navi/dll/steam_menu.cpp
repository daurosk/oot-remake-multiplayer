// SPDX-License-Identifier: GPL-3.0-or-later
#include "steam_menu.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

#include "overlay_model.hpp"
#include "steam_model.hpp"

namespace CoopNx2navi::Menu::SteamPages {
namespace {

struct Words {
    const char* en;
    const char* fr;
};
const char* W(Lang lang, const Words& w) {
    return lang == Lang::fr ? w.fr : w.en;
}

// Captions: two lines of at most 46 characters (the game's box does not wrap).
constexpr Words kHostSteam{"Host over Steam", "Héberger sur Steam"};
constexpr Words kJoinSteam{"Join over Steam", "Rejoindre via Steam"};
constexpr Words kOpen{"Open", "Ouvrir"};
constexpr Words kUnavailable{"Unavailable", "Indisponible"};
constexpr Words kHostEntryWhy{"Makes a Steam room: friends join with its ID\nor an invitation. No port to open.",
    "Crée un salon Steam : vos amis le rejoignent\npar son ID ou sur invitation. Aucun port."};
constexpr Words kJoinEntryWhy{"Joins a friend's Steam room with its ID or\ntheir invitation. You play on their save.",
    "Rejoint le salon Steam d'un ami par son ID\nou son invitation, sur sa sauvegarde."};
constexpr Words kWhyOldGame{"Not available yet:\nupdate NX2navi (Steam for mods).",
    "Pas encore disponible :\nmettez NX2navi à jour (Steam pour les mods)."};
constexpr Words kWhyChecking{"Signing in to Steam...", "Connexion à Steam..."};
constexpr Words kWhyNoLibrary{"steam_api64.dll is not beside the game:\nthe launcher copies it from a Steam game.",
    "steam_api64.dll manque à côté du jeu : le\nlanceur le copie d'un jeu Steam installé."};
constexpr Words kWhyOldLibrary{"The steam_api64.dll beside the game is too\nold: delete it, the launcher finds another.",
    "Le steam_api64.dll à côté du jeu est trop\nancien : supprimez-le, le lanceur en copie un."};
constexpr Words kWhyNoClient{"Steam is not running, or not signed in:\nstart Steam, then come back to this page.",
    "Steam n'est pas lancé ou pas connecté :\nlancez Steam, puis revenez sur cette page."};
constexpr Words kWhyGameCoop{"NX2navi's own co-op is on and uses Steam:\nturn it off (the menu's Co-op tab).",
    "Le co-op de NX2navi est activé et utilise\nSteam : coupez-le (onglet Co-op du menu)."};
constexpr Words kWhyInUse{"Another mod is using Steam.", "Un autre mod utilise Steam."};
constexpr Words kWhyOther{"Steam cannot be used\n(coop-nx2navi.log says why).",
    "Steam est inutilisable\n(coop-nx2navi.log dit pourquoi)."};

constexpr Words kName{"Name", "Nom"};
constexpr Words kNameWhy{"Your name for the other players: your Steam\nname, unless you type another (16 at most).",
    "Votre nom pour les autres joueurs : celui de\nSteam, ou tapez-en un autre (16 au plus)."};
constexpr Words kEditHelp{"Type, Enter confirms, Esc cancels.\nPad: up/down, right adds, left deletes, A.",
    "Tapez, Entrée valide, Échap annule.\nManette : haut/bas, droite, gauche, A valide."};
constexpr Words kBadName{"The name needs 1 to 16 characters.", "Le nom doit avoir 1 à 16 caractères."};
constexpr Words kRoom{"Room ID", "ID du salon"};
constexpr Words kRoomLater{"The room is made when the session starts;\nits ID then shows here, to give to friends.",
    "Le salon est créé au démarrage de la session ;\nson ID s'affiche alors ici, pour vos amis."};
constexpr Words kRoomHow{"Your room's ID: friends type it to join.\nChoose it: copies it.",
    "L'ID de votre salon : vos amis le tapent pour\nrejoindre. Valider : le copie."};
constexpr Words kRoomCopied{"Copied: paste it to your friends.", "Copié : collez-le à vos amis."};
constexpr Words kRoomNotCopied{"The clipboard refused it: read it out.", "Copie refusée : dictez-le à vos amis."};
constexpr Words kInvite{"Invite a friend", "Inviter un ami"};
constexpr Words kInviteLater{"Once the session runs: your Steam friends,\nto send one an invitation.",
    "Une fois la session lancée : vos amis Steam,\npour envoyer une invitation à l'un d'eux."};
constexpr Words kInviteWhy{"Your Steam friends: choose one to send\nthem an invitation to this room.",
    "Vos amis Steam : choisissez-en un pour lui\nenvoyer une invitation dans ce salon."};
constexpr Words kPlayers{"Players", "Joueurs"};
constexpr Words kPlayersNone{"Nobody yet: the players of your room\nare listed here.",
    "Personne pour l'instant : les joueurs de\nvotre salon s'affichent ici."};
constexpr Words kSave{"Save", "Sauvegarde"};
constexpr Words kChoose{"Choose", "Choisir"};
constexpr Words kYourOwn{"Your own", "La vôtre"};
constexpr Words kSaveRowWhy{"The multiplayer save this game is played on.\nYour solo saves are never written.",
    "La sauvegarde multijoueur de cette partie.\nVos sauvegardes solo ne sont jamais modifiées."};
constexpr Words kSavesOffWhy{"This NX2navi cannot change saves in the game:\na host plays on its own saves.",
    "Ce NX2navi ne sait pas changer de sauvegarde :\nun hôte joue sur les siennes."};
constexpr Words kStart{"Start", "Démarrer"};
constexpr Words kStartWhy{"The game restarts on the chosen save, then\nmakes the Steam room and the session.",
    "Le jeu redémarre sur la sauvegarde choisie,\npuis crée le salon Steam et la session."};
constexpr Words kStartOwnWhy{"Makes the Steam room and the session.\nThen choose Continue to load your save.",
    "Crée le salon Steam et la session.\nChoisissez ensuite Continuer."};
constexpr Words kHostingWhy{"The session runs over Steam: give the room\nID above, or invite a friend.",
    "La session tourne sur Steam : donnez l'ID du\nsalon ci-dessus, ou invitez un ami."};
constexpr Words kNoSaveChosen{"Choose a multiplayer save first\n(the Save row above).",
    "Choisissez d'abord une sauvegarde multijoueur\n(ligne Sauvegarde ci-dessus)."};
constexpr Words kAlreadyRunning{"A session already runs.\nRestart the game for another one.",
    "Une session tourne déjà.\nRedémarrez le jeu pour une autre."};
constexpr Words kBack{"Back", "Retour"};
constexpr Words kBackWhy{"Back to the Multiplayer page.", "Retour à la page Multijoueur."};
constexpr Words kBackHostWhy{"Back to Host over Steam.", "Retour à Héberger sur Steam."};
constexpr Words kBackJoinWhy{"Back to Join over Steam.", "Retour à Rejoindre via Steam."};

constexpr Words kRoomJoinWhy{"The host's room ID, eight letters and digits\nsuch as ABCD-2345 (they see it on their page).",
    "L'ID du salon de l'hôte, huit lettres et\nchiffres comme ABCD-2345 (affiché chez lui)."};
constexpr Words kBadRoom{"Not a room ID: eight letters and digits,\nsuch as ABCD-2345 (no 0, 1, I or O).",
    "Ce n'est pas un ID de salon : huit lettres\net chiffres comme ABCD-2345 (sans 0, 1, I, O)."};
constexpr Words kNoRoom{"Type or paste the host's room ID first,\nor choose an invitation.",
    "Tapez ou collez d'abord l'ID du salon de\nl'hôte, ou choisissez une invitation."};
constexpr Words kPaste{"Paste", "Coller"};
constexpr Words kPasteWhy{"Takes the room ID you copied\n(from a message of the host's).",
    "Reprend l'ID de salon que vous avez copié\n(depuis un message de l'hôte)."};
constexpr Words kPasted{"Pasted: now choose Join.", "Collé : choisissez maintenant Rejoindre."};
constexpr Words kNothingToPaste{"The clipboard holds no room ID\n(eight letters and digits, like ABCD-2345).",
    "Le presse-papiers ne contient pas d'ID de\nsalon (huit lettres et chiffres)."};
constexpr Words kInvitations{"Invitations", "Invitations"};
constexpr Words kNone{"none", "aucune"};
constexpr Words kInvitationsWhy{"Steam invitations to a room, received while\nthis menu is open. Choose one to join.",
    "Invitations Steam à un salon, reçues pendant\nque ce menu est ouvert. Valider : rejoindre."};
constexpr Words kInvitationsNone{"None yet. A friend who hosts can invite you\nwhile this menu is open, or give the room ID.",
    "Aucune pour l'instant. Un ami qui héberge peut\nvous inviter (menu ouvert) ou donner son ID."};
constexpr Words kHostsSave{"The host's", "Celle de l'hôte"};
constexpr Words kHostsSaveWhy{"Received from the host and kept with your\nmultiplayer saves. Your solo saves stay.",
    "Reçue de l'hôte et rangée dans vos sauvegardes\nmultijoueur. Vos sauvegardes solo restent."};
constexpr Words kJoin{"Join", "Rejoindre"};
constexpr Words kJoinWhy{"Takes the host's save, then the game restarts\non it. Your own saves are not touched.",
    "Reçoit la sauvegarde de l'hôte, puis le jeu\nredémarre dessus. Les vôtres ne changent pas."};

constexpr Words kFriends{"Invite a friend", "Inviter un ami"};
constexpr Words kInApp{"In the game", "En jeu"};
constexpr Words kOnline{"Online", "En ligne"};
constexpr Words kOffline{"Offline", "Hors ligne"};
constexpr Words kFriendWhy{"Choose: sends a Steam invitation. It shows\nin their game on its Join over Steam page.",
    "Valider : envoie une invitation Steam, visible\ndans son jeu, page Rejoindre via Steam."};
constexpr Words kFriendAwayWhy{"Not in the game: give them the room ID, or\ninvite them once their game is open.",
    "Pas en jeu : donnez-lui l'ID du salon, ou\ninvitez-le quand son jeu sera ouvert."};
constexpr Words kNoFriendRow{"No other friend.", "Pas d'autre ami."};
constexpr Words kNoFriends{"Steam lists no friend for this account.", "Steam ne liste aucun ami pour ce compte."};
constexpr Words kMoreFriends{"More friends", "Autres amis"};
constexpr Words kMoreFriendsWhy{"Shows the next friends.", "Affiche les amis suivants."};
constexpr Words kInvitedOpen{"Invitation sent to ", "Invitation envoyée à "};
constexpr Words kNotInvited{"Steam did not take the invitation.", "Steam n'a pas pris l'invitation."};
constexpr Words kInviteRowWhy{"Choose: joins this friend's room\n(their save, then the game restarts).",
    "Valider : rejoint le salon de cet ami\n(sa sauvegarde, puis le jeu redémarre)."};
constexpr Words kNoInviteRow{"No other invitation.", "Pas d'autre invitation."};
constexpr Words kMoreInvites{"More invitations", "Autres invitations"};
constexpr Words kMoreInvitesWhy{"Shows the next invitations.", "Affiche les invitations suivantes."};
constexpr Words kAFriend{"A friend", "Un ami"};
constexpr Words kReceived{"Received", "Reçues"};

constexpr Words kPhaseServer{"Starting...", "Démarrage..."};
constexpr Words kPhaseConnecting{"Connecting...", "Connexion..."};
constexpr Words kPhaseSave{"Receiving...", "Réception..."};
constexpr Words kPhaseRestart{"Restarting...", "Redémarrage..."};
constexpr Words kPhaseHosting{"Hosting", "Hébergement"};
constexpr Words kPhaseJoined{"Connected", "Connecté"};
constexpr Words kPhaseFailed{"Failed", "Échec"};

constexpr Words kFails[] = {
    /* none */ {"", ""},
    /* unavailable */ {"Steam cannot be used now\n(the Multiplayer page says why).", "Steam est inutilisable pour l'instant\n(la page Multijoueur dit pourquoi)."},
    /* create */ {"Steam did not make the room:\ntry again in a moment.", "Steam n'a pas créé le salon :\nréessayez dans un instant."},
    /* no_room */ {"No room has this ID (any more):\ncheck it with the host.", "Aucun salon n'a (plus) cet ID :\nvérifiez-le avec l'hôte."},
    /* full */ {"The room is full.", "Le salon est complet."},
    /* refused */ {"Steam does not let you into this room:\nask the host for an invitation.", "Steam ne vous laisse pas entrer dans ce\nsalon : demandez une invitation à l'hôte."},
    /* not_ours */ {"This Steam room is not a room of this mod\n(another program's, or NX2navi's co-op).", "Ce salon Steam n'est pas un salon de ce mod\n(autre programme, ou co-op de NX2navi)."},
    /* host_silent */ {"The room's host does not answer:\nis their session still running?", "L'hôte du salon ne répond pas :\nsa session tourne-t-elle encore ?"},
    /* version */ {"The host has another version of the mod:\nupdate both to the same one.", "L'hôte a une autre version du mod :\nmettez les deux à la même version."},
    /* lost */ {"The Steam room is gone: the host left,\nor Steam lost its connection.", "Le salon Steam a disparu : l'hôte est parti,\nou Steam a perdu sa connexion."},
    /* server */ {"The session's server did not start\n(coop-nx2navi.log says why).", "Le serveur de la session n'a pas démarré\n(coop-nx2navi.log dit pourquoi)."},
    /* other */ {"Steam did not let the room be entered\n(coop-nx2navi.log says why).", "Steam n'a pas permis d'entrer dans le salon\n(coop-nx2navi.log dit pourquoi)."},
};

bool Busy(Phase phase) {
    return phase != Phase::idle && phase != Phase::failed;
}

Row Button(const Words& label, std::string value, std::string description, Lang lang) {
    Row r;
    r.label = W(lang, label);
    r.value = std::move(value);
    r.description = std::move(description);
    return r;
}

// A caption line that names something of any length: cut to the box's width.
std::string Line(std::string text) {
    while (Ui::Utf8Count(text) > 46) {
        Ui::Utf8PopBack(text);
    }
    return text;
}

std::string PhaseWord(const State& s) {
    switch (s.phase) {
    case Phase::starting_server: return W(s.lang, kPhaseServer);
    case Phase::connecting: return W(s.lang, kPhaseConnecting);
    case Phase::receiving_save: return W(s.lang, kPhaseSave);
    case Phase::restarting: return W(s.lang, kPhaseRestart);
    case Phase::hosting: return W(s.lang, kPhaseHosting);
    case Phase::joined: return W(s.lang, kPhaseJoined);
    default: return W(s.lang, kPhaseFailed);
    }
}

Row TextRow(const State& s, Field field, const Words& label, const Words& why) {
    const bool typing = s.editing == field;
    const bool game_typing = s.game_typing && s.editing == Field::none && !Busy(s.phase);
    std::string value = typing ? s.edit + "_" : field == Field::steam_name ? PlayerName(s) : s.steam.room;
    if (value.empty() && !game_typing) {
        value = "-";
    }
    std::string description = typing ? W(s.lang, kEditHelp) : W(s.lang, why);
    if (!typing && !s.message.empty() && s.phase != Phase::failed) {
        description = s.message;  // a refused text's rule, until the next change
    }
    Row r = Button(label, std::move(value), std::move(description), s.lang);
    r.kind = RowKind::text;
    r.text_max = game_typing ? (field == Field::steam_name ? static_cast<std::uint32_t>(kMaxName * 4) : 16u) : 0;
    return r;
}

Row GoRow(const State& s, const Words& label, const Words& why) {
    return Button(label, Busy(s.phase) || s.phase == Phase::failed ? PhaseWord(s) : W(s.lang, label),
                  s.message.empty() ? W(s.lang, why) : s.message, s.lang);
}

const SaveInfo* Chosen(const State& s) {
    return FindSave(s, s.save);
}

bool Hosting(const State& s) {
    return !s.steam.hosted_room.empty();
}

// "Zelda, Saria, Mido" on two lines of the caption box.
std::string Names(const std::vector<std::string>& names) {
    std::string lines[2];
    std::size_t at = 0;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string piece = names[i] + (i + 1 < names.size() ? ", " : "");
        if (Ui::Utf8Count(lines[at]) + Ui::Utf8Count(piece) > 46 && at == 0 && !lines[0].empty()) {
            at = 1;
        }
        lines[at] += piece;
    }
    return lines[1].empty() ? Line(lines[0]) : Line(lines[0]) + "\n" + Line(lines[1]);
}

std::string Range(std::size_t first, std::size_t count) {
    if (count <= kSteamListed) {
        return {};
    }
    return std::to_string(first + 1) + "-" + std::to_string(std::min(first + kSteamListed, count)) + " / " +
           std::to_string(count);
}

Row Entry(const State& s, bool host) {
    const SteamAvail avail = s.steam.avail;
    const bool usable = avail == SteamAvail::ready;
    Row r = Button(host ? kHostSteam : kJoinSteam, W(s.lang, usable ? kOpen : kUnavailable),
                   usable ? W(s.lang, host ? kHostEntryWhy : kJoinEntryWhy) : WhyUnavailable(s.lang, avail),
                   s.lang);
    r.disabled = !usable && s.dim_steam;
    return r;
}

void ClearIfIdle(State& s) {
    if (!Busy(s.phase)) {
        s.message.clear();
        s.phase = Phase::idle;
    }
}

Action Go(State& s, bool host) {
    if (Busy(s.phase)) {
        if (s.phase == Phase::hosting || s.phase == Phase::joined) {
            s.message = W(s.lang, kAlreadyRunning);
            return Action::redraw;
        }
        return Action::none;  // on its way
    }
    const char* why = nullptr;
    if (s.steam.avail != SteamAvail::ready) {
        why = WhyUnavailable(s.lang, s.steam.avail);
    } else if (!ValidName(PlayerName(s))) {
        why = W(s.lang, kBadName);
    } else if (host && s.saves_on && Chosen(s) == nullptr) {
        why = W(s.lang, kNoSaveChosen);  // never on the player's own saves
    } else if (!host && s.steam.room.empty()) {
        why = W(s.lang, kNoRoom);
    }
    if (why != nullptr) {
        s.phase = Phase::failed;
        s.message = why;
        return Action::redraw;
    }
    s.message.clear();
    s.steam.note.clear();
    if (host) {
        return Action::start_steam_host;
    }
    s.steam.join_room = s.steam.room;
    return Action::start_steam_join;
}

}  // namespace

bool Is(Page page) {
    return page == Page::steam_host || page == Page::steam_join || List(page);
}

bool List(Page page) {
    return page == Page::steam_friends || page == Page::steam_invites;
}

Row HostEntry(const State& s) {
    return Entry(s, true);
}

Row JoinEntry(const State& s) {
    return Entry(s, false);
}

Action Open(State& s, bool host) {
    if (s.steam.avail != SteamAvail::ready && s.steam.avail != SteamAvail::checking) {
        return Action::none;
    }
    s.page = host ? Page::steam_host : Page::steam_join;
    s.focus = 1;
    s.steam.note.clear();
    ClearIfIdle(s);
    return Action::steam_open;
}

std::string Title(const State& s) {
    switch (s.page) {
    case Page::steam_host: return W(s.lang, kHostSteam);
    case Page::steam_join: return W(s.lang, kJoinSteam);
    case Page::steam_friends: return W(s.lang, kFriends);
    default: return W(s.lang, kInvitations);
    }
}

std::string PlayerName(const State& s) {
    if (!s.steam.own_name.empty()) {
        return s.steam.own_name;
    }
    return s.steam.name.empty() ? s.name : s.steam.name;
}

std::vector<Row> Rows(const State& s) {
    std::vector<Row> rows;
    rows.reserve(kRows);
    Row separator;
    separator.kind = RowKind::separator;
    const SteamState& st = s.steam;
    if (s.page == Page::steam_host) {
        rows.push_back(TextRow(s, Field::steam_name, kName, kNameWhy));
        Row room = Button(kRoom, Hosting(s) ? st.hosted_room : "-",
                          !Hosting(s)        ? W(s.lang, kRoomLater)
                          : !st.note.empty() ? st.note
                                             : W(s.lang, kRoomHow),
                          s.lang);
        room.disabled = !Hosting(s) && s.dim_steam;
        rows.push_back(std::move(room));
        Row invite = Button(kInvite, Hosting(s) ? std::to_string(st.friends.size()) : "-",
                            W(s.lang, Hosting(s) ? kInviteWhy : kInviteLater), s.lang);
        invite.disabled = !Hosting(s) && s.dim_steam;
        rows.push_back(std::move(invite));
        Row players = Button(kPlayers, Hosting(s) ? std::to_string(st.members.size()) : "-",
                             st.members.empty() ? std::string(W(s.lang, kPlayersNone)) : Names(st.members), s.lang);
        players.disabled = !Hosting(s) && s.dim_steam;
        rows.push_back(std::move(players));
        const SaveInfo* chosen = Chosen(s);
        rows.push_back(Button(kSave,
                              !s.saves_on ? W(s.lang, kYourOwn)
                              : chosen    ? SaveLabel(*chosen)
                                          : W(s.lang, kChoose),
                              W(s.lang, s.saves_on ? kSaveRowWhy : kSavesOffWhy), s.lang));
        rows.push_back(separator);
        rows.push_back(GoRow(s, kStart, Hosting(s) ? kHostingWhy : s.saves_on ? kStartWhy : kStartOwnWhy));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackWhy), s.lang));
    } else if (s.page == Page::steam_join) {
        rows.push_back(TextRow(s, Field::steam_name, kName, kNameWhy));
        rows.push_back(TextRow(s, Field::room, kRoom, kRoomJoinWhy));
        rows.push_back(Button(kPaste, W(s.lang, kPaste), st.note.empty() ? W(s.lang, kPasteWhy) : st.note, s.lang));
        Row invitations = Button(kInvitations,
                                 st.invites.empty() ? W(s.lang, kNone) : std::to_string(st.invites.size()),
                                 W(s.lang, st.invites.empty() ? kInvitationsNone : kInvitationsWhy), s.lang);
        rows.push_back(std::move(invitations));
        rows.push_back(Button(kSave, W(s.lang, kHostsSave), W(s.lang, kHostsSaveWhy), s.lang));
        rows.push_back(separator);
        rows.push_back(GoRow(s, kJoin, kJoinWhy));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackWhy), s.lang));
    } else if (s.page == Page::steam_friends) {
        for (std::size_t i = 0; i < kSteamListed; ++i) {
            const std::size_t at = st.friend_first + i;
            Row r;
            if (at >= st.friends.size()) {
                r.label = "-";
                r.description = W(s.lang, st.friends.empty() ? kNoFriends : kNoFriendRow);
                r.disabled = s.dim_steam;
            } else {
                const SteamFriend& f = st.friends[at];
                r.label = f.name;
                r.value = W(s.lang, f.in_app ? kInApp : f.online ? kOnline : kOffline);
                r.description = !st.note.empty() ? st.note : W(s.lang, f.in_app ? kFriendWhy : kFriendAwayWhy);
            }
            rows.push_back(std::move(r));
        }
        Row more = Button(kMoreFriends, Range(st.friend_first, st.friends.size()), W(s.lang, kMoreFriendsWhy),
                          s.lang);
        more.disabled = st.friends.size() <= kSteamListed && s.dim_steam;
        rows.push_back(std::move(more));
        rows.push_back(separator);
        rows.push_back(Button(kRoom, Hosting(s) ? st.hosted_room : "-", W(s.lang, kRoomHow), s.lang));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackHostWhy), s.lang));
    } else {
        for (std::size_t i = 0; i < kSteamListed; ++i) {
            const std::size_t at = st.invite_first + i;
            Row r;
            if (at >= st.invites.size()) {
                r.label = "-";
                r.description = W(s.lang, kNoInviteRow);
                r.disabled = s.dim_steam;
            } else {
                r.label = st.invites[at].from.empty() ? W(s.lang, kAFriend) : st.invites[at].from;
                r.value = W(s.lang, kJoin);
                r.description = W(s.lang, kInviteRowWhy);
            }
            rows.push_back(std::move(r));
        }
        Row more = Button(kMoreInvites, Range(st.invite_first, st.invites.size()), W(s.lang, kMoreInvitesWhy),
                          s.lang);
        more.disabled = st.invites.size() <= kSteamListed && s.dim_steam;
        rows.push_back(std::move(more));
        rows.push_back(separator);
        rows.push_back(Button(kReceived, std::to_string(st.invites.size()),
                              s.message.empty() ? W(s.lang, st.invites.empty() ? kInvitationsNone : kInvitationsWhy)
                                                : s.message.c_str(),
                              s.lang));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackJoinWhy), s.lang));
    }
    return rows;
}

Field FieldAt(Page page, std::size_t row) {
    if ((page == Page::steam_host || page == Page::steam_join) && row == 0) {
        return Field::steam_name;
    }
    return page == Page::steam_join && row == 1 ? Field::room : Field::none;
}

Action Activate(State& s, std::size_t row) {
    SteamState& st = s.steam;
    if (s.page == Page::steam_friends) {
        if (row < kSteamListed) {
            const std::size_t at = st.friend_first + row;
            if (at >= st.friends.size() || !Hosting(s)) {
                return Action::none;
            }
            st.invite_id = st.friends[at].id;
            st.note = Line(W(s.lang, kInvitedOpen) + st.friends[at].name + ".");
            return Action::invite_friend;
        }
        if (row == kSteamListed) {
            if (st.friends.size() <= kSteamListed) {
                return Action::none;
            }
            st.friend_first = st.friend_first + kSteamListed >= st.friends.size() ? 0 : st.friend_first + kSteamListed;
            st.note.clear();
            s.focus = 1;
            return Action::redraw;
        }
        if (row == kRowGo) {
            return Hosting(s) ? Action::copy_room : Action::none;
        }
        if (row == kRowBack) {
            s.page = Page::steam_host;
            s.focus = 3;  // the row that led here
            st.note.clear();
            return Action::redraw;
        }
        return Action::none;
    }
    if (s.page == Page::steam_invites) {
        if (row < kSteamListed) {
            const std::size_t at = st.invite_first + row;
            if (at >= st.invites.size() || Busy(s.phase)) {
                return Action::none;
            }
            st.room = st.invites[at].room;
            s.page = Page::steam_join;
            s.focus = static_cast<std::uint32_t>(1 + kRowGo);
            return Go(s, false);
        }
        if (row == kSteamListed) {
            if (st.invites.size() <= kSteamListed) {
                return Action::none;
            }
            st.invite_first = st.invite_first + kSteamListed >= st.invites.size() ? 0 : st.invite_first + kSteamListed;
            s.focus = 1;
            return Action::redraw;
        }
        if (row == kRowBack) {
            s.page = Page::steam_join;
            s.focus = 4;
            return Action::redraw;
        }
        return Action::none;
    }
    const bool host = s.page == Page::steam_host;
    if (row == kRowBack) {
        s.focus = host ? 1 : 3;  // the entry that led here
        s.page = Page::main;
        st.note.clear();
        ClearIfIdle(s);
        return Action::redraw;
    }
    if (row == kRowGo) {
        return Go(s, host);
    }
    if (host && row == 1) {
        return Hosting(s) ? Action::copy_room : Action::none;
    }
    if (host && row == 2) {
        if (!Hosting(s)) {
            return Action::none;
        }
        s.page = Page::steam_friends;
        s.focus = 1;
        st.friend_first = 0;
        st.note.clear();
        return Action::redraw;
    }
    if (!host && row == 2) {
        return Busy(s.phase) ? Action::none : Action::paste_room;
    }
    if (!host && row == 3) {
        if (st.invites.empty()) {
            return Action::none;
        }
        s.page = Page::steam_invites;
        s.focus = 1;
        st.invite_first = 0;
        return Action::redraw;
    }
    const Field field = SteamPages::FieldAt(s.page, row);
    if (field == Field::none || Busy(s.phase)) {
        return Action::none;
    }
    st.note.clear();
    BeginEdit(s, field);
    return Action::begin_edit;
}

void Pasted(State& s, std::string_view text) {
    const std::string id = SteamModel::RoomId(text);
    if (id.empty()) {
        s.steam.note = W(s.lang, kNothingToPaste);
        return;
    }
    s.steam.room = id;
    s.steam.note = W(s.lang, kPasted);
    if (s.phase == Phase::failed) {
        s.phase = Phase::idle;
        s.message.clear();
    }
}

void Copied(State& s, bool ok) {
    s.steam.note = W(s.lang, ok ? kRoomCopied : kRoomNotCopied);
}

void Invited(State& s, bool ok) {
    if (!ok) {
        s.steam.note = W(s.lang, kNotInvited);
    }
}

bool Update(State& s, SteamAvail avail, std::string_view name, std::string_view hosted_room,
            std::vector<std::string> members, std::vector<SteamFriend> friends,
            std::vector<SteamInvite> invites) {
    SteamState next = s.steam;
    next.avail = avail;
    next.name = SteamModel::PlayerName(name);
    next.hosted_room = std::string(hosted_room);
    next.members = std::move(members);
    next.friends = std::move(friends);
    next.invites = std::move(invites);
    if (next.friend_first >= next.friends.size()) {
        next.friend_first = 0;
    }
    if (next.invite_first >= next.invites.size()) {
        next.invite_first = 0;
    }
    if (next == s.steam) {
        return false;
    }
    s.steam = std::move(next);
    return true;
}

std::string EditStart(const State& s, Field field) {
    return field == Field::steam_name ? PlayerName(s) : s.steam.room;
}

bool Commit(State& s, Field field, std::string_view text) {
    if (field == Field::steam_name) {
        if (text.empty() || text == s.steam.name) {
            s.steam.own_name.clear();  // the Steam name again
            return true;
        }
        if (!ValidName(text)) {
            s.message = W(s.lang, kBadName);
            return false;
        }
        s.steam.own_name = std::string(text);
        return true;
    }
    if (text.empty()) {
        s.steam.room.clear();
        return true;
    }
    const std::string id = SteamModel::RoomId(text);
    if (id.empty()) {
        s.message = W(s.lang, kBadRoom);
        return false;
    }
    s.steam.room = id;
    return true;
}

const char* WhyUnavailable(Lang lang, SteamAvail avail) {
    switch (avail) {
    case SteamAvail::old_game: return W(lang, kWhyOldGame);
    case SteamAvail::checking: return W(lang, kWhyChecking);
    case SteamAvail::no_library: return W(lang, kWhyNoLibrary);
    case SteamAvail::old_library: return W(lang, kWhyOldLibrary);
    case SteamAvail::no_client: return W(lang, kWhyNoClient);
    case SteamAvail::game_coop: return W(lang, kWhyGameCoop);
    case SteamAvail::in_use: return W(lang, kWhyInUse);
    default: return W(lang, kWhyOther);
    }
}

const char* WhyFailed(Lang lang, SteamFail fail) {
    const auto at = static_cast<std::size_t>(fail);
    return W(lang, kFails[at < std::size(kFails) ? at : std::size(kFails) - 1]);
}

}  // namespace CoopNx2navi::Menu::SteamPages
