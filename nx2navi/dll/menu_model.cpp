// SPDX-License-Identifier: GPL-3.0-or-later
#include "menu_model.hpp"
#include "steam_menu.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "mp_saves.hpp"
#include "overlay_model.hpp"

namespace CoopNx2navi::Menu {
namespace {

char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
std::string Lowered(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), Lower);
    return out;
}
std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}
bool Digits(std::string_view s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool Control(unsigned char c) {
    return c < 0x20 || c == 0x7f;
}

// Every text of the menu, English then French.
struct Words {
    const char* en;
    const char* fr;
};
const char* W(Lang lang, const Words& w) {
    return lang == Lang::fr ? w.fr : w.en;
}

constexpr Words kTitleMain{"Multiplayer", "Multijoueur"};
constexpr Words kHostLocal{"Host over Local Network", "Héberger en réseau local"};
constexpr Words kJoinLocal{"Join over Local Network", "Rejoindre en réseau local"};
constexpr Words kUnavailable{"Unavailable", "Indisponible"};
constexpr Words kOpen{"Open", "Ouvrir"};
// Captions: two lines of at most 46 characters (the box does not wrap).
constexpr Words kHostLocalWhy{"Starts the co-op server on this PC and plays.\nYour friends join with this PC's address.",
    "Lance le serveur co-op sur ce PC et y joue.\nVos amis rejoignent avec l'adresse de ce PC."};
constexpr Words kJoinLocalWhy{"Joins a friend's game with their address.\nYou play on their save; yours is kept.",
    "Rejoint la partie d'un ami avec son adresse.\nVous jouez sur sa sauvegarde ; la v\u00f4tre reste."};
constexpr Words kStatus{"Status", "État"};
constexpr Words kClose{"Close", "Fermer"};
constexpr Words kCloseWhy{"Back to the title screen.", "Retour à l'écran titre."};
constexpr Words kBack{"Back", "Retour"};
constexpr Words kBackWhy{"Back to the Multiplayer page.", "Retour à la page Multijoueur."};
constexpr Words kName{"Name", "Nom"};
constexpr Words kNameWhy{"Your name for the other players\n(1 to 16 characters).",
    "Votre nom pour les autres joueurs\n(1 à 16 caractères)."};
constexpr Words kPort{"Port", "Port"};
constexpr Words kPortWhy{"The server's UDP port (usually 27510).\nYour friends need the same one.",
    "Port UDP du serveur (d'habitude 27510).\nVos amis doivent utiliser le même."};
constexpr Words kPassword{"Password", "Mot de passe"};
constexpr Words kHostPasswordWhy{"Optional: your friends need it to join.\nLeave it empty for none.",
    "Facultatif : vos amis en auront besoin.\nLaissez vide pour ne pas en mettre."};
constexpr Words kJoinPasswordWhy{"The host's password, if there is one.\nLeave it empty for none.",
    "Le mot de passe de l'hôte, s'il y en a un.\nLaissez vide s'il n'y en a pas."};
constexpr Words kNone{"none", "aucun"};
constexpr Words kSet{"set", "défini"};
constexpr Words kGive{"Address to give", "Adresse à donner"};
// The address to give: what it is (first line), then what choosing it does.
constexpr Words kGiveLan{"This PC's address on your home network.",
    "L'adresse de ce PC sur votre réseau local."};
constexpr Words kGiveVpn{"This PC's VPN address (Hamachi, Radmin...).",
    "L'adresse VPN de ce PC (Hamachi, Radmin...)."};
constexpr Words kGiveOther{"Another network card of this PC.", "Une autre carte réseau de ce PC."};
constexpr Words kGiveVirtual{"Virtual card (Hyper-V, WSL): not for friends.",
    "Carte virtuelle (Hyper-V, WSL) : pas celle-ci."};
constexpr Words kGiveSamePc{"This PC only (a second game here).", "Ce PC seulement (un second jeu ici)."};
constexpr Words kGiveHow{"Choose it: copy, then the next address.", "Valider : copie, puis l'adresse suivante."};
constexpr Words kCopied{"Copied. Choose it again: the next address.",
    "Copiée. Valider encore : l'adresse suivante."};
constexpr Words kStart{"Start", "Démarrer"};
constexpr Words kStartWhy{"Starts the server and the session.\nThen choose Continue to load your save.",
    "Lance le serveur et la session.\nChoisissez ensuite Continuer."};
constexpr Words kAddress{"Address", "Adresse"};
constexpr Words kAddressWhy{"The host's address, for example 192.168.1.20\n(address:port sets the port too).",
    "L'adresse de l'hôte, par exemple 192.168.1.20\n(adresse:port règle aussi le port)."};
constexpr Words kJoin{"Join", "Rejoindre"};
constexpr Words kJoinWhy{"Takes the host's save, then the game restarts\non it. Your own saves are not touched.",
    "Reçoit la sauvegarde de l'hôte, puis le jeu\nredémarre dessus. Les vôtres ne changent pas."};
constexpr Words kEditHelp{"Type, Enter confirms, Esc cancels.\nPad: up/down, right adds, left deletes, A.",
    "Tapez, Entrée valide, Échap annule.\nManette : haut/bas, droite, gauche, A valide."};
constexpr Words kBadName{"The name needs 1 to 16 characters.", "Le nom doit avoir 1 à 16 caractères."};
constexpr Words kBadPort{"The port is a number from 1 to 65535.",
    "Le port est un nombre de 1 à 65535."};
constexpr Words kBadPassword{"A password: no spaces, 128 characters at most.",
    "Mot de passe : sans espace, 128 caractères au plus."};
constexpr Words kBadAddress{"Not an address. Example: 192.168.1.20",
    "Ce n'est pas une adresse. Exemple : 192.168.1.20"};
constexpr Words kNoAddress{"Type the host's address first.", "Tapez d'abord l'adresse de l'hôte."};
constexpr Words kOffline{"Offline", "Hors ligne"};
constexpr Words kOfflineWhy{"No session yet:\nhost or join with the entries above.",
    "Pas de session :\nhébergez ou rejoignez ci-dessus."};
constexpr Words kPhaseServer{"Starting...", "Démarrage..."};
constexpr Words kPhaseConnecting{"Connecting...", "Connexion..."};
constexpr Words kPhaseSave{"Receiving...", "Réception..."};
constexpr Words kPhaseRestart{"Restarting...", "Redémarrage..."};
constexpr Words kPhaseHosting{"Hosting", "Hébergement"};
constexpr Words kPhaseJoined{"Connected", "Connecté"};
constexpr Words kPhaseFailed{"Failed", "Échec"};
// Multiplayer saves.
constexpr Words kSaves{"Multiplayer saves", "Sauvegardes multijoueur"};
constexpr Words kSavesWhy{"Your co-op games, kept apart from your solo\nsaves: create, copy, delete.",
    "Vos parties co-op, à part de vos sauvegardes\nsolo : créer, copier, supprimer."};
constexpr Words kSavesOffWhy{"This NX2navi cannot change saves in the game:\na host plays on its own saves.",
    "Ce NX2navi ne sait pas changer de sauvegarde :\nun hôte joue sur les siennes."};
constexpr Words kSave{"Save", "Sauvegarde"};
constexpr Words kChoose{"Choose", "Choisir"};
constexpr Words kChosen{"Chosen", "Choisie"};
constexpr Words kYourOwn{"Your own", "La vôtre"};
constexpr Words kSaveRowWhy{"The multiplayer save this game is played on.\nYour solo saves are never written.",
    "La sauvegarde multijoueur de cette partie.\nVos sauvegardes solo ne sont jamais modifiées."};
constexpr Words kStartSaveWhy{"The game restarts on the chosen save,\nthen starts the server and the session.",
    "Le jeu redémarre sur la sauvegarde choisie,\npuis lance le serveur et la session."};
constexpr Words kHostsSave{"The host's", "Celle de l'hôte"};
constexpr Words kHostsSaveWhy{"Received from the host and kept with your\nmultiplayer saves. Your solo saves stay.",
    "Reçue de l'hôte et rangée dans vos sauvegardes\nmultijoueur. Vos sauvegardes solo restent."};
constexpr Words kNoSaveChosen{"Choose a multiplayer save first\n(the Save row above).",
    "Choisissez d'abord une sauvegarde multijoueur\n(ligne Sauvegarde ci-dessus)."};
constexpr Words kNoSaves{"None", "Aucune"};
constexpr Words kNoSaveRowWhy{"No other multiplayer save.", "Pas d'autre sauvegarde multijoueur."};
constexpr Words kMore{"More saves", "Autres sauvegardes"};
constexpr Words kMoreWhy{"Shows the next multiplayer saves.", "Affiche les sauvegardes multijoueur suivantes."};
constexpr Words kNewSave{"New save", "Nouvelle sauvegarde"};
constexpr Words kNewSaveWhy{"Type its name: a fresh co-op game,\nstarted from the beginning.",
    "Tapez son nom : une partie co-op neuve,\ncommencée depuis le début du jeu."};
constexpr Words kCopySolo{"Copy my solo save", "Copier ma sauvegarde solo"};
constexpr Words kCopy{"Copy", "Copier"};
constexpr Words kCopySoloWhy{"Makes a multiplayer save from your newest\nsolo save, which stays as it is.",
    "Fait une sauvegarde multijoueur de votre\ndernière partie solo, qui reste intacte."};
constexpr Words kNoSolo{"You have no solo save to copy yet.", "Vous n'avez pas encore de sauvegarde solo."};
constexpr Words kBackFromSavesWhy{"Back to the previous page.", "Retour à la page précédente."};
constexpr Words kNewGame{"New game", "Nouvelle partie"};
constexpr Words kSavedIn{"Saved in ", "Lieu : "};
constexpr Words kNotBegun{"Not begun yet: a new game.", "Pas encore commencée : une nouvelle partie."};
constexpr Words kOwnLine{"Your multiplayer save.", "Votre sauvegarde multijoueur."};
constexpr Words kChosenLine{"Chosen for hosting.", "Choisie pour héberger."};
constexpr Words kReceivedFrom{"Received from ", "Reçue de "};
// Short labels on this page: the game draws a page's labels smaller when one is long.
constexpr Words kPlay{"Play on it", "Jouer dessus"};
constexpr Words kPlayWhy{"Host on this save: the game restarts on it\nwhen you choose Start.",
    "Pour héberger dessus : le jeu redémarre sur\ncette sauvegarde quand vous démarrez."};
constexpr Words kSaveNameWhy{"Another name for this save\n(1 to 16 characters).",
    "Un autre nom pour cette sauvegarde\n(1 à 16 caractères)."};
constexpr Words kReceivedNameWhy{"A save received from a host keeps the name\nthe host gave it.",
    "Une sauvegarde reçue d'un hôte garde le nom\nque l'hôte lui a donné."};
constexpr Words kLastPlayed{"Last played", "Dernière partie"};
constexpr Words kNever{"never", "jamais"};
constexpr Words kLastPlayedWhy{"When this save was last written.", "Dernière écriture de cette sauvegarde."};
constexpr Words kPlace{"Place", "Lieu"};
constexpr Words kPlaceWhy{"Where Link was at the last save.", "Où était Link à la dernière sauvegarde."};
constexpr Words kOrigin{"Origin", "Origine"};
constexpr Words kYours{"Yours", "À vous"};
constexpr Words kOriginOwnWhy{"Your save: it is the reference. Friends who\njoin get a copy of it.",
    "Votre sauvegarde : elle fait foi. Vos amis\nen reçoivent une copie en rejoignant."};
constexpr Words kOriginReceivedWhy{"A copy of the host's save, replaced each\ntime you join them (the last one is kept).",
    "Copie de la sauvegarde de l'hôte, remplacée\nà chaque fois que vous le rejoignez."};
constexpr Words kDelete{"Delete", "Supprimer"};
constexpr Words kDeleteWhy{"Takes this save out of the list\n(kept in coop-saves\\_deleted).",
    "Retire cette sauvegarde de la liste\n(gardée dans coop-saves\\_deleted)."};
constexpr Words kConfirm{"Confirm", "Confirmer"};
constexpr Words kConfirmOpen{"Delete \"", "Supprimer « "};
constexpr Words kConfirmClose{"\"?", " » ?"};
constexpr Words kConfirmHow{"Choose it again, or move away to cancel.", "Validez encore, ou changez de ligne."};
constexpr Words kBackToSavesWhy{"Back to the multiplayer saves.", "Retour aux sauvegardes multijoueur."};
constexpr Words kBadSaveName{"A save's name: 1 to 16 characters,\nnone of \\ / : * ? \" < > | @",
    "Nom d'une sauvegarde : 1 à 16 caractères,\nsans \\ / : * ? \" < > | @"};
constexpr Words kSaveExists{"A save of that name is there already.", "Une sauvegarde porte déjà ce nom."};
constexpr Words kNewSaveBase{"Coop", "Coop"};
constexpr Words kSoloCopyBase{"Solo", "Solo"};

constexpr Words kTexts[] = {
    /* title_entry */ {"Multiplayer", "Multijoueur"},
    /* server_failed */ {"The server stopped at once:\nis the port already in use?", "Le serveur s'est arrêté aussitôt :\nle port est-il déjà pris ?"},
    /* server_missing */ {"bin\\oot-coop-server.exe is missing.", "bin\\oot-coop-server.exe est introuvable."},
    /* connecting */ {"Connecting to the host...", "Connexion à l'hôte..."},
    /* receiving_save */ {"Receiving the host's save...", "Réception de la sauvegarde de l'hôte..."},
    /* restarting */ {"Save received. The game restarts:\nthen choose Continue.", "Sauvegarde reçue. Le jeu redémarre :\nchoisissez ensuite Continuer."},
    /* no_host_save */ {"The host sent no save:\nload one of your own.", "L'hôte n'a pas envoyé de sauvegarde :\nchargez l'une des vôtres."},
    /* no_answer */ {"No answer.\nCheck the address and the port.", "Pas de réponse.\nVérifiez l'adresse et le port."},
    /* refused */ {"The server refused", "Le serveur a refusé"},
    /* hosting_done */ {"The session runs.\nChoose Continue to load your save.", "La session est lancée.\nChoisissez Continuer."},
    /* joined_done */ {"Connected to the session.", "Connecté à la session."},
    /* already_running */ {"A session already runs.\nRestart the game for another one.", "Une session tourne déjà.\nRedémarrez le jeu pour une autre."},
    /* save_failed */ {"The host's save could not be written:\nload one of your own.", "La sauvegarde de l'hôte n'a pas pu être\nécrite : chargez l'une des vôtres."},
    /* restart_failed */ {"The save is ready, but the game cannot restart\nitself: close it, join from the launcher.", "Sauvegarde prête, mais le jeu ne peut pas\nredémarrer seul : rejoignez depuis le lanceur."},
    /* host_restarting */ {"The game restarts on your multiplayer save:\nthen choose Continue.", "Le jeu redémarre sur votre sauvegarde\nmultijoueur : choisissez ensuite Continuer."},
    /* host_restarting_new */ {"The game restarts on your new multiplayer\nsave: then start a new game.", "Le jeu redémarre sur votre nouvelle sauvegarde\nmultijoueur : commencez une nouvelle partie."},
    /* host_restart_failed */ {"The game cannot restart itself on the save:\nupdate NX2navi, or host from the launcher.", "Le jeu ne peut pas redémarrer sur la\nsauvegarde : mettez NX2navi à jour."},
    /* save_op_failed */ {"That could not be done with the save\n(coop-nx2navi.log says why).", "Impossible pour cette sauvegarde\n(coop-nx2navi.log dit pourquoi)."},
    /* save_in_use */ {"The game runs on this save:\nnot while it does.", "Le jeu tourne sur cette sauvegarde :\npas pendant ce temps."},
    /* restarting_new */ {"The host begins a new game. The game restarts:\nthen start a new game too.", "L'hôte commence une partie. Le jeu redémarre :\ncommencez une nouvelle partie vous aussi."},
    /* steam_making */ {"Making the Steam room...", "Création du salon Steam..."},
    /* steam_entering */ {"Entering the host's Steam room...", "Entrée dans le salon Steam de l'hôte..."},
};

// The alphabets of the gamepad's typing.
constexpr std::string_view kNameChars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 -_";
constexpr std::string_view kAddressChars = "0123456789.:abcdefghijklmnopqrstuvwxyz-";
constexpr std::string_view kPortChars = "0123456789";
constexpr std::string_view kRoomChars = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // a Steam room's id
constexpr std::string_view kPasswordChars =
    "abcdefghijklmnopqrstuvwxyz0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-_.!";

// A field whose text is a name: any character, counted in code points.
bool NameLike(Field field) {
    return field == Field::name || field == Field::save_new || field == Field::save_rename ||
           field == Field::steam_name;
}
bool SaveField(Field field) {
    return field == Field::save_new || field == Field::save_rename;
}

std::string_view Alphabet(Field field) {
    switch (field) {
    case Field::name:
    case Field::save_new:
    case Field::save_rename:
    case Field::steam_name: return kNameChars;
    case Field::room: return kRoomChars;
    case Field::address: return kAddressChars;
    case Field::port: return kPortChars;
    default: return kPasswordChars;
    }
}

std::size_t MaxOf(Field field) {
    switch (field) {
    case Field::name:
    case Field::save_new:
    case Field::save_rename:
    case Field::steam_name: return kMaxName;
    case Field::room: return 9;  // "ABCD-2345"
    case Field::address: return kMaxAddress;
    case Field::port: return 5;
    default: return kMaxPassword;
    }
}

// The most bytes of UTF-8 a field's text can have (the game's own typing).
std::size_t MaxBytes(Field field) {
    return NameLike(field) ? kMaxName * 4 : MaxOf(field);
}

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

// The first of "Coop", "Coop 2" ... that no save has.
std::string FreeSaveName(const State& s, const Words& base) {
    for (unsigned n = 1; n < 1000; ++n) {
        const std::string name = n == 1 ? std::string(W(s.lang, base))
                                        : std::string(W(s.lang, base)) + " " + std::to_string(n);
        const std::string folded = MpSaves::Folded(name);
        if (std::none_of(s.saves.begin(), s.saves.end(),
                         [&](const SaveInfo& e) { return MpSaves::Folded(e.folder) == folded; })) {
            return name;
        }
    }
    return W(s.lang, base);
}

Row Plain(std::string label, std::string value, std::string description) {
    Row r;
    r.label = std::move(label);
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

// A field's row: its text, or what is being typed with a cursor.
Row FieldRow(const State& s, Field field, const Words& label, const Words& why) {
    const bool typing = s.editing == field;
    // The game's own typing (a keyboard): not for a password, whose row shows a
    // word and not the text, and not while we type ourselves.
    const bool game_typing = s.game_typing && s.editing == Field::none &&
                             (!Busy(s.phase) || SaveField(field)) &&
                             field != Field::host_password && field != Field::join_password;
    std::string value;
    if (typing) {
        value = s.edit + "_";
    } else {
        switch (field) {
        case Field::name: value = s.name; break;
        case Field::address: value = s.address; break;
        case Field::port: value = s.port; break;
        case Field::host_password:
            // Shown as it is: the host has to give it to the others. Empty: none (anyone who
            // reaches the address and port can join; settings.hpp kOpenPassword).
            value = s.host_password.empty() ? W(s.lang, kNone) : s.host_password;
            break;
        case Field::join_password: value = W(s.lang, s.join_password.empty() ? kNone : kSet); break;
        case Field::save_new: value = FreeSaveName(s, kNewSaveBase); break;
        case Field::save_rename:
            if (const SaveInfo* shown = FindSave(s, s.save_shown)) {
                value = shown->name;
            }
            break;
        default: break;
        }
        if (value.empty() && !game_typing) {
            value = "-";  // the game's typing starts from the row's own text
        }
    }
    std::string description = typing ? W(s.lang, kEditHelp) : W(s.lang, why);
    if (SaveField(field)) {
        if (!typing && !s.save_note.empty()) {
            description = s.save_note;
        }
    } else if (!typing && !s.message.empty() && s.phase != Phase::failed) {
        // A refused text's rule, until the next change.
        description = s.message;
    }
    Row r = Button(label, std::move(value), std::move(description), s.lang);
    r.kind = RowKind::text;
    r.text_max = game_typing ? static_cast<std::uint32_t>(MaxBytes(field)) : 0;
    return r;
}

std::string PhaseWord(const State& s) {
    switch (s.phase) {
    case Phase::starting_server: return W(s.lang, kPhaseServer);
    case Phase::connecting: return W(s.lang, kPhaseConnecting);
    case Phase::receiving_save: return W(s.lang, kPhaseSave);
    case Phase::restarting: return W(s.lang, kPhaseRestart);
    case Phase::hosting: return W(s.lang, kPhaseHosting);
    case Phase::joined: return W(s.lang, kPhaseJoined);
    case Phase::failed: return W(s.lang, kPhaseFailed);
    default: return W(s.lang, kOffline);
    }
}

// The Start / Join button: its word while nothing runs, else how far it got.
Row GoRow(const State& s, const Words& label, const Words& why) {
    Row r = Button(label, Busy(s.phase) || s.phase == Phase::failed ? PhaseWord(s) : W(s.lang, label),
                   s.message.empty() ? W(s.lang, why) : s.message, s.lang);
    return r;
}

}  // namespace

Lang LangFromCode(std::string_view code) {
    const std::string c = Lowered(Trim(code));
    return c.size() >= 2 && c[0] == 'f' && c[1] == 'r' && (c.size() == 2 || c[2] == '-' || c[2] == '_')
               ? Lang::fr
               : Lang::en;
}

std::string IniValue(std::string_view text, std::string_view key) {
    const std::string want = Lowered(key);
    if (text.substr(0, 3) == "\xef\xbb\xbf") {
        text.remove_prefix(3);
    }
    std::string found;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = Trim(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (line.empty() || line.front() == '#' || line.front() == ';' || line.front() == '[') {
            continue;
        }
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) {
            continue;
        }
        std::string name = Lowered(Trim(line.substr(0, eq)));
        if (name.rfind("nx2navi_", 0) == 0) {
            name.erase(0, 8);
        }
        if (name != want) {
            continue;
        }
        std::string_view value = Trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }
        found = std::string(value);  // the last line wins, as a later line replaces an earlier one
    }
    return found;
}

Lang DetectLang(std::string_view env_lang, std::string_view ini_text, std::string_view env_locale,
                std::string_view user_locale) {
    if (!Trim(env_lang).empty()) {
        return LangFromCode(env_lang);
    }
    if (const std::string lang = IniValue(ini_text, "lang"); !lang.empty()) {
        return LangFromCode(lang);
    }
    if (!Trim(env_locale).empty()) {
        return LangFromCode(env_locale);
    }
    if (const std::string locale = IniValue(ini_text, "locale"); !locale.empty()) {
        return LangFromCode(locale);
    }
    return LangFromCode(user_locale);
}

bool ValidName(std::string_view name) {
    if (name.empty() || std::any_of(name.begin(), name.end(), [](char c) {
            return Control(static_cast<unsigned char>(c));
        })) {
        return false;
    }
    const std::size_t count = Ui::Utf8Count(name);
    return count >= 1 && count <= kMaxName && !Trim(name).empty();
}

bool ValidPort(std::string_view port) {
    if (!Digits(port) || port.size() > 5) {
        return false;
    }
    const long value = std::strtol(std::string(port).c_str(), nullptr, 10);
    return value >= 1 && value <= 65535;
}

bool ValidPassword(std::string_view password) {
    return !password.empty() && password.size() <= kMaxPassword &&
           std::none_of(password.begin(), password.end(), [](char c) {
               const auto u = static_cast<unsigned char>(c);
               return Control(u) || u == ' ';
           });
}

std::string AddressKind(std::string_view address) {
    int o[4] = {};
    int part = 0;
    int digits = 0;
    for (const char c : address) {
        if (c == '.') {
            if (digits == 0 || ++part > 3) {
                return {};
            }
            digits = 0;
        } else if (c >= '0' && c <= '9') {
            if (++digits > 3) {
                return {};
            }
            o[part] = o[part] * 10 + (c - '0');
        } else {
            return {};
        }
    }
    if (part != 3 || digits == 0 || o[0] > 255 || o[1] > 255 || o[2] > 255 || o[3] > 255) {
        return {};
    }
    if (o[0] == 127) return "loopback";
    if (o[0] == 169 && o[1] == 254) return "link-local";
    if (o[0] == 25) return "hamachi";
    if (o[0] == 26) return "radmin";
    if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return "tailscale";
    if (o[0] == 10 || (o[0] == 172 && o[1] >= 16 && o[1] <= 31) || (o[0] == 192 && o[1] == 168)) {
        return "lan";
    }
    if (o[0] == 0 || o[0] >= 224) return {};
    return "public";
}

std::optional<JoinAddress> ParseJoinAddress(std::string_view text) {
    std::string t;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= ' ' || c == '"' || c == '\'' || c == '<' || c == '>') {
            continue;
        }
        t.push_back(c);
    }
    const std::string low = Lowered(t);
    for (const std::string_view scheme : {"udp://", "http://", "https://"}) {
        if (low.rfind(scheme, 0) == 0) {
            t.erase(0, scheme.size());
            break;
        }
    }
    while (!t.empty() && (t.back() == '/' || t.back() == '.' || t.back() == ',' || t.back() == ';')) {
        t.pop_back();
    }
    JoinAddress out;
    const auto colon = t.find(':');
    out.server = t.substr(0, colon);
    if (colon != std::string::npos) {
        out.port = t.substr(colon + 1);
        if (!ValidPort(out.port)) {
            return std::nullopt;
        }
    }
    while (!out.server.empty() && out.server.back() == '.') {
        out.server.pop_back();
    }
    if (out.server.empty() || out.server.size() > kMaxAddress) {
        return std::nullopt;
    }
    bool numeric = true;
    for (const char c : out.server) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        c == '.' || c == '_' || c == '-';
        if (!ok) {
            return std::nullopt;
        }
        numeric = numeric && ((c >= '0' && c <= '9') || c == '.');
    }
    if (Lowered(out.server) == "localhost") {
        out.server = "127.0.0.1";
    }
    // Digits and dots only: it must be a whole IPv4 address (26.1.2 is a typo).
    if (numeric && AddressKind(out.server).empty()) {
        return std::nullopt;
    }
    return out;
}

bool VirtualCard(std::string_view name, std::string_view description) {
    const std::string text = Lowered(name) + " | " + Lowered(description);
    for (const std::string_view mark :
         {"vethernet", "hyper-v", "wsl", "virtual", "vmware", "vmnet", "virtualbox", "docker",
          "loopback", "npcap", "tap-windows", "teredo", "isatap", "bluetooth"}) {
        if (text.find(mark) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<GiveAddress> JoinAddresses(const std::vector<LocalAddress>& local) {
    using Kind = GiveAddress::Kind;
    std::vector<GiveAddress> out;
    // Best first: one pass per kind, in the order the cards came.
    for (const Kind want : {Kind::lan, Kind::vpn, Kind::other, Kind::virtual_card}) {
        for (const LocalAddress& a : local) {
            const std::string kind = AddressKind(a.address);
            Kind is;
            if (kind == "hamachi" || kind == "radmin" || kind == "tailscale") {
                is = Kind::vpn;  // a VPN's card is "virtual" to Windows too: the address says what it is
            } else if (kind != "lan") {
                continue;  // loopback, link-local, a public address: nothing to give
            } else if (a.virtual_card) {
                is = Kind::virtual_card;
            } else {
                is = a.gateway ? Kind::lan : Kind::other;
            }
            if (is == want && std::none_of(out.begin(), out.end(), [&](const GiveAddress& g) {
                    return g.address == a.address;
                })) {
                out.push_back({a.address, is});
            }
        }
    }
    out.push_back({"127.0.0.1", Kind::same_pc});
    return out;
}

// --- FlatJson ---

namespace {

void SkipSpace(std::string_view text, std::size_t& at) {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) {
        ++at;
    }
}

// One JSON value starting at `at`: its text, with `at` moved past it. Nested
// objects and arrays are kept whole.
bool TakeValue(std::string_view text, std::size_t& at, std::string& out) {
    const std::size_t start = at;
    if (at >= text.size()) {
        return false;
    }
    if (text[at] == '"') {
        for (++at; at < text.size(); ++at) {
            if (text[at] == '\\') {
                ++at;
            } else if (text[at] == '"') {
                ++at;
                out = std::string(text.substr(start, at - start));
                return true;
            }
        }
        return false;
    }
    if (text[at] == '{' || text[at] == '[') {
        int depth = 0;
        bool in_string = false;
        for (; at < text.size(); ++at) {
            const char c = text[at];
            if (in_string) {
                if (c == '\\') {
                    ++at;
                } else if (c == '"') {
                    in_string = false;
                }
            } else if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) {
                    ++at;
                    out = std::string(text.substr(start, at - start));
                    return true;
                }
            }
        }
        return false;
    }
    while (at < text.size() && text[at] != ',' && text[at] != '}' && text[at] != ' ' && text[at] != '\n' &&
           text[at] != '\r' && text[at] != '\t') {
        ++at;
    }
    out = std::string(text.substr(start, at - start));
    return !out.empty();
}

void AppendUtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else if (c < 0x800) {
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else if (c < 0x10000) {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
}

std::string Unquote(std::string_view json) {
    std::string out;
    if (json.size() < 2) {
        return out;
    }
    json = json.substr(1, json.size() - 2);
    for (std::size_t i = 0; i < json.size(); ++i) {
        if (json[i] != '\\' || i + 1 >= json.size()) {
            out.push_back(json[i]);
            continue;
        }
        const char e = json[++i];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'u': {
            if (i + 4 < json.size()) {
                std::uint32_t c = static_cast<std::uint32_t>(
                    std::strtoul(std::string(json.substr(i + 1, 4)).c_str(), nullptr, 16));
                i += 4;
                if (c >= 0xd800 && c <= 0xdbff && i + 6 < json.size() && json[i + 1] == '\\' &&
                    json[i + 2] == 'u') {
                    const auto low = static_cast<std::uint32_t>(
                        std::strtoul(std::string(json.substr(i + 3, 4)).c_str(), nullptr, 16));
                    c = 0x10000 + ((c - 0xd800) << 10) + (low - 0xdc00);
                    i += 6;
                }
                AppendUtf8(out, c);
            }
            break;
        }
        default: out.push_back(e); break;
        }
    }
    return out;
}

std::string Quote(std::string_view text) {
    std::string out = "\"";
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out.push_back(kHex[u >> 4]);
            out.push_back(kHex[u & 15]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

bool FlatJson::Parse(std::string_view text) {
    items_.clear();
    if (text.substr(0, 3) == "\xef\xbb\xbf") {
        text.remove_prefix(3);
    }
    std::size_t at = 0;
    SkipSpace(text, at);
    if (at >= text.size() || text[at] != '{') {
        return false;
    }
    ++at;
    std::vector<std::pair<std::string, std::string>> items;
    while (true) {
        SkipSpace(text, at);
        if (at < text.size() && text[at] == '}') {
            break;
        }
        std::string key, value;
        if (at >= text.size() || text[at] != '"' || !TakeValue(text, at, key)) {
            return false;
        }
        SkipSpace(text, at);
        if (at >= text.size() || text[at] != ':') {
            return false;
        }
        ++at;
        SkipSpace(text, at);
        if (!TakeValue(text, at, value)) {
            return false;
        }
        items.emplace_back(Unquote(key), std::move(value));
        SkipSpace(text, at);
        if (at < text.size() && text[at] == ',') {
            ++at;
            continue;
        }
        if (at < text.size() && text[at] == '}') {
            break;
        }
        return false;
    }
    items_ = std::move(items);
    return true;
}

std::string FlatJson::Dump() const {
    if (items_.empty()) {
        return "{}";
    }
    std::string out = "{\n";
    for (std::size_t i = 0; i < items_.size(); ++i) {
        out += "  " + Quote(items_[i].first) + ": " + items_[i].second;
        out += i + 1 < items_.size() ? ",\n" : "\n";
    }
    out += "}";
    return out;
}

const std::string* FlatJson::Find(std::string_view key) const {
    for (const auto& item : items_) {
        if (item.first == key) {
            return &item.second;
        }
    }
    return nullptr;
}

std::optional<std::string> FlatJson::String(std::string_view key) const {
    const std::string* v = Find(key);
    if (v == nullptr || v->size() < 2 || v->front() != '"') {
        return std::nullopt;
    }
    return Unquote(*v);
}

std::optional<long long> FlatJson::Number(std::string_view key) const {
    const std::string* v = Find(key);
    if (v == nullptr || v->empty() || !(v->front() == '-' || (v->front() >= '0' && v->front() <= '9'))) {
        return std::nullopt;
    }
    return std::strtoll(v->c_str(), nullptr, 10);
}

std::optional<bool> FlatJson::Bool(std::string_view key) const {
    const std::string* v = Find(key);
    if (v == nullptr || (*v != "true" && *v != "false")) {
        return std::nullopt;
    }
    return *v == "true";
}

void FlatJson::Set(std::string_view key, std::string value) {
    for (auto& item : items_) {
        if (item.first == key) {
            item.second = std::move(value);
            return;
        }
    }
    items_.emplace_back(std::string(key), std::move(value));
}

void FlatJson::SetString(std::string_view key, std::string_view value) { Set(key, Quote(value)); }
void FlatJson::SetNumber(std::string_view key, long long value) { Set(key, std::to_string(value)); }
void FlatJson::SetBool(std::string_view key, bool value) { Set(key, value ? "true" : "false"); }
void FlatJson::Erase(std::string_view key) {
    items_.erase(std::remove_if(items_.begin(), items_.end(),
                                [&](const auto& item) { return item.first == key; }),
                 items_.end());
}

// --- The pages ---

std::string Title(const State& s) {
    if (SteamPages::Is(s.page)) {
        return SteamPages::Title(s);
    }
    switch (s.page) {
    case Page::host: return W(s.lang, kHostLocal);
    case Page::join: return W(s.lang, kJoinLocal);
    case Page::saves: return W(s.lang, kSaves);
    case Page::save:
        if (const SaveInfo* shown = FindSave(s, s.save_shown)) {
            return SaveLabel(*shown);
        }
        return W(s.lang, kSaves);
    default: return W(s.lang, kTitleMain);
    }
}

std::string SaveLabel(const SaveInfo& save) {
    if (!save.received) {
        return save.name;
    }
    return save.name.empty() ? "(" + save.host + ")" : save.name + " (" + save.host + ")";
}

const SaveInfo* FindSave(const State& s, std::string_view folder) {
    if (folder.empty()) {
        return nullptr;
    }
    for (const SaveInfo& save : s.saves) {
        if (save.folder == folder) {
            return &save;
        }
    }
    return nullptr;
}

Field FieldAt(Page page, std::size_t row) {
    if (SteamPages::Is(page)) {
        return SteamPages::FieldAt(page, row);
    }
    if (page == Page::host) {
        return row == 0 ? Field::name : row == 1 ? Field::port : row == 2 ? Field::host_password : Field::none;
    }
    if (page == Page::join) {
        return row == 0   ? Field::name
               : row == 1 ? Field::address
               : row == 2 ? Field::port
               : row == 3 ? Field::join_password
                          : Field::none;
    }
    if (page == Page::saves) {
        return row == kRowExtra ? Field::save_new : Field::none;
    }
    if (page == Page::save) {
        return row == 1 ? Field::save_rename : Field::none;
    }
    return Field::none;
}

std::vector<Row> Rows(const State& s) {
    if (SteamPages::Is(s.page)) {
        return SteamPages::Rows(s);
    }
    std::vector<Row> rows;
    rows.reserve(kRows);
    Row separator;
    separator.kind = RowKind::separator;
    if (s.page == Page::main) {
        // Over Steam (steam_menu.hpp): "Open", or "Unavailable" and why.
        rows.push_back(SteamPages::HostEntry(s));
        rows.push_back(Button(kHostLocal, W(s.lang, kOpen), W(s.lang, kHostLocalWhy), s.lang));
        rows.push_back(SteamPages::JoinEntry(s));
        rows.push_back(Button(kJoinLocal, W(s.lang, kOpen), W(s.lang, kJoinLocalWhy), s.lang));
        Row saves = Button(kSaves,
                           !s.saves_on        ? W(s.lang, kUnavailable)
                           : s.saves.empty()  ? W(s.lang, kNoSaves)
                                              : std::to_string(s.saves.size()),
                           W(s.lang, s.saves_on ? kSavesWhy : kSavesOffWhy), s.lang);
        saves.disabled = !s.saves_on && s.dim_steam;
        rows.push_back(std::move(saves));
        rows.push_back(separator);
        std::string status = PhaseWord(s);
        if ((s.phase == Phase::hosting || s.phase == Phase::joined) && s.player != 0) {
            status += s.lang == Lang::fr ? " (J" : " (P";
            status += std::to_string(s.player) + ")";
        }
        rows.push_back(Button(kStatus, std::move(status),
                              s.message.empty() ? W(s.lang, kOfflineWhy) : s.message, s.lang));
        rows.push_back(Button(kClose, W(s.lang, kClose), W(s.lang, kCloseWhy), s.lang));
    } else if (s.page == Page::host) {
        rows.push_back(FieldRow(s, Field::name, kName, kNameWhy));
        rows.push_back(FieldRow(s, Field::port, kPort, kPortWhy));
        rows.push_back(FieldRow(s, Field::host_password, kPassword, kHostPasswordWhy));
        const GiveAddress give = s.addresses.empty()
                                     ? GiveAddress{"127.0.0.1", GiveAddress::Kind::same_pc}
                                     : s.addresses[s.address_shown % s.addresses.size()];
        std::string shown = give.address;
        std::string why = W(s.lang, give.kind == GiveAddress::Kind::lan            ? kGiveLan
                                    : give.kind == GiveAddress::Kind::vpn          ? kGiveVpn
                                    : give.kind == GiveAddress::Kind::other        ? kGiveOther
                                    : give.kind == GiveAddress::Kind::virtual_card ? kGiveVirtual
                                                                                   : kGiveSamePc);
        why += "\n";
        why += W(s.lang, s.address_copied ? kCopied : kGiveHow);
        if (s.port != std::to_string(kDefaultPort) && ValidPort(s.port)) {
            shown += ":" + s.port;
        }
        rows.push_back(Button(kGive, std::move(shown), std::move(why), s.lang));
        const SaveInfo* chosen = FindSave(s, s.save);
        rows.push_back(Button(kSave,
                              !s.saves_on ? W(s.lang, kYourOwn)
                              : chosen    ? SaveLabel(*chosen)
                                          : W(s.lang, kChoose),
                              W(s.lang, s.saves_on ? kSaveRowWhy : kSavesOffWhy), s.lang));
        rows.push_back(separator);
        rows.push_back(GoRow(s, kStart, s.saves_on ? kStartSaveWhy : kStartWhy));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackWhy), s.lang));
    } else if (s.page == Page::saves) {
        const std::size_t count = s.saves.size();
        for (std::size_t i = 0; i < kSavesShown; ++i) {
            const std::size_t at = s.save_first + i;
            if (at >= count) {
                Row none = Plain("-", "", W(s.lang, kNoSaveRowWhy));
                none.disabled = s.dim_steam;
                rows.push_back(std::move(none));
                continue;
            }
            const SaveInfo& save = s.saves[at];
            std::string why = save.empty ? std::string(W(s.lang, kNotBegun))
                              : save.place.empty() ? std::string()
                                                   : Line(W(s.lang, kSavedIn) + save.place);
            if (!why.empty()) {
                why += "\n";
            }
            why += save.received            ? Line(W(s.lang, kReceivedFrom) + save.host + ".")
                   : save.folder == s.save  ? std::string(W(s.lang, kChosenLine))
                                            : std::string(W(s.lang, kOwnLine));
            rows.push_back(Plain(SaveLabel(save), save.empty ? W(s.lang, kNewGame) : save.played,
                                 std::move(why)));
        }
        std::string range;
        if (count > kSavesShown) {
            range = std::to_string(s.save_first + 1) + "-" +
                    std::to_string(std::min(s.save_first + kSavesShown, count)) + " / " +
                    std::to_string(count);
        }
        Row more = Button(kMore, std::move(range), W(s.lang, kMoreWhy), s.lang);
        more.disabled = count <= kSavesShown && s.dim_steam;
        rows.push_back(std::move(more));
        rows.push_back(FieldRow(s, Field::save_new, kNewSave, kNewSaveWhy));
        rows.push_back(separator);
        rows.push_back(Button(kCopySolo, W(s.lang, s.solo_save ? kCopy : kNoSaves),
                              !s.save_note.empty() && s.editing == Field::none ? s.save_note
                              : W(s.lang, s.solo_save ? kCopySoloWhy : kNoSolo), s.lang));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackFromSavesWhy), s.lang));
    } else if (s.page == Page::save) {
        static const SaveInfo kGone;
        const SaveInfo* found = FindSave(s, s.save_shown);
        const SaveInfo& save = found != nullptr ? *found : kGone;
        rows.push_back(Button(kPlay, W(s.lang, save.folder == s.save && found ? kChosen : kChoose),
                              W(s.lang, s.saves_on ? kPlayWhy : kSavesOffWhy), s.lang));
        if (save.received) {
            rows.push_back(Button(kName, save.name.empty() ? "-" : save.name, W(s.lang, kReceivedNameWhy),
                                  s.lang));
        } else {
            rows.push_back(FieldRow(s, Field::save_rename, kName, kSaveNameWhy));
        }
        rows.push_back(Button(kLastPlayed, save.played.empty() ? W(s.lang, kNever) : save.played,
                              W(s.lang, save.empty ? kNotBegun : kLastPlayedWhy), s.lang));
        rows.push_back(Button(kPlace, save.place.empty() ? "-" : save.place, W(s.lang, kPlaceWhy), s.lang));
        rows.push_back(Button(kOrigin, save.received ? save.host : W(s.lang, kYours),
                              W(s.lang, save.received ? kOriginReceivedWhy : kOriginOwnWhy), s.lang));
        rows.push_back(separator);
        std::string why = W(s.lang, kDeleteWhy);
        if (s.delete_armed) {
            why = Line(W(s.lang, kConfirmOpen) + save.name + W(s.lang, kConfirmClose)) + "\n" +
                  W(s.lang, kConfirmHow);
        } else if (!s.save_note.empty() && s.editing == Field::none) {
            why = s.save_note;
        }
        rows.push_back(Button(kDelete, W(s.lang, s.delete_armed ? kConfirm : kDelete), std::move(why),
                              s.lang));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackToSavesWhy), s.lang));
    } else {
        rows.push_back(FieldRow(s, Field::name, kName, kNameWhy));
        rows.push_back(FieldRow(s, Field::address, kAddress, kAddressWhy));
        rows.push_back(FieldRow(s, Field::port, kPort, kPortWhy));
        rows.push_back(FieldRow(s, Field::join_password, kPassword, kJoinPasswordWhy));
        rows.push_back(Button(kSave, W(s.lang, kHostsSave), W(s.lang, kHostsSaveWhy), s.lang));
        rows.push_back(separator);
        rows.push_back(GoRow(s, kJoin, kJoinWhy));
        rows.push_back(Button(kBack, W(s.lang, kBack), W(s.lang, kBackWhy), s.lang));
    }
    return rows;
}

bool ReadyToHost(const State& s, std::string& why) {
    if (!ValidName(s.name)) {
        why = W(s.lang, kBadName);
    } else if (!ValidPort(s.port)) {
        why = W(s.lang, kBadPort);
    } else if (!s.host_password.empty() && !ValidPassword(s.host_password)) {
        why = W(s.lang, kBadPassword);
    } else if (s.saves_on && FindSave(s, s.save) == nullptr) {
        why = W(s.lang, kNoSaveChosen);  // never on the player's own saves
    } else {
        return true;
    }
    return false;
}

bool ReadyToJoin(const State& s, std::string& why) {
    if (!ValidName(s.name)) {
        why = W(s.lang, kBadName);
    } else if (s.address.empty()) {
        why = W(s.lang, kNoAddress);
    } else if (!ParseJoinAddress(s.address)) {
        why = W(s.lang, kBadAddress);
    } else if (!ValidPort(s.port)) {
        why = W(s.lang, kBadPort);
    } else if (!s.join_password.empty() && !ValidPassword(s.join_password)) {
        why = W(s.lang, kBadPassword);
    } else {
        return true;
    }
    return false;
}

namespace {

// The first save of the list page's window that holds save number `at`.
std::size_t WindowOf(std::size_t at) {
    return at - at % kSavesShown;
}

void OpenSaves(State& s, Page from) {
    s.saves_from = from;
    s.page = Page::saves;
    s.save_note.clear();
    s.delete_armed = false;
    s.save_first = 0;
    s.focus = static_cast<std::uint32_t>(1 + (s.saves.empty() ? kRowExtra : 0));
    for (std::size_t i = 0; i < s.saves.size(); ++i) {
        if (s.saves[i].folder == s.save) {  // the chosen one under the cursor
            s.save_first = WindowOf(i);
            s.focus = static_cast<std::uint32_t>(1 + i - s.save_first);
        }
    }
}

// The list page, with the cursor on a save (or on "New save" when it is not there).
void ShowInList(State& s, std::string_view folder) {
    s.page = Page::saves;
    s.delete_armed = false;
    s.focus = static_cast<std::uint32_t>(1 + (s.saves.empty() ? kRowExtra : 0));
    if (s.save_first >= s.saves.size()) {
        s.save_first = s.saves.empty() ? 0 : WindowOf(s.saves.size() - 1);
    }
    for (std::size_t i = 0; i < s.saves.size(); ++i) {
        if (s.saves[i].folder == folder) {
            s.save_first = WindowOf(i);
            s.focus = static_cast<std::uint32_t>(1 + i - s.save_first);
        }
    }
}

Action ActivateSaves(State& s, std::size_t row) {
    s.save_note.clear();
    if (row < kSavesShown) {
        const std::size_t at = s.save_first + row;
        if (at >= s.saves.size()) {
            return Action::none;
        }
        s.save_shown = s.saves[at].folder;
        s.page = Page::save;
        s.delete_armed = false;
        s.focus = 1;
        return Action::redraw;
    }
    if (row == kSavesShown) {
        if (s.saves.size() <= kSavesShown) {
            return Action::none;
        }
        s.save_first = s.save_first + kSavesShown >= s.saves.size() ? 0 : s.save_first + kSavesShown;
        return Action::redraw;
    }
    if (row == kRowExtra) {
        BeginEdit(s, Field::save_new);
        return Action::begin_edit;
    }
    if (row == kRowGo) {
        if (!s.solo_save) {
            s.save_note = W(s.lang, kNoSolo);
            return Action::redraw;
        }
        s.pending = {SaveOp::copy_solo, {}, FreeSaveName(s, kSoloCopyBase)};
        return Action::save_op;
    }
    if (row == kRowBack) {
        s.page = s.saves_from == Page::saves || s.saves_from == Page::save ? Page::main : s.saves_from;
        s.focus = static_cast<std::uint32_t>(1 + kRowExtra);  // the row that led here
        return Action::redraw;
    }
    return Action::none;
}

Action ActivateSave(State& s, std::size_t row) {
    s.save_note.clear();
    const SaveInfo* shown = FindSave(s, s.save_shown);
    if (shown == nullptr) {
        ShowInList(s, {});
        return Action::redraw;
    }
    const bool armed = s.delete_armed;
    s.delete_armed = false;
    switch (row) {
    case 0:
        if (!s.saves_on) {
            return armed ? Action::redraw : Action::none;
        }
        // Chosen: on to the Host page (the one the saves were opened from), the cursor on Start.
        s.save = shown->folder;
        s.page = s.saves_from == Page::steam_host ? Page::steam_host : Page::host;
        s.focus = static_cast<std::uint32_t>(1 + kRowGo);
        s.address_copied = false;
        if (!Busy(s.phase)) {
            s.message.clear();
            s.phase = Phase::idle;
        }
        return Action::save_chosen;
    case 1:
        if (shown->received) {
            return armed ? Action::redraw : Action::none;
        }
        BeginEdit(s, Field::save_rename);
        return Action::begin_edit;
    case kRowGo:
        if (shown->folder == s.running_save) {
            s.save_note = T(s.lang, Text::save_in_use);
            return Action::redraw;
        }
        if (!armed) {
            s.delete_armed = true;  // once more to confirm
            return Action::redraw;
        }
        s.pending = {SaveOp::remove, shown->folder, {}};
        return Action::save_op;
    case kRowBack: {
        const std::string folder = shown->folder;
        ShowInList(s, folder);
        return Action::redraw;
    }
    default: return armed ? Action::redraw : Action::none;  // the rows that only tell
    }
}

}  // namespace

void SetSaves(State& s, std::vector<SaveInfo> saves) {
    s.saves = std::move(saves);
    if (FindSave(s, s.save) == nullptr) {
        s.save.clear();
    }
    if (s.page == Page::save && FindSave(s, s.save_shown) == nullptr) {
        ShowInList(s, {});
    }
    if (s.save_first >= s.saves.size()) {
        s.save_first = s.saves.empty() ? 0 : WindowOf(s.saves.size() - 1);
    }
}

PendingOp TakePending(State& s) {
    PendingOp out;
    std::swap(out, s.pending);
    return out;
}

void OpDone(State& s, const PendingOp& op, bool ok, std::string_view folder, std::vector<SaveInfo> saves,
            std::string_view error) {
    s.delete_armed = false;
    const std::string now(folder);
    if (ok && op.op == SaveOp::rename) {
        // The same save under its new name, before the old one is looked for.
        if (s.save == op.folder) {
            s.save = now;
        }
        if (s.save_shown == op.folder) {
            s.save_shown = now;
        }
    }
    SetSaves(s, std::move(saves));
    if (!ok) {
        s.save_note = error.empty() ? std::string(T(s.lang, Text::save_op_failed)) : std::string(error);
        return;
    }
    s.save_note.clear();
    switch (op.op) {
    case SaveOp::create:
    case SaveOp::copy_solo:
        // A save just made is the one to play on.
        if (FindSave(s, now) != nullptr && s.saves_on) {
            s.save = now;
        }
        if (s.saves_from == Page::host || s.saves_from == Page::steam_host) {
            s.page = s.saves_from;
            s.focus = static_cast<std::uint32_t>(1 + kRowExtra);
        } else {
            ShowInList(s, now);
        }
        break;
    case SaveOp::remove:
        ShowInList(s, {});
        break;
    default: break;
    }
}

bool Focused(State& s, std::size_t row) {
    if (s.delete_armed && !(s.page == Page::save && row == kRowGo)) {
        s.delete_armed = false;
        return true;
    }
    return false;
}

Action Activate(State& s, std::size_t row) {
    if (s.editing != Field::none) {
        return Action::none;
    }
    const bool running = Busy(s.phase);
    if (s.page == Page::saves) {
        return ActivateSaves(s, row);
    }
    if (s.page == Page::save) {
        return ActivateSave(s, row);
    }
    if (SteamPages::List(s.page)) {
        return SteamPages::Activate(s, row);  // every row of a list page is its own
    }
    if (row == kRowExtra) {
        // The multiplayer saves, from any of the three pages.
        if (!s.saves_on) {
            return Action::none;
        }
        OpenSaves(s, s.page);
        return Action::redraw;
    }
    if (SteamPages::Is(s.page)) {
        return SteamPages::Activate(s, row);
    }
    if (s.page == Page::main) {
        switch (row) {
        case 0:
        case 2: return SteamPages::Open(s, row == 0);
        case 1:
        case 3:
            s.page = row == 1 ? Page::host : Page::join;
            s.focus = 1;
            if (!running) {
                s.message.clear();
                s.phase = Phase::idle;
            }
            s.address_copied = false;
            return Action::redraw;
        case kRowBack: return Action::close;
        default: return Action::none;  // the status: nothing to do
        }
    }
    if (row == kRowBack) {
        s.focus = s.page == Page::host ? 2 : 4;  // the entry that led here
        s.page = Page::main;
        if (!running) {
            s.message.clear();
            s.phase = Phase::idle;
        }
        return Action::redraw;
    }
    if (row == kRowGo) {
        if (running) {
            if (s.phase == Phase::hosting || s.phase == Phase::joined) {
                s.message = T(s.lang, Text::already_running);
                return Action::redraw;
            }
            return Action::none;  // on its way
        }
        std::string why;
        const bool ready = s.page == Page::host ? ReadyToHost(s, why) : ReadyToJoin(s, why);
        if (!ready) {
            s.phase = Phase::failed;
            s.message = why;
            return Action::redraw;
        }
        s.message.clear();
        return s.page == Page::host ? Action::start_host : Action::start_join;
    }
    if (s.page == Page::host && row == 3) {
        if (!s.addresses.empty()) {
            s.address_shown = (s.address_shown + (s.address_copied ? 1 : 0)) % s.addresses.size();
        }
        s.address_copied = true;
        return Action::copy_address;
    }
    const Field field = FieldAt(s.page, row);
    if (field == Field::none || running) {
        return Action::none;
    }
    BeginEdit(s, field);
    return Action::begin_edit;
}

bool TakeText(State& s, std::size_t row, std::string_view text) {
    const Field field = FieldAt(s.page, row);
    if (field == Field::none || s.editing != Field::none || (Busy(s.phase) && !SaveField(field))) {
        return false;
    }
    if (field == Field::save_rename) {
        const SaveInfo* shown = FindSave(s, s.save_shown);
        if (shown == nullptr || shown->received) {
            return false;
        }
    }
    BeginEdit(s, field);
    s.edit = std::string(text);
    return CommitEdit(s);
}

void Closed(State& s) {
    s.page = Page::main;
    s.focus = 0;
    s.editing = Field::none;
    s.edit.clear();
    s.address_copied = false;
    s.delete_armed = false;
    s.save_note.clear();
    s.save_shown.clear();
    s.pending = {};
    s.steam.note.clear();
    if (!Busy(s.phase)) {
        s.phase = Phase::idle;
        s.message.clear();
    }
}

// --- Typing ---

void BeginEdit(State& s, Field field) {
    s.editing = field;
    s.delete_armed = false;
    s.save_note.clear();
    if (SaveField(field)) {
        // A save's name: nothing of the session's is touched.
        const SaveInfo* shown = FindSave(s, s.save_shown);
        s.edit = field == Field::save_new ? FreeSaveName(s, kNewSaveBase)
                 : shown != nullptr       ? shown->name
                                          : std::string();
        return;
    }
    s.message.clear();
    if (s.phase == Phase::failed) {
        s.phase = Phase::idle;
    }
    switch (field) {
    case Field::name: s.edit = s.name; break;
    case Field::address: s.edit = s.address; break;
    case Field::port: s.edit = s.port; break;
    case Field::steam_name:
    case Field::room: s.edit = SteamPages::EditStart(s, field); break;
    default: s.edit.clear(); break;  // a password is typed anew, never shown again
    }
}

void EditText(State& s, std::uint32_t codepoint) {
    if (s.editing == Field::none) {
        return;
    }
    if (s.editing == Field::port && !(codepoint >= '0' && codepoint <= '9')) {
        return;
    }
    if (!NameLike(s.editing) && (codepoint == ' ' || codepoint > 0x7e)) {
        return;  // addresses and passwords: printable ASCII without spaces
    }
    if (!NameLike(s.editing) && s.edit.size() >= MaxOf(s.editing)) {
        return;
    }
    Ui::Utf8Append(s.edit, codepoint, MaxOf(s.editing));
}

void EditBackspace(State& s) {
    if (s.editing != Field::none) {
        Ui::Utf8PopBack(s.edit);
    }
}

void EditPad(State& s, PadKey key) {
    if (s.editing == Field::none) {
        return;
    }
    const std::string_view alphabet = Alphabet(s.editing);
    if (key == PadKey::left) {
        Ui::Utf8PopBack(s.edit);
        return;
    }
    if (key == PadKey::right) {
        if (Ui::Utf8Count(s.edit) < MaxOf(s.editing)) {
            // The same character again when there is one: "11", "aa" are then one step away.
            const bool ascii = !s.edit.empty() && static_cast<unsigned char>(s.edit.back()) < 0x80;
            s.edit.push_back(ascii ? s.edit.back() : alphabet.front());
        }
        return;
    }
    if (s.edit.empty()) {
        s.edit.push_back(alphabet.front());
        return;
    }
    const char last = s.edit.back();
    auto at = alphabet.find(last);
    if (static_cast<unsigned char>(last) >= 0x80) {
        Ui::Utf8PopBack(s.edit);  // a typed character outside the alphabet is replaced
        s.edit.push_back(alphabet.front());
        return;
    }
    if (at == std::string_view::npos) {
        at = 0;
    } else if (key == PadKey::up) {
        at = (at + 1) % alphabet.size();
    } else {
        at = (at + alphabet.size() - 1) % alphabet.size();
    }
    s.edit.back() = alphabet[at];
}

bool CommitEdit(State& s) {
    const Field field = s.editing;
    const std::string text(Trim(s.edit));
    s.editing = Field::none;
    s.edit.clear();
    switch (field) {
    case Field::name:
        if (!ValidName(text)) {
            s.message = W(s.lang, kBadName);
            return false;
        }
        s.name = text;
        return true;
    case Field::port:
        if (!ValidPort(text)) {
            s.message = W(s.lang, kBadPort);
            return false;
        }
        s.port = std::to_string(std::strtol(text.c_str(), nullptr, 10));
        return true;
    case Field::address: {
        if (text.empty()) {
            s.address.clear();
            return true;
        }
        const auto parsed = ParseJoinAddress(text);
        if (!parsed) {
            s.message = W(s.lang, kBadAddress);
            return false;
        }
        s.address = parsed->server;
        if (!parsed->port.empty()) {
            s.port = parsed->port;  // "address:port" sets both, as in the launcher
        }
        return true;
    }
    case Field::host_password:
    case Field::join_password: {
        std::string& password = field == Field::host_password ? s.host_password : s.join_password;
        if (text.empty()) {
            password.clear();
            return true;
        }
        if (!ValidPassword(text)) {
            s.message = W(s.lang, kBadPassword);
            return false;
        }
        password = text;
        return true;
    }
    case Field::steam_name:
    case Field::room: return SteamPages::Commit(s, field, text);
    case Field::save_new:
    case Field::save_rename: {
        const SaveInfo* shown = FindSave(s, s.save_shown);
        if (field == Field::save_rename && (shown == nullptr || shown->received)) {
            return false;
        }
        if (field == Field::save_rename && text == shown->name) {
            return true;  // as it was
        }
        if (!MpSaves::ValidName(text)) {
            s.save_note = W(s.lang, kBadSaveName);
            return false;
        }
        // One folder to Windows whatever the capitals; a rename may change only those.
        const std::string folded = MpSaves::Folded(text);
        for (const SaveInfo& save : s.saves) {
            if (MpSaves::Folded(save.folder) == folded &&
                !(field == Field::save_rename && save.folder == shown->folder)) {
                s.save_note = W(s.lang, kSaveExists);
                return false;
            }
        }
        s.pending = field == Field::save_new ? PendingOp{SaveOp::create, {}, text}
                                             : PendingOp{SaveOp::rename, shown->folder, text};
        return true;
    }
    default: return false;
    }
}

void CancelEdit(State& s) {
    s.editing = Field::none;
    s.edit.clear();
}

const char* T(Lang lang, Text text) {
    return W(lang, kTexts[static_cast<std::size_t>(text)]);
}

}  // namespace CoopNx2navi::Menu
