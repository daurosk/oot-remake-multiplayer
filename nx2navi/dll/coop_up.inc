// SPDX-License-Identifier: GPL-3.0-or-later
// Included by coop_dll.cpp in the build that runs upstream's hook (upstream.hpp, COOP_UPSTREAM): the
// session is lichii/hook's own (CoopHook), and this class is only what this port adds on top of it:
// the panel, chat box, minimap dots and name tags, the title page's state, the save transfer's host
// side, the join at the host and the follow through exits, the way back after a Game Over.

class ApiGuest final : public CoopPuppet::Guest {
public:
    bool Read(std::uint64_t address, void* out, std::size_t size) override {
        const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
        return size == 0 || (api != nullptr && address >= 0x10000 && api->read(address, out, size) == 1);
    }
    bool Write(std::uint64_t address, const void* in, std::size_t size) override {
        const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
        return size == 0 || (api != nullptr && address >= 0x10000 && api->write(address, in, size) == 1);
    }
};

class Coop {
public:
    explicit Coop(CoopNx2navi::Settings settings, std::string welcome = {})
        : settings_(std::move(settings)), welcome_(std::move(welcome)) {}

    bool Begin() {
        CoopNx2navi::Up::Hooks hooks;
        hooks.frame = [](const std::optional<coop::host::PlayerReading>& reading, coop::host::Session* session,
                         void* user) { static_cast<Coop*>(user)->Frame(reading, session); };
        hooks.event = [](const coop::net::Event& event, coop::net::Client&, void* user) {
            static_cast<Coop*>(user)->OnEvent(event);
        };
        hooks.user = this;
        CoopNx2navi::Up::Configure(settings_, g_folder, LogLine, hooks);
        {
            CoopNx2navi::Teleport::Auto::Options automatic;
            automatic.join_at_host = settings_.join_at_host;
            automatic.follow =
                settings_.follow == CoopNx2navi::Settings::Follow::host
                    ? CoopNx2navi::Teleport::FollowMode::host
                    : settings_.follow == CoopNx2navi::Settings::Follow::anyone
                          ? CoopNx2navi::Teleport::FollowMode::anyone
                          : CoopNx2navi::Teleport::FollowMode::off;
            CoopNx2navi::Teleport::Configure(automatic, LogLine);
            CoopNx2navi::Teleport::TestGoto(Utf8(Environment(L"OOT_COOP_TEST_GOTO")));
            CoopNx2navi::Teleport::TestScene(Utf8(Environment(L"OOT_COOP_TEST_SCENE")));
            debug_position_ = Environment(L"OOT_COOP_DEBUG_POS") == L"1";
            puppet_voice_ = Environment(L"OOT_COOP_PUPPET_VOICE") != L"0";
            debug_own_sounds_ = Environment(L"OOT_COOP_DEBUG_OWN_SOUNDS") == L"1";
            test_goto_file_ = std::filesystem::path(Environment(L"OOT_COOP_TEST_GOTO_FILE"));
            CoopNx2navi::Teleport::SetArriveEvent(Environment(L"OOT_COOP_TELEPORT_ARRIVE") == L"1");
        }
        // The line the launcher's menu looks for ("coop: on, server").
        g_log.Line("on, server %s:%u as '%s', join password %s, shared progress %s (the hook's own session)",
                   settings_.coop.server.c_str(), static_cast<unsigned>(settings_.coop.port),
                   settings_.coop.name.c_str(), settings_.coop.password.empty() ? "none" : "set",
                   settings_.share_progress ? "on" : "off (share_progress = 0)");
        g_state = 1;
        CoopNx2navi::Overlay::SetMinimap(settings_.minimap, Environment(L"OOT_COOP_DEBUG_MINIMAP") == L"1");
        CoopNx2navi::Overlay::SetNameTags(settings_.name_tags, Environment(L"OOT_COOP_DEBUG_TAGS") == L"1");
        {
            std::string spec = Utf8(Environment(L"OOT_COOP_TEST_LIFE"));
            while (!spec.empty()) {
                const auto end = spec.find(';');
                const std::string part = spec.substr(0, end);
                spec = end == std::string::npos ? std::string() : spec.substr(end + 1);
                double at = 0.0;
                int value = 0;
                if (std::sscanf(part.c_str(), "%lf:%d", &at, &value) == 2 && at >= 0.0 && value >= 0) {
                    test_life_.push_back({static_cast<std::uint64_t>(at * 1e6), value});
                }
            }
        }
        if (settings_.overlay) {
            CoopNx2navi::Overlay::Want();
        } else {
            g_log.Line("overlay: off (overlay = 0 in coop.ini)");
        }
        return true;
    }

    void Run() {
        try {
            auto next = Clock::now();
            while (!g_stop) {
                Tick();
                next += kFrame;
                const auto now = Clock::now();
                if (next < now) {
                    next = now;
                }
                if (SleepUntil(next)) {
                    break;
                }
            }
        } catch (const std::exception& e) {
            g_log.Line("frame thread stopped: %s", e.what());
        }
        g_stop = true;
        CoopNx2navi::Overlay::Shutdown();
        CoopNx2navi::Up::Stop();  // CoopHook::Shutdown: the goodbye to the server
        g_state = 0;
        g_log.Line("stopped");
    }

private:
    // Our own thread, 60 times a second: waits for the game's interface, then only serves the save transfer.
    void Tick() {
        const auto now = Clock::now();
        EnsureTitleEntry();
        if (!CoopNx2navi::Up::Attached() && !refused_ && now >= next_look_) {
            next_look_ = now + std::chrono::milliseconds(250);
            const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
            const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
            const std::string_view told = info != nullptr && info->build_id != nullptr ? info->build_id : "";
            if (info != nullptr && info->main_base != 0 && told.size() >= 16) {
                const std::string_view want = CoopGameBuild::kPrimary.main_id;
                bool same = true;
                for (std::size_t i = 0; i < std::min(told.size(), want.size()); ++i) {
                    same = same && std::toupper(static_cast<unsigned char>(told[i])) == want[i];
                }
                if (!same) {
                    refused_ = true;
                    g_log.Line("%s", CoopGameBuild::LogRefusal(std::string(told)).c_str());
                } else {
                    main_base_ = info->main_base;
                    trusted_ = true;
                    g_log.Line("game found: main at %#llx, build id %.*s (told by the game's plugin interface)",
                               static_cast<unsigned long long>(info->main_base),
                               static_cast<int>(std::min<std::size_t>(told.size(), 40)), told.data());
                    refused_ = !CoopNx2navi::Up::Attach(api, main_base_);
                }
            } else if (++looks_ == 240) {
                g_log.Line("the game's plugin interface has not shown yet (an NX2navi build from before it cannot "
                           "run this mod); still waiting");
            }
        }
        coop::host::Session* session = session_.load();
        if (session != nullptr) {
            CoopNx2navi::SaveSync::Frame(*session, connected_.load(), SaveDir());
        }
    }

    // The game's presenting thread, once per presented frame, inside the hook's own frame (after its
    // session's frame and its objects, before its PvP).
    void Frame(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session* session) {
        std::lock_guard lock(frame_mutex_);
        const auto now = Clock::now();
        if (session != nullptr) {
            session_ = session;
        }
        session = session_.load();
        const bool connected = session != nullptr && session->with_client([](coop::net::Client& c) {
            return c.state() == coop::net::State::connected;
        });
        connected_ = connected;
        const std::uint64_t server_now =
            connected ? session->with_client([](coop::net::Client& c) { return c.server_now_us(); }) : 0;
        {
            ApiGuest guest;
            GuestView memory(guest, main_base_);
            coop::host::PlayerReader reader(memory, &drawn_);
            WatchLife(memory, reader, reading.has_value());
        }
        if (reading.has_value() != had_reading_ || (reading && reading->scene != scene_ && reading->scene)) {
            had_reading_ = reading.has_value();
            if (reading) {
                scene_ = reading->scene;
                g_log.Line("player found in %s at (%.1f, %.1f, %.1f)", scene_ ? scene_->c_str() : "an unknown scene",
                           static_cast<double>(reading->position[0]), static_cast<double>(reading->position[1]),
                           static_cast<double>(reading->position[2]));
            } else {
                g_log.Line("player not readable (a load, the title screen or a menu)");
            }
        }
        if (debug_position_ && reading && now >= next_position_) {
            next_position_ = now + std::chrono::seconds(1);
            double fx = 0.0, fz = 0.0;
            if (reading->rotation) {
                const auto& q = *reading->rotation;
                fx = 2.0 * (static_cast<double>(q[0]) * q[2] + static_cast<double>(q[3]) * q[1]);
                fz = 1.0 - 2.0 * (static_cast<double>(q[0]) * q[0] + static_cast<double>(q[1]) * q[1]);
            }
            g_log.Line("pos: %s %.1f %.1f %.1f%s, life %d, facing %.2f %.2f",
                       reading->scene ? reading->scene->c_str() : "?", static_cast<double>(reading->position[0]),
                       static_cast<double>(reading->position[1]), static_cast<double>(reading->position[2]),
                       reading->event_running.value_or(false) ? " (event)" : "", life_, fx, fz);
        }
        if (connected && now >= next_status_) {
            next_status_ = now + kStatusEvery;
            Status(reading, *session, server_now);
        }
        // For tests (OOT_COOP_TEST_GOTO_FILE=<file>): a place written into the file while the game runs
        // ("x,y,z" or "x,y,z,yaw") is where our Link is put, once; the file is then removed. A test's own
        // script writes it from what the log says (where a drop lies).
        if (!test_goto_file_.empty() && now >= next_goto_file_) {
            next_goto_file_ = now + std::chrono::milliseconds(250);
            std::error_code ec;
            if (std::filesystem::exists(test_goto_file_, ec)) {
                std::string place;
                {
                    std::ifstream in(test_goto_file_);
                    std::getline(in, place);
                }
                std::filesystem::remove(test_goto_file_, ec);
                if (!place.empty() && !ec) {
                    g_log.Line("test: a place from the file: %s", place.c_str());
                    CoopNx2navi::Teleport::TestGoto("0:" + place);
                }
            }
        }
        if (session != nullptr) {
            CoopNx2navi::Teleport::Frame(reading, *session, connected, server_now, true);
            SendExtras(*session, connected, reading.has_value());
        }
        for (const std::string& line : CoopNx2navi::Teleport::TakeLines()) {
            CoopNx2navi::Overlay::Notice(line);
        }
        if (CoopNx2navi::Overlay::Active()) {
            for (const std::string& text : CoopNx2navi::Overlay::TakeOutbox()) {
                // The hook does its own commands (/teleport, /pvp) and sends the rest.
                if (text.rfind("/", 0) == 0 ? CoopNx2navi::Up::SendChat(text) || true
                                           : CoopNx2navi::Up::SendChat(text)) {
                    if (text.rfind("/", 0) != 0) {
                        const std::string line = settings_.coop.name + ": " + text;
                        g_log.Line("chat: %s", line.c_str());
                        CoopNx2navi::Overlay::Notice(line, true);
                    }
                } else {
                    CoopNx2navi::Overlay::Notice("(not sent: not connected, or the text was refused)");
                }
            }
            PublishOverlay(reading, session, connected, server_now);
        }
    }

    void PublishOverlay(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session* session,
                        bool connected, std::uint64_t now) {
        namespace Ui = CoopNx2navi::Ui;
        {
            // The panel's own texts in the game's language (the title page's: the game's, or the one chosen).
            const bool french = CoopNx2navi::TitleMenu::Language() == CoopNx2navi::Menu::Lang::fr;
            if (french != Ui::French() || !panel_language_said_) {
                panel_language_said_ = true;
                g_log.Line("panel: its texts are %s", french ? "French" : "English");
            }
            Ui::SetFrench(french);
        }
        Ui::Snapshot s;
        s.link = static_cast<Ui::Link>(link_.load());
        s.server = settings_.coop.server + ":" + std::to_string(settings_.coop.port);
        s.self_name = settings_.coop.name;
        {
            std::lock_guard lock(names_mutex_);
            s.refused = refused_reason_;
        }
        s.local.game_found = trusted_;
        s.local.in_game = reading.has_value();
        if (reading) {
            s.local.scene = reading->scene;
            s.local.position = reading->position;
            s.local.event = reading->event_running.value_or(false);
            s.local.child = reading->age == coop::protocol::Age::child;
            s.local.max_life = max_life_;
        }
        if (session != nullptr) {
            struct Room {
                std::uint8_t self = 0;
                std::vector<coop::net::Player> players;
            };
            Room room = session->with_client([](coop::net::Client& client) {
                return Room{client.player_number(), client.players()};
            });
            s.self = room.self;
            if (!connected) {
                room.players.clear();
            }
            session->with_presence([&](coop::presence::Presence& presence) {
                for (const auto& player : room.players) {
                    if (player.self) {
                        continue;
                    }
                    Ui::Peer peer;
                    peer.number = player.number;
                    peer.name = player.nickname;
                    if (const auto* p = presence.player(player.number)) {
                        peer.known = true;
                        peer.scene = p->scene();
                        if (!p->samples().empty()) {
                            const std::uint64_t last = p->samples().back().time.server_time_us;
                            peer.fresh = now >= last ? now - last <= Ui::kStaleUs : true;
                        }
                        if (const auto pose = presence.shown(player.number, now)) {
                            peer.position = pose->position;
                        }
                    }
                    // Their hearts (the hook keeps each player's last PLAYER_LIFE), and when they lost some.
                    if (const auto life = CoopHud::PlayerLife(player.number);
                        life && life->max > 0 && player.number < peer_life_.size()) {
                        PeerLife& seen = peer_life_[player.number];
                        const std::uint64_t at = SteadyUs() / 1000;
                        if (seen.known && life->life < seen.life) {
                            seen.hurt_ms = at;
                            CoopNx2navi::Up::PuppetHurt(player.number);  // the puppet's model turns red
                            // Their hurt voice, from their puppet (the game's three: Damage_S_01 to _03),
                            // unless their game sends its own Link's sounds (then the real one comes).
                            if (puppet_voice_ && !sound_feed_[player.number].load()) {
                                static const char* const kHurt[3] = {"Damage_S_01", "Damage_S_02", "Damage_S_03"};
                                CoopNx2navi::Up::PuppetSound(player.number, kHurt[hurt_voice_++ % 3]);
                            }
                            g_log.Line("hearts: P%u %s lost life: %d -> %d of %d quarter hearts",
                                       static_cast<unsigned>(player.number), player.nickname.c_str(), seen.life,
                                       static_cast<int>(life->life), static_cast<int>(life->max));
                        }
                        seen.known = true;
                        seen.life = life->life;
                        peer.life = life->life;
                        peer.max_life = life->max;
                    }
                    if (player.number < peer_magic_.size()) {
                        const std::uint32_t magic = peer_magic_[player.number].load();
                        if (magic != 0) {
                            peer.magic = static_cast<int>(magic & 0xffff);
                            peer.max_magic = static_cast<int>(magic >> 16);
                        }
                    }
                    s.peers.push_back(std::move(peer));
                }
            });
        }
        CoopNx2navi::Overlay::Publish(std::move(s));
    }

    // Our own Link's sounds and magic for the other games.
    void SendExtras(coop::host::Session& session, bool connected, bool in_game) {
        namespace Fx = CoopNx2navi::PuppetFx;
        const std::vector<std::string> sounds = CoopNx2navi::Up::TakeOwnSounds();
        const std::uint64_t now_ms = SteadyUs() / 1000;
        std::optional<std::vector<std::uint8_t>> magic;
        if (in_game && now_ms - magic_read_ms_ >= 250) {
            magic_read_ms_ = now_ms;
            ApiGuest guest;
            GuestView memory(guest, main_base_);
            coop::host::GameFlags flags(memory);
            const auto mp = flags.get_int(0x76b18200), most = flags.get_int(0x12d63a97);  // PlayerStatus.MP, .MPMax
            if (mp && most && *most >= 0 && *most <= 0xffff && *mp >= 0) {
                const std::uint32_t packed = (static_cast<std::uint32_t>(*most) << 16) |
                                             static_cast<std::uint32_t>(std::min<std::int32_t>(*mp, *most));
                // Again every 5 s: a player who joined later has it soon.
                if (packed != magic_sent_ || now_ms - magic_sent_ms_ >= 5000) {
                    magic_sent_ = packed;
                    magic_sent_ms_ = now_ms;
                    magic = Fx::EncodeMagic(*mp, *most);
                }
            }
        }
        if (!connected || (sounds.empty() && !magic)) {
            return;
        }
        session.with_client([&](coop::net::Client& client) {
            for (const std::string& name : sounds) {
                if (sound_budget_.Take(now_ms)) {
                    client.send(coop::protocol::PlayerExtra{coop::protocol::kPlayerSelf, Fx::kExtraSound,
                                                            std::vector<std::uint8_t>(name.begin(), name.end())});
                }
            }
            if (magic) {
                client.send(coop::protocol::PlayerExtra{coop::protocol::kPlayerSelf, Fx::kExtraMagic, *magic});
            }
        });
    }

    // Our Link's life (quarter hearts): the test writes, and the way back after a Game Over.
    void WatchLife(coop::host::WritableGameMemory& memory, coop::host::PlayerReader& reader, bool in_game) {
        if (!in_game) {
            return;
        }
        const auto actor = reader.find_player_actor();
        {
            // Our Link's sound user, for the hook that hears what it starts; the sounds since the last
            // frame and our magic go to the other games (PLAYER_EXTRA, puppet_fx.hpp).
            ApiGuest guest;
            CoopNx2navi::Up::SetOwnSoundUser(actor ? CoopPuppetSound::SoundUser(guest, *actor) : 0);
        }
        // For tests (OOT_COOP_DEBUG_OWN_SOUNDS=1): what our own Link's sound user plays, said when it
        // changes (the hook's own reader, which it uses for a puppet with LICHII_COOP_DEBUG_ANIM=1).
        if (debug_own_sounds_ && actor) {
            ApiGuest guest;
            own_sound_.playing_ms = 0;  // every frame: a hurt voice is short
            CoopPuppetSound::LogPlaying(own_sound_, guest, *actor, SteadyUs() / 1000 + 1000, "own Link",
                                        [](const std::string& line) { g_log.Line("%s", line.c_str()); });
        }
        const auto table = actor ? coop::host::read_ptr(memory, *actor + 0x270) : std::nullopt;
        const auto life = table ? coop::host::read_ptr(memory, *table + 0x2b8) : std::nullopt;
        const auto value = life ? coop::host::read_ptr(memory, *life + 0x1c10) : std::nullopt;
        const auto most = life ? coop::host::read_ptr(memory, *life + 0x1c18) : std::nullopt;
        const auto now = value ? coop::host::read_u32(memory, *value + 8) : std::nullopt;
        const auto max = most ? coop::host::read_u32(memory, *most + 8) : std::nullopt;
        if (!now || !max || static_cast<std::int32_t>(*max) <= 0 || static_cast<std::int32_t>(*max) > 4 * 40) {
            life_ = -1;
            return;
        }
        life_ = static_cast<std::int32_t>(*now);
        max_life_ = static_cast<std::int32_t>(*max);
        if (!test_life_.empty()) {
            const std::uint64_t at = SteadyUs();
            if (in_game_us_ == 0) {
                in_game_us_ = at;
            }
            if (at - in_game_us_ >= test_life_.front().after_us) {
                const std::uint32_t to = static_cast<std::uint32_t>(test_life_.front().value);
                std::uint8_t bytes[4];
                std::memcpy(bytes, &to, 4);
                const bool ok = memory.write(*value + 8, bytes);
                g_log.Line("test: our Link's life %d -> %u (%s)", life_, to, ok ? "written" : "NOT written");
                test_life_.erase(test_life_.begin());
            }
        }
        if (life_ <= 0 && !dead_) {
            dead_ = true;
            g_log.Line("our Link has no life left (Game Over); after the reload this game goes back to the "
                       "player it follows");
            CoopNx2navi::Teleport::Died();
        } else if (life_ > 0) {
            dead_ = false;
        }
    }

    const std::filesystem::path& SaveDir() {
        if (save_dir_.empty()) {
            const Nx2PluginApi* api = CoopNx2navi::Overlay::Api();
            const Nx2GameInfo* info = api != nullptr ? api->game_info() : nullptr;
            if (info != nullptr && NX2_HAS(info, Nx2GameInfo, save_dir) && info->save_dir != nullptr &&
                *info->save_dir != 0) {
                save_dir_ = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(info->save_dir)));
                g_log.Line("save: the game's save folder is %s", info->save_dir);
            }
        }
        return save_dir_;
    }

    void Status(const std::optional<coop::host::PlayerReading>& reading, coop::host::Session& session,
                std::uint64_t server_now) {
        struct Room {
            std::uint8_t self = 0;
            std::optional<std::uint64_t> round_trip_us;
            std::vector<coop::net::Player> players;
            bool host = false;
        };
        const Room room = session.with_client([](coop::net::Client& client) {
            return Room{client.player_number(), client.round_trip_us(), client.players(), client.is_host()};
        });
        std::string line =
            "status: player " + std::to_string(room.self) + " of " + std::to_string(room.players.size());
        if (room.host) {
            line += " (host)";
        }
        if (room.round_trip_us) {
            line += ", ping " + std::to_string(*room.round_trip_us / 1000) + " ms";
        }
        line += reading ? ", you: " + (reading->scene ? *reading->scene : std::string("unknown scene"))
                        : std::string(", you: not in the game");
        for (const auto& player : room.players) {
            if (player.self) {
                continue;
            }
            struct Seen {
                bool known = false;
                std::optional<std::string> scene;
                std::optional<coop::presence::Pose> pose;
            };
            const Seen seen = session.with_presence([&](coop::presence::Presence& presence) {
                Seen s;
                if (const auto* p = presence.player(player.number)) {
                    s.known = true;
                    s.scene = p->scene();
                    s.pose = presence.shown(player.number, server_now);
                }
                return s;
            });
            line += "; P" + std::to_string(player.number) + " " + player.nickname + ": ";
            if (!seen.known) {
                line += "joining";
            } else if (!seen.pose) {
                line += seen.scene ? *seen.scene + ", no position" : "no samples";
            } else {
                line += seen.scene ? *seen.scene : std::string("unknown scene");
                if (reading && reading->scene && seen.scene && *reading->scene == *seen.scene) {
                    const double dx = static_cast<double>(seen.pose->position[0] - reading->position[0]);
                    const double dy = static_cast<double>(seen.pose->position[1] - reading->position[1]);
                    const double dz = static_cast<double>(seen.pose->position[2] - reading->position[2]);
                    char buffer[48];
                    std::snprintf(buffer, sizeof(buffer), ", %.1f units away",
                                  std::sqrt(dx * dx + dy * dy + dz * dz));
                    line += buffer;
                }
            }
        }
        line += CoopNx2navi::Up::StatusText();
        g_log.Line("%s", line.c_str());
    }

    // What the client saw. Network thread, under the session's lock.
    void OnEvent(const coop::net::Event& event) {
        using coop::net::EventKind;
        switch (event.kind) {
        case EventKind::welcomed:
        case EventKind::reconnected: {
            {
                std::lock_guard lock(names_mutex_);
                names_ = {};
                for (const auto& p : event.welcome.players) {
                    if (p.number < names_.size()) {
                        names_[p.number] = p.nickname;
                    }
                }
            }
            g_state = 2;
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::connected);
            CoopNx2navi::Overlay::Notice(
                (event.kind == EventKind::reconnected ? "Connected again as P" : "Connected as P") +
                std::to_string(event.welcome.player_number));
            CoopNx2navi::TitleMenu::SetPhase(Hosting() ? CoopNx2navi::Menu::Phase::hosting
                                                       : CoopNx2navi::Menu::Phase::joined,
                                             welcome_, event.welcome.player_number);
            break;
        }
        case EventKind::refused: {
            g_state = 3;
            const std::string reason(coop::net::refuse_reason_text(event.refuse_reason));
            {
                std::lock_guard lock(names_mutex_);
                refused_reason_ = reason;
            }
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::refused);
            CoopNx2navi::Overlay::Notice("Refused by the server: " + reason);
            CoopNx2navi::TitleMenu::SetPhase(
                CoopNx2navi::Menu::Phase::failed,
                std::string(CoopNx2navi::Menu::T(CoopNx2navi::TitleMenu::Language(),
                                                 CoopNx2navi::Menu::Text::refused)) +
                    ":\n" + reason);
            break;
        }
        case EventKind::disconnected:
            g_state = 1;
            link_ = static_cast<int>(CoopNx2navi::Ui::Link::lost);
            CoopNx2navi::Overlay::Notice("Server lost; retrying");
            break;
        case EventKind::player_joined: {
            std::lock_guard lock(names_mutex_);
            if (event.player < names_.size()) {
                names_[event.player] = event.nickname;
            }
            CoopNx2navi::Overlay::Notice(event.nickname + " joined");
            break;
        }
        case EventKind::player_left:
            CoopNx2navi::Overlay::Notice(event.nickname + " left");
            break;
        case EventKind::chat: {
            const std::string line = Name(event.player) + ": " + event.text;
            g_log.Line("chat: %s", line.c_str());
            CoopNx2navi::Overlay::Notice(line, true);
            break;
        }
        case EventKind::message:
            if (const auto* extra = std::get_if<coop::protocol::PlayerExtra>(&event.message);
                extra != nullptr && extra->player_number != 0 && extra->player_number < peer_magic_.size()) {
                namespace Fx = CoopNx2navi::PuppetFx;
                if (extra->kind == Fx::kExtraSound) {
                    sound_feed_[extra->player_number] = true;
                    CoopNx2navi::Up::PuppetSound(extra->player_number,
                                                 std::string(extra->data.begin(), extra->data.end()));
                } else if (extra->kind == Fx::kExtraMagic) {
                    if (const auto m = Fx::DecodeMagic(extra->data); m && m->most > 0) {
                        peer_magic_[extra->player_number] =
                            (static_cast<std::uint32_t>(m->most) << 16) | static_cast<std::uint32_t>(m->now);
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    std::string Name(std::uint8_t number) {
        std::lock_guard lock(names_mutex_);
        if (number < names_.size() && !names_[number].empty()) {
            return names_[number];
        }
        return "P" + std::to_string(number);
    }

    bool Hosting() const {
        const std::string& server = settings_.coop.server;
        return !settings_.host_save && (server.rfind("127.", 0) == 0 || server == "localhost" || server == "::1");
    }

    CoopNx2navi::Settings settings_;
    std::string welcome_;
    std::atomic<coop::host::Session*> session_{nullptr};  // the hook's, alive until Up::Stop
    std::atomic<bool> connected_{false};
    std::atomic<bool> trusted_{false};
    std::uint64_t main_base_ = CoopNx2navi::kMainBase;
    std::mutex frame_mutex_;
    coop::host::PlayerReader::DrawnState drawn_{};
    bool refused_ = false;
    unsigned looks_ = 0;
    Clock::time_point next_look_{};
    Clock::time_point next_status_{};
    bool had_reading_ = false;
    bool panel_language_said_ = false;
    bool debug_position_ = false;
    std::filesystem::path test_goto_file_;
    struct PeerLife {
        bool known = false;
        int life = 0;
        std::uint64_t hurt_ms = 0;
    };
    std::array<PeerLife, 5> peer_life_{};
    std::array<std::atomic<std::uint32_t>, 5> peer_magic_{};  // most << 16 | now; 0 not told
    std::array<std::atomic<bool>, 5> sound_feed_{};           // that player's game sends its Link's sounds
    CoopNx2navi::PuppetFx::SoundBudget sound_budget_;
    std::uint64_t magic_read_ms_ = 0, magic_sent_ms_ = 0;
    std::uint32_t magic_sent_ = ~std::uint32_t{0};
    bool debug_own_sounds_ = false;
    bool puppet_voice_ = true;
    unsigned hurt_voice_ = 0;
    CoopPuppetSound::State own_sound_;
    Clock::time_point next_goto_file_{};
    Clock::time_point next_position_{};
    std::int32_t life_ = -1;
    std::int32_t max_life_ = 0;
    struct TestLife {
        std::uint64_t after_us = 0;
        int value = 0;
    };
    std::vector<TestLife> test_life_;
    std::uint64_t in_game_us_ = 0;
    bool dead_ = false;
    std::filesystem::path save_dir_;
    std::optional<std::string> scene_;
    std::mutex names_mutex_;
    std::array<std::string, 5> names_;
    std::string refused_reason_;
    std::atomic<int> link_{static_cast<int>(CoopNx2navi::Ui::Link::connecting)};
};
