#include "coop_window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <QApplication>
#include <QEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGraphicsDropShadowEffect>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "common/logging.h"
#include "coop_hook.h"
#include "coop_hud.h"
#include "coop_hud_hearts.h"
#if __has_include("coop_colors.h")
#include "coop_colors.h"  // each player's colour (branch puppet-colors)
#define COOP_WINDOW_HAS_COLORS 1
#endif

namespace CoopWindow {
namespace {

constexpr int kPad = 8;
constexpr int kMargin = 16;
constexpr int kRefreshMs = 200;
constexpr int kMarkerRefreshMs = 16;  // markers follow the camera
constexpr int kChatRefreshMs = 50;    // smooth enough for the fade
constexpr std::uint64_t kChatShowMs = 8000;
constexpr std::uint64_t kChatFadeMs = 1000;  // after kChatShowMs
constexpr int kChatWidth = 520;
constexpr int kChatMaxChars = 200;  // coop::protocol::kMaxChatChars

// ---- The game look (docs/overlay.md, "The look") ----
//
// The game's own font and heart pictures, made by make-puppet-pack from the
// player's game files (tools/puppet_pack/hud.py) and found by CoopHud::Folder.
// With them every co-op text is drawn like the game's HUD: no box, the game's
// font, a soft dark shadow. Without them (an old pack, LICHII_COOP_HUD=0) the
// window keeps its plain look: the panel, boxes, outlined name tags.

// The game's HUD is laid out for this picture height; sizes below are in its
// pixels and scale with the picture.
constexpr double kGameHeight = 720.0;
// The highlight colour of room notices ("Saria joined").
const QColor kHighlight(255, 206, 82);

struct GameLook {
    bool ok = false;
    QString regular;  // font families
    QString medium;
    std::array<QPixmap, 5> hearts;  // 0 to 4 quarters filled
};

const GameLook& Look() {
    static const GameLook look = [] {
        GameLook l;
        const auto dir = CoopHud::Folder();
        if (!dir) {
            return l;  // CoopHud logged why
        }
        const auto family = [&](const char* file) -> QString {
            const QString path = QString::fromStdWString((*dir / file).wstring());
            const int id = QFontDatabase::addApplicationFont(path);
            const QStringList names = id < 0 ? QStringList{} : QFontDatabase::applicationFontFamilies(id);
            return names.isEmpty() ? QString() : names.front();
        };
        l.regular = family("font-regular.otf");
        l.medium = family("font-medium.otf");
        bool hearts = true;
        for (int q = 0; q < 5; ++q) {
            const auto file = *dir / ("heart-" + std::to_string(q) + ".png");
            hearts = l.hearts[q].load(QString::fromStdWString(file.wstring())) && hearts;
        }
        if (l.medium.isEmpty()) {
            l.medium = l.regular;
        }
        l.ok = !l.regular.isEmpty() && hearts;
        if (l.ok) {
            LOG_INFO(Core, "coop: hud: game font '{}', hearts loaded", l.regular.toStdString());
        } else {
            LOG_WARNING(Core, "coop: hud: {} in {} could not be loaded; plain look",
                        l.regular.isEmpty() ? "the font" : "a heart picture", dir->string());
        }
        return l;
    }();
    return look;
}

bool Game() {
    return Look().ok;
}

// The old panel (ping, markers, distances): always in the plain look; in the
// game look only with LICHII_COOP_DEBUG_PANEL=1, or after F7.
bool& DebugPanel() {
    static bool on = [] {
        const char* v = std::getenv("LICHII_COOP_DEBUG_PANEL");
        return v != nullptr && std::string_view(v) == "1";
    }();
    return on;
}

QFont GameFont(bool medium, double pixels) {
    QFont f = QApplication::font();
    if (Game()) {
        f.setFamily(medium ? Look().medium : Look().regular);
        f.setWeight(QFont::Normal);
    } else {
        f.setWeight(medium ? QFont::Bold : QFont::DemiBold);
    }
    f.setPixelSize(std::max(8, static_cast<int>(std::lround(pixels))));
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

// Text as the game draws it on its HUD: the fill over a soft dark shadow,
// a little down and right, no box and no hard outline.
void DrawGameText(QPainter& p, const QPointF& at, const QString& text, const QFont& font,
                  const QColor& fill) {
    QPainterPath path;
    path.addText(at, font, text);
    const double a = fill.alphaF();
    const double d = std::max(1.0, font.pixelSize() / 16.0);
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    // A soft rim, then the drop shadow.
    p.setBrush(QColor(0, 0, 0, static_cast<int>(70 * a)));
    for (const QPointF o : {QPointF(-d, 0), QPointF(d, 0), QPointF(0, -d), QPointF(0, d)}) {
        p.drawPath(path.translated(o * 0.8));
    }
    p.setBrush(QColor(0, 0, 0, static_cast<int>(150 * a)));
    p.drawPath(path.translated(d, d * 1.3));
    p.setBrush(fill);
    p.drawPath(path);
    p.restore();
}

// A player's colour for names (puppet-colors' CoopColors when it is built
// in), lightened so it reads on the dark shadow. White when not known.
QColor PlayerColour(std::uint8_t number, std::uint8_t self) {
#ifdef COOP_WINDOW_HAS_COLORS
    if (number != 0) {
        const std::uint32_t rgb = CoopColors::ForPlayer(number, self);
        const auto lift = [](std::uint32_t c) { return static_cast<int>(c + (255 - c) * 35 / 100); };
        return QColor(lift((rgb >> 16) & 0xFF), lift((rgb >> 8) & 0xFF), lift(rgb & 0xFF));
    }
#else
    (void)number;
    (void)self;
#endif
    return QColor(255, 255, 255);
}

// Who is who, by name, for the name tags and chat lines (both carry the
// name only): {name: number}, and our own number. Refreshed by the callers
// a few times a second.
struct Names {
    std::map<std::string, std::uint8_t, std::less<>> numbers;
    std::uint8_t self = 0;

    void Refresh() {
        numbers.clear();
        self = 0;
        for (const auto& row : CoopHook::PlayerList()) {
            numbers[row.name] = row.number;
            if (row.self) {
                self = row.number;
            }
        }
    }
    std::uint8_t Of(std::string_view name) const {
        if (const auto it = numbers.find(name); it != numbers.end()) {
            return it->second;
        }
        if (name.size() == 2 && name[0] == 'P' && name[1] >= '1' && name[1] <= '4') {
            return static_cast<std::uint8_t>(name[1] - '0');  // a tag with no name
        }
        if (name == "you") {
            return self;
        }
        return 0;
    }
};

// Where the game's picture is inside `area`: it keeps its aspect (the
// scene target's when known, else 16:9), centred.
QRectF Picture(const QSizeF& area, double game_w = 16.0, double game_h = 9.0) {
    if (game_w <= 0 || game_h <= 0 || area.isEmpty()) {
        return QRectF(QPointF(0, 0), area);
    }
    const double s = std::min(area.width() / game_w, area.height() / game_h);
    return QRectF((area.width() - game_w * s) / 2, (area.height() - game_h * s) / 2, game_w * s,
                  game_h * s);
}

// Where an overlay window sits: on the game when it is shown, else on the
// main window.
QWidget* Anchor(QWidget* main, QWidget* render) {
    return render && render->isVisible() ? render : main;
}

bool MainShown(const QWidget* main) {
    return main && main->isVisible() && !main->isMinimized();
}

// A frameless see-through tool window that takes no input, as the panel.
void MakeOverlay(QWidget* w, int pixel_size) {
    w->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                      Qt::WindowDoesNotAcceptFocus);
    w->setAttribute(Qt::WA_TranslucentBackground);
    w->setAttribute(Qt::WA_ShowWithoutActivating);
    QFont f = w->font();
    f.setPixelSize(pixel_size);
    f.setWeight(QFont::DemiBold);
    w->setFont(f);
}

// A frameless tool window, as the emulator's own performance overlay: a child
// widget would be hidden under the native render surface.
class Panel : public QWidget {
public:
    Panel(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        QFont f = font();
        f.setPixelSize(13);
        f.setWeight(QFont::DemiBold);
        setFont(f);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
        timer->start(kRefreshMs);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 150));
        p.drawRoundedRect(rect(), 6, 6);
        const QFontMetrics fm(font());
        int y = kPad + fm.ascent();
        for (const auto& line : lines_) {
            p.setPen(line.dim ? QColor(170, 170, 170) : QColor(255, 255, 255));
            p.drawText(kPad, y, QString::fromStdString(line.text));
            y += fm.lineSpacing();
        }
    }

private:
    void Refresh() {
        lines_ = CoopHook::OverlayLines();
        QWidget* main = parentWidget();
        const bool game_shown = render_ && render_->isVisible();
        // The game look shows the players under the hearts (HudLayer); the
        // panel is then for debugging only (F7, LICHII_COOP_DEBUG_PANEL=1).
        if ((Game() && !DebugPanel()) || lines_.empty() || !main || !main->isVisible() ||
            main->isMinimized()) {
            hide();
            return;
        }
        const QFontMetrics fm(font());
        int width = 0;
        for (const auto& line : lines_) {
            width = std::max(width, fm.horizontalAdvance(QString::fromStdString(line.text)));
        }
        const int height = static_cast<int>(lines_.size()) * fm.lineSpacing();
        resize(width + 2 * kPad, height + 2 * kPad);
        QWidget* anchor = game_shown ? render_.data() : main;
        move(anchor->mapToGlobal(QPoint(kMargin, kMargin)));
        if (!isVisible()) {
            show();
        }
        update();
    }

    QPointer<QWidget> render_;
    std::vector<CoopHook::OverlayLine> lines_;
};

// The players over the game, when the hook knows the camera: a frameless,
// see-through tool window the size of the render area, like the panel. A
// player on screen is a name tag floating over the head (no dot); one just
// off screen is an arrow at the edge with the name beside it.
class MarkerLayer : public QWidget {
public:
    MarkerLayer(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
        timer->start(kMarkerRefreshMs);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        if (markers_.width <= 0 || markers_.height <= 0) {
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        // The game's picture keeps its aspect inside the render area.
        const double s = std::min(width() / double(markers_.width),
                                  height() / double(markers_.height));
        const double ox = (width() - markers_.width * s) / 2;
        const double oy = (height() - markers_.height * s) / 2;
        // The text grows with the picture: 3.5% of its height, 14 to 32 px.
        const double tag_px = std::clamp(markers_.height * s * 0.035, 14.0, 32.0);
        QFont f = font();
        f.setPixelSize(static_cast<int>(tag_px));
        f.setWeight(QFont::Bold);
        if (Game()) {
            f = GameFont(true, tag_px * 1.1);  // the game's font is a little small for its size
        }
        const QFontMetrics fm(f);
        const double outline = std::max(2.0, f.pixelSize() / 5.0);
        for (const auto& m : markers_.items) {
            const int a = std::clamp(static_cast<int>(m.alpha * 255.0f), 0, 255);
            if (a == 0) {
                continue;
            }
            QColor fill = m.self ? QColor(255, 210, 60, a) : QColor(90, 200, 255, a);
            if (Game() && !m.self) {
                fill = PlayerColour(names_.Of(m.tag), names_.self);
                fill.setAlpha(a);
            }
            const QColor dark(0, 0, 0, a * 3 / 4);
            const QPointF c(ox + m.x * s, oy + m.y * s);
            if (m.edge) {
                // A triangle at the edge pointing at the player.
                const double r = std::max(6.0, m.size * s / 2);
                const double ca = std::cos(m.arrow_angle), sa = std::sin(m.arrow_angle);
                const QPointF tip(c.x() + ca * r, c.y() + sa * r);
                const QPointF l(c.x() - ca * r * 0.6 - sa * r * 0.8,
                                c.y() - sa * r * 0.6 + ca * r * 0.8);
                const QPointF rr(c.x() - ca * r * 0.6 + sa * r * 0.8,
                                 c.y() - sa * r * 0.6 - ca * r * 0.8);
                const QPointF tri[] = {tip, l, rr};
                p.setPen(QPen(dark, 2));
                p.setBrush(fill);
                p.drawPolygon(tri, 3);
            } else if (!m.tag_head) {
                // No head height known: the old dot under the tag.
                const double r = std::max(3.0, m.size * s / 2);
                p.setPen(QPen(dark, 2));
                p.setBrush(fill);
                p.drawEllipse(c, r, r);
            }
            const QString tag = QString::fromStdString(m.tag);
            const double tw = fm.horizontalAdvance(tag);
            double tx = ox + m.tag_x * s - tw / 2;
            const double ty = m.tag_below ? oy + m.tag_y * s + fm.ascent()
                                          : oy + m.tag_y * s - fm.descent();
            tx = std::clamp(tx, 2.0, std::max(2.0, width() - tw - 2.0));
            if (Game()) {
                // The game's HUD style: the player's colour over a soft
                // shadow (docs/overlay.md, "The look").
                DrawGameText(p, QPointF(tx, ty), tag, f, fill);
                continue;
            }
            // White text (yellow for yourself) with a dark outline, so it
            // reads on sky and on grass alike.
            QPainterPath path;
            path.addText(QPointF(tx, ty), f, tag);
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(0, 0, 0, a * 4 / 5), outline, Qt::SolidLine, Qt::RoundCap,
                          Qt::RoundJoin));
            p.drawPath(path);
            p.setPen(Qt::NoPen);
            p.setBrush(m.self ? QColor(255, 225, 120, a) : QColor(255, 255, 255, a));
            p.drawPath(path);
        }
    }

private:
    static bool Same(const CoopHook::Markers& a, const CoopHook::Markers& b) {
        if (a.width != b.width || a.height != b.height || a.items.size() != b.items.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.items.size(); ++i) {
            const auto& p = a.items[i];
            const auto& q = b.items[i];
            if (p.x != q.x || p.y != q.y || p.size != q.size || p.alpha != q.alpha ||
                p.edge != q.edge || p.arrow_angle != q.arrow_angle || p.tag != q.tag ||
                p.tag_x != q.tag_x || p.tag_y != q.tag_y || p.tag_below != q.tag_below ||
                p.tag_head != q.tag_head) {
                return false;
            }
        }
        return true;
    }

    void Refresh() {
        auto markers = CoopHook::CurrentMarkers();
        QWidget* main = parentWidget();
        if (!markers || markers->items.empty() || !render_ || !render_->isVisible() || !main ||
            !main->isVisible() || main->isMinimized()) {
            markers_ = {};
            hide();
            return;
        }
        if (Game() && names_age_++ % 15 == 0) {  // who is who, four times a second
            names_.Refresh();
        }
        const QRect area(render_->mapToGlobal(QPoint(0, 0)), render_->size());
        if (geometry() != area) {
            setGeometry(area);
        }
        if (!isVisible()) {
            show();
        } else if (Same(*markers, markers_)) {
            return;
        }
        markers_ = std::move(*markers);
        update();
    }

    QPointer<QWidget> render_;
    CoopHook::Markers markers_;
    Names names_;
    unsigned names_age_ = 0;
};

// The other players under the game's own hearts, top left, as the game
// would draw them (docs/overlay.md, "The look"): per player their name in the
// game's font, a row of their hearts with the game's heart pictures, smaller
// than ours, and the game's name of the place they are in. No box. Game look
// only; the plain look has the panel.
class HudLayer : public QWidget {
public:
    HudLayer(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
        timer->start(kRefreshMs);
    }

    // Where the blocks start and their sizes, in the game's 720-line picture.
    // The game's hearts sit near the top left with the item icons under them.
    static constexpr double kLeft = 40.0;
    static constexpr double kTop = 205.0;
    static constexpr double kNamePx = 25.0;
    static constexpr double kPlacePx = 19.0;
    // The heart pictures are 96 x 96 with the heart about half as wide in
    // the middle: drawn kHeartPx big, kHeartStep apart, the heart shows
    // about 20 (the game's own are about 30).
    static constexpr double kHeartPx = 38.0;
    static constexpr double kHeartStep = 0.55;  // of kHeartPx, across and down
    static constexpr double kGap = 16.0;      // between two players

    struct Block {
        std::uint8_t number = 0;
        QString name;
        QString place;
        std::vector<int> hearts;  // quarters filled in each
        bool dim = false;         // no samples for a while
    };

protected:
    void paintEvent(QPaintEvent*) override {
        if (blocks_.empty()) {
            return;
        }
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        const QRectF pic = Picture(size());
        const double u = pic.height() / kGameHeight;
        const QFont name_font = GameFont(true, kNamePx * u);
        const QFont place_font = GameFont(false, kPlacePx * u);
        const QFontMetricsF nm(name_font), pm(place_font);
        const double heart = kHeartPx * u;
        const double x = pic.left() + kLeft * u;
        double y = pic.top() + kTop * u;
        for (const Block& b : blocks_) {
            const int alpha = b.dim ? 150 : 255;
            y += nm.ascent();
            DrawGameText(p, QPointF(x, y), b.name, name_font, QColor(255, 255, 255, alpha));
            y += nm.descent() + 2 * u;
            if (!b.hearts.empty()) {
                p.setOpacity(alpha / 255.0);
                for (std::size_t i = 0; i < b.hearts.size(); ++i) {
                    const int col = static_cast<int>(i) % CoopHudHearts::kPerRow;
                    const int row = static_cast<int>(i) / CoopHudHearts::kPerRow;
                    const QRectF r(x - heart * 0.22 + col * heart * kHeartStep,
                                   y - heart * 0.2 + row * heart * kHeartStep, heart, heart);
                    p.drawPixmap(r, Look().hearts[std::clamp(b.hearts[i], 0, 4)],
                                 QRectF(Look().hearts[0].rect()));
                }
                p.setOpacity(1.0);
                const int rows = (static_cast<int>(b.hearts.size()) + CoopHudHearts::kPerRow - 1) /
                                 CoopHudHearts::kPerRow;
                y += rows * heart * kHeartStep + 3 * u;
            }
            if (!b.place.isEmpty()) {
                y += pm.ascent();
                DrawGameText(p, QPointF(x, y), b.place, place_font, QColor(225, 225, 225, alpha));
                y += pm.descent();
            }
            y += kGap * u;
        }
    }

private:
    void Refresh() {
        QWidget* main = parentWidget();
        std::vector<Block> blocks;
        if (Game() && render_ && render_->isVisible() && MainShown(main)) {
            for (const auto& row : CoopHook::PlayerList()) {
                if (row.self) {
                    continue;
                }
                Block b;
                b.number = row.number;
                b.name = QString::fromStdString(row.name.empty() ? "Player " + std::to_string(row.number)
                                                                 : row.name);
                if (row.scene && !row.scene->empty()) {
                    b.place = QString::fromStdString(CoopHud::PlaceName(*row.scene));
                }
                if (const auto life = CoopHud::PlayerLife(row.number); life && life->max > 0) {
                    b.hearts = CoopHudHearts::Quarters(life->life, life->max);
                }
                b.dim = row.state != "connected";
                blocks.push_back(std::move(b));
            }
        }
        if (blocks.empty()) {
            blocks_.clear();
            hide();
            return;
        }
        const QRect area(render_->mapToGlobal(QPoint(0, 0)), render_->size());
        if (geometry() != area) {
            setGeometry(area);
        }
        if (!isVisible()) {
            show();
        }
        blocks_ = std::move(blocks);
        update();
    }

    QPointer<QWidget> render_;
    std::vector<Block> blocks_;
};

// The last chat lines and room notices, bottom left of the game. Each shows
// for kChatShowMs, then fades out; all kept lines show while the box is open.
class ChatLayer : public QWidget {
public:
    ChatLayer(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        MakeOverlay(this, 14);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
        timer->start(kChatRefreshMs);
    }

    // The chat box is open (`box_height` tall): every line, unfaded, above it.
    void SetTyping(int box_height) {
        box_height_ = box_height;
        Refresh();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QFontMetrics fm(font());
        if (Game()) {
            PaintGame(p);
            return;
        }
        int y = kPad + fm.ascent();
        for (const auto& l : shown_) {
            const int a = std::clamp(static_cast<int>(l.alpha * 255.0f), 0, 255);
            const QString text = fm.elidedText(QString::fromStdString(l.text), Qt::ElideRight,
                                               width() - 2 * kPad);
            const int tw = fm.horizontalAdvance(text);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, a * 150 / 255));
            p.drawRoundedRect(QRect(kPad - 4, y - fm.ascent() - 2, tw + 8, fm.height() + 4), 4, 4);
            p.setPen(l.notice ? QColor(255, 220, 120, a) : QColor(255, 255, 255, a));
            p.drawText(kPad, y, text);
            y += fm.lineSpacing() + 4;
        }
    }

private:
    struct Shown {
        std::string text;
        bool notice = false;
        float alpha = 1.0f;
    };

    // The game look: no box; "Name: text" with the name in the player's
    // colour and the text in white, room notices in the game's highlight
    // colour, all with the game's shadow.
    void PaintGame(QPainter& p) {
        const QFont f = font();
        const QFontMetricsF fm(f);
        const double line = fm.lineSpacing() * 1.1;
        double y = kPad + fm.ascent();
        const double room = width() - 2.0 * kPad;
        for (const auto& l : shown_) {
            const int a = std::clamp(static_cast<int>(l.alpha * 255.0f), 0, 255);
            const QString all = QString::fromStdString(l.text);
            QString name;
            QString rest = all;
            const qsizetype colon = all.indexOf(QStringLiteral(": "));
            if (!l.notice && colon > 0) {
                name = all.left(colon + 1);
                rest = all.mid(colon + 1);
            }
            double x = kPad;
            if (!name.isEmpty()) {
                QColor c = PlayerColour(names_.Of(all.left(colon).toStdString()), names_.self);
                c.setAlpha(a);
                DrawGameText(p, QPointF(x, y), name, f, c);
                x += fm.horizontalAdvance(name);
            }
            const QString text = fm.elidedText(rest, Qt::ElideRight, std::max(10.0, room - (x - kPad)));
            QColor c = l.notice ? kHighlight : QColor(255, 255, 255);
            c.setAlpha(a);
            DrawGameText(p, QPointF(x, y), text, f, c);
            y += line;
        }
    }

    void Refresh() {
        std::vector<Shown> shown;
        for (const auto& l : CoopHook::ChatLines()) {
            float alpha = 1.0f;
            if (box_height_ == 0 && l.age_ms >= kChatShowMs) {
                alpha = 1.0f - float(l.age_ms - kChatShowMs) / float(kChatFadeMs);
            }
            if (alpha > 0.0f) {
                shown.push_back({l.text, l.notice, alpha});
            }
        }
        QWidget* main = parentWidget();
        if (shown.empty() || !MainShown(main)) {
            shown_.clear();
            hide();
            return;
        }
        QWidget* anchor = Anchor(main, render_);
        int width = std::min(kChatWidth, anchor->width() - 2 * kMargin);
        int margin = kMargin;
        if (Game()) {
            // The game's font, sized with the picture; wider lines, and the
            // same left edge as the player blocks.
            const QRectF pic = Picture(anchor->size());
            const double u = pic.height() / kGameHeight;
            const QFont f = GameFont(false, std::clamp(23.0 * u, 14.0, 40.0));
            if (f != font()) {
                setFont(f);
            }
            margin = std::max(kMargin, static_cast<int>(pic.left() + (HudLayer::kLeft - kPad) * u));
            width = std::max(200, static_cast<int>(pic.width() * 0.5));
            if (names_age_++ % 5 == 0) {  // who is who, four times a second
                names_.Refresh();
            }
        }
        const QFontMetrics fm(font());
        const int line = Game() ? static_cast<int>(fm.lineSpacing() * 1.1) + 1 : fm.lineSpacing() + 4;
        const int height = static_cast<int>(shown.size()) * line + 2 * kPad;
        const int bottom = anchor->height() - kMargin - box_height_ - 4;
        const QRect area(anchor->mapToGlobal(QPoint(margin, std::max(0, bottom - height))),
                         QSize(std::min(width, anchor->width() - 2 * margin), height));
        if (geometry() != area) {
            setGeometry(area);
        }
        if (!isVisible()) {
            show();
        }
        shown_ = std::move(shown);
        update();
    }

    QPointer<QWidget> render_;
    int box_height_ = 0;  // 0: the box is closed
    std::vector<Shown> shown_;
    Names names_;
    unsigned names_age_ = 0;
};

// The one-line chat box: Enter sends, Esc (or clicking back on the game)
// closes. While it is open it has the keyboard, so the game gets no input
// (the render window releases every key when it loses focus).
class ChatBox : public QWidget {
public:
    ChatBox(QWidget* main, QWidget* render, ChatLayer* lines)
        : QWidget(main), render_(render), lines_(lines) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setAttribute(Qt::WA_TranslucentBackground);
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        edit_ = new QLineEdit(this);
        edit_->setMaxLength(kChatMaxChars);
        edit_->setPlaceholderText(QStringLiteral("Say something (Enter sends, Esc closes)"));
        QFont f = edit_->font();
        f.setPixelSize(14);
        edit_->setFont(f);
        SetFailed(false);
        layout->addWidget(edit_);
        connect(edit_, &QLineEdit::returnPressed, this, [this] { Send(); });
        connect(edit_, &QLineEdit::textEdited, this, [this] { SetFailed(false); });
        edit_->installEventFilter(this);
    }

    void Open() {
        QWidget* main = parentWidget();
        if (!MainShown(main)) {
            return;
        }
        QWidget* anchor = Anchor(main, render_);
        int w = std::min(kChatWidth, anchor->width() - 2 * kMargin);
        int margin = kMargin;
        if (Game()) {
            // As the chat lines (ChatLayer): the game's font, sized with the
            // picture, and their left edge.
            const QRectF pic = Picture(anchor->size());
            const double u = pic.height() / kGameHeight;
            edit_->setFont(GameFont(false, std::clamp(23.0 * u, 14.0, 40.0)));
            margin = std::max(kMargin, static_cast<int>(pic.left() + (HudLayer::kLeft - kPad) * u));
            w = std::max(200, static_cast<int>(pic.width() * 0.5));
            SetFailed(false);
        }
        const int h = edit_->sizeHint().height();
        w = std::min(w, anchor->width() - 2 * margin);
        setGeometry(QRect(anchor->mapToGlobal(QPoint(margin, anchor->height() - kMargin - h)),
                          QSize(w, h)));
        show();
        raise();
        activateWindow();
        edit_->setFocus(Qt::OtherFocusReason);
        if (lines_) {
            lines_->SetTyping(h);
        }
    }

    void Close() {
        if (!isVisible()) {
            return;
        }
        hide();
        edit_->clear();
        SetFailed(false);
        if (lines_) {
            lines_->SetTyping(0);
        }
        if (QWidget* main = parentWidget()) {
            main->activateWindow();
        }
        if (render_) {
            render_->setFocus(Qt::OtherFocusReason);
        }
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (obj == edit_) {
            // The emulator's "Exit Fullscreen" hotkey is Esc on the main
            // window, and would take it before the box sees it: claim it.
            if (event->type() == QEvent::ShortcutOverride &&
                static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                event->accept();
                return true;
            }
            if (event->type() == QEvent::KeyPress &&
                static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
                Close();
                return true;
            }
            if (event->type() == QEvent::FocusOut && isVisible()) {
                // Clicked back on the game or elsewhere; after the focus
                // change is done, so activating the main window is not undone.
                QTimer::singleShot(0, this, [this] {
                    if (!isActiveWindow()) {
                        Close();
                    }
                });
            }
        }
        return QWidget::eventFilter(obj, event);
    }

private:
    void Send() {
        const QString text = edit_->text().trimmed();
        if (text.isEmpty()) {
            Close();
            return;
        }
        if (!CoopHook::SendChat(text.toStdString())) {
            SetFailed(true);  // not connected, or text the protocol refuses
            return;
        }
        Close();
    }

    void SetFailed(bool failed) {
        if (Game()) {
            // No box: the typed text in the game's font with its shadow; red
            // text when it was not sent.
            if (edit_->graphicsEffect() == nullptr) {
                auto* shadow = new QGraphicsDropShadowEffect(edit_);
                shadow->setBlurRadius(6);
                shadow->setOffset(1.5, 2);
                shadow->setColor(QColor(0, 0, 0, 200));
                edit_->setGraphicsEffect(shadow);
                QPalette pal = edit_->palette();
                pal.setColor(QPalette::PlaceholderText, QColor(255, 255, 255, 160));
                edit_->setPalette(pal);
            }
            edit_->setStyleSheet(
                failed ? QStringLiteral("QLineEdit { background: transparent; border: none;"
                                        " color: #ff7a6a; padding: 2px 4px; }")
                       : QStringLiteral("QLineEdit { background: transparent; border: none;"
                                        " color: white; padding: 2px 4px; }"));
            edit_->setToolTip(failed ? QStringLiteral("Not sent: not connected, or characters chat "
                                                      "does not allow")
                                     : QString());
            return;
        }
        edit_->setStyleSheet(
            failed ? QStringLiteral("QLineEdit { background: rgba(90,0,0,210); color: white;"
                                    " border: 1px solid #f66; border-radius: 4px; padding: 4px; }")
                   : QStringLiteral("QLineEdit { background: rgba(0,0,0,190); color: white;"
                                    " border: 1px solid #888; border-radius: 4px; padding: 4px; }"));
        edit_->setToolTip(failed ? QStringLiteral("Not sent: not connected, or characters chat "
                                                  "does not allow")
                                 : QString());
    }

    QPointer<QWidget> render_;
    QPointer<ChatLayer> lines_;
    QLineEdit* edit_ = nullptr;
};

// Everyone in the room while Tab is held: number, name, state, ping (our own
// round trip; for the others "~" and how old their newest sample is), scene.
class PlayerList : public QWidget {
public:
    PlayerList(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        MakeOverlay(this, 15);
        auto* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this] {
            if (isVisible()) {
                Refresh();
            }
        });
        timer->start(kRefreshMs);
    }

    void SetHeld(bool held) {
        if (held) {
            Refresh();
        } else {
            hide();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        if (!Game()) {  // the game look has no box
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 170));
            p.drawRoundedRect(rect(), 8, 8);
        }
        const QFontMetrics fm(font());
        int y = 2 * kPad + fm.ascent();
        for (std::size_t r = 0; r < cells_.size(); ++r) {
            int x = 2 * kPad;
            const QColor colour = r == 0      ? (Game() ? kHighlight : QColor(255, 220, 120))
                                  : dim_[r] ? QColor(170, 170, 170)
                                            : QColor(255, 255, 255);
            p.setPen(colour);
            for (std::size_t c = 0; c < cells_[r].size(); ++c) {
                if (Game()) {
                    DrawGameText(p, QPointF(x, y), cells_[r][c], font(), colour);
                    x += widths_[c] + kColumnGap;
                    continue;
                }
                p.drawText(x, y, cells_[r][c]);
                x += widths_[c] + kColumnGap;
            }
            y += fm.lineSpacing();
        }
    }

private:
    static constexpr int kColumnGap = 18;

    void Refresh() {
        QWidget* main = parentWidget();
        if (!MainShown(main)) {
            hide();
            return;
        }
        if (Game()) {
            const QRectF pic = Picture(Anchor(main, render_)->size());
            const QFont f = GameFont(false, std::clamp(24.0 * pic.height() / kGameHeight, 14.0, 40.0));
            if (f != font()) {
                setFont(f);
            }
        }
        cells_.assign(1, {QStringLiteral("#"), QStringLiteral("Player"), QStringLiteral("State"),
                          QStringLiteral("Ping"),
                          Game() ? QStringLiteral("Place") : QStringLiteral("Scene")});
        dim_.assign(1, false);
        for (const auto& row : CoopHook::PlayerList()) {
            QString ms = QStringLiteral("-");
            if (row.ms) {
                ms = QStringLiteral("%1%2 ms").arg(row.self ? QString() : QStringLiteral("~")).arg(*row.ms);
            }
            QString name = QString::fromStdString(row.name);
            if (row.self) {
                name += QStringLiteral(" (you)");
            }
            cells_.push_back({row.number ? QStringLiteral("P%1").arg(row.number)
                                         : QStringLiteral("-"),
                              name, QString::fromStdString(row.state), ms,
                              QString::fromStdString(
                                  Game() && row.scene && !row.scene->empty()
                                      ? CoopHud::PlaceName(*row.scene)
                                      : row.scene.value_or("?"))});
            dim_.push_back(row.state != "connected");
        }
        const QFontMetrics fm(font());
        widths_.assign(cells_[0].size(), 0);
        for (const auto& r : cells_) {
            for (std::size_t c = 0; c < r.size(); ++c) {
                widths_[c] = std::max(widths_[c], fm.horizontalAdvance(r[c]));
            }
        }
        int width = 4 * kPad - kColumnGap;
        for (const int w : widths_) {
            width += w + kColumnGap;
        }
        const int height = static_cast<int>(cells_.size()) * fm.lineSpacing() + 4 * kPad;
        QWidget* anchor = Anchor(main, render_);
        const QRect area(anchor->mapToGlobal(QPoint(std::max(0, (anchor->width() - width) / 2),
                                                    anchor->height() / 6)),
                         QSize(width, height));
        if (geometry() != area) {
            setGeometry(area);
        }
        if (!isVisible()) {
            show();
        }
        update();
    }

    QPointer<QWidget> render_;
    std::vector<std::vector<QString>> cells_;
    std::vector<bool> dim_;
    std::vector<int> widths_;
};

// The keys, taken only while the game's render window has the keyboard:
// Enter opens the chat box, Tab held shows the player list, F7 shows or hides
// the debug panel in the game look. None is a
// button in the emulator's default keymap (they only reach the emulated USB
// keyboard, which Ocarina does not read), and both are eaten. With a
// modifier held they pass, so emulator hotkeys keep working.
class Keys : public QObject {
public:
    Keys(QWidget* main, QWidget* render, ChatBox* box, PlayerList* list)
        : QObject(main), main_(main), render_(render), box_(box), list_(list) {
        qApp->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        const auto type = event->type();
        if (type == QEvent::ApplicationDeactivate ||
            (type == QEvent::WindowDeactivate && obj == main_)) {
            if (list_ && list_->isVisible()) {
                list_->SetHeld(false);  // the Tab release may never come
            }
            return false;
        }
        if (type != QEvent::KeyPress && type != QEvent::KeyRelease) {
            return false;
        }
        auto* key = static_cast<QKeyEvent*>(event);
        if (!OnGame(obj) || (key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) {
            return false;
        }
        switch (key->key()) {
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (type == QEvent::KeyPress && !key->isAutoRepeat() && box_) {
                box_->Open();
            }
            return true;
        case Qt::Key_Tab:
            if (!key->isAutoRepeat() && list_) {
                list_->SetHeld(type == QEvent::KeyPress);
            }
            return true;
        case Qt::Key_F7:  // no emulator hotkey uses it (F4 to F11 do, F7 not)
            if (type == QEvent::KeyPress && !key->isAutoRepeat()) {
                DebugPanel() = !DebugPanel();
            }
            return true;
        default:
            return false;
        }
    }

private:
    // The event goes to the game's render window or the surface inside it.
    bool OnGame(QObject* obj) const {
        auto* w = qobject_cast<QWidget*>(obj);
        return w && render_ && render_->isVisible() &&
               (w == render_ || render_->isAncestorOf(w));
    }

    QPointer<QWidget> main_;
    QPointer<QWidget> render_;
    QPointer<ChatBox> box_;
    QPointer<PlayerList> list_;
};

} // namespace

void Attach(QWidget* main, QWidget* render) {
    CoopHook::Start();
    if (CoopHook::OverlayLines().empty()) {
        return;  // co-op is off
    }
    new Panel(main, render);
    // The other players under our hearts, in the game look (docs/overlay.md).
    new HudLayer(main, render);
    // Shows only while the hook has the camera; the panel stays either way.
    new MarkerLayer(main, render);
    // Chat lines, the chat box (Enter) and the player list (Tab held).
    auto* lines = new ChatLayer(main, render);
    auto* box = new ChatBox(main, render, lines);
    auto* list = new PlayerList(main, render);
    new Keys(main, render, box, list);
}

} // namespace CoopWindow
