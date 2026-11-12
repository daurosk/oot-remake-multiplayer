#include "coop_window.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "coop_hook.h"
#include "coop_settings.h"
#include "common/fs/path_util.h"
#include <fstream>
#include <sstream>

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
        if (lines_.empty() || !main || !main->isVisible() || main->isMinimized()) {
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
        QFont f = font();
        f.setPixelSize(std::clamp(static_cast<int>(markers_.height * s * 0.035), 14, 32));
        f.setWeight(QFont::Bold);
        const QFontMetrics fm(f);
        const double outline = std::max(2.0, f.pixelSize() / 5.0);
        for (const auto& m : markers_.items) {
            const int a = std::clamp(static_cast<int>(m.alpha * 255.0f), 0, 255);
            if (a == 0) {
                continue;
            }
            const QColor fill = m.self ? QColor(255, 210, 60, a) : QColor(90, 200, 255, a);
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
        const QFontMetrics fm(font());
        const int height = static_cast<int>(shown.size()) * (fm.lineSpacing() + 4) + 2 * kPad;
        QWidget* anchor = Anchor(main, render_);
        const int bottom = anchor->height() - kMargin - box_height_ - 4;
        const QRect area(anchor->mapToGlobal(QPoint(kMargin, std::max(0, bottom - height))),
                         QSize(std::min(kChatWidth, anchor->width() - 2 * kMargin), height));
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
        const int h = edit_->sizeHint().height();
        const int w = std::min(kChatWidth, anchor->width() - 2 * kMargin);
        setGeometry(QRect(anchor->mapToGlobal(QPoint(kMargin, anchor->height() - kMargin - h)),
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
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(rect(), 8, 8);
        const QFontMetrics fm(font());
        int y = 2 * kPad + fm.ascent();
        for (std::size_t r = 0; r < cells_.size(); ++r) {
            int x = 2 * kPad;
            const QColor colour = r == 0      ? QColor(255, 220, 120)
                                  : dim_[r] ? QColor(170, 170, 170)
                                            : QColor(255, 255, 255);
            p.setPen(colour);
            for (std::size_t c = 0; c < cells_[r].size(); ++c) {
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
        cells_.assign(1, {QStringLiteral("#"), QStringLiteral("Player"), QStringLiteral("State"),
                          QStringLiteral("Ping"), QStringLiteral("Scene")});
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
                              QString::fromStdString(row.scene.value_or("?"))});
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
// Enter opens the chat box, Tab held shows the player list. Neither is a
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


// The Co-op menu: name and tunic, written to coop.ini (the tunic also applies
// at once; a new name is used from the next start of the emulator).
void NameAndTunic(QWidget* main) {
    const auto ini = Common::FS::GetEdenPath(Common::FS::EdenPath::ConfigDir) / "coop.ini";
    std::string text;
    {
        std::ifstream in(ini, std::ios::binary);
        std::ostringstream all;
        all << in.rdbuf();
        text = all.str();
    }
    std::vector<std::string> errors;
    const CoopHook::Settings now = CoopHook::LoadSettings(ini, errors);
    const std::string profile = CoopHook::CurrentProfileName();

    QDialog dialog(main);
    dialog.setWindowTitle(QStringLiteral("Co-op: nombre y t\u00fanica"));
    auto* form = new QFormLayout(&dialog);
    auto* use_profile = new QCheckBox(
        QStringLiteral("Usar el nombre del perfil del emulador (%1)")
            .arg(profile.empty() ? QStringLiteral("sin perfil") : QString::fromStdString(profile)));
    auto* name = new QLineEdit(QString::fromStdString(now.name_set ? now.name : profile));
    name->setMaxLength(16);
    use_profile->setChecked(!now.name_set && !profile.empty());
    name->setEnabled(!use_profile->isChecked());
    QObject::connect(use_profile, &QCheckBox::toggled, name, [name, profile](bool on) {
        name->setEnabled(!on);
        if (on) name->setText(QString::fromStdString(profile));
    });
    auto* tunic = new QComboBox;
    tunic->addItem(QStringLiteral("La del juego"), QString());
    tunic->addItem(QStringLiteral("Verde (Kokiri)"), QStringLiteral("green"));
    tunic->addItem(QStringLiteral("Roja (Goron)"), QStringLiteral("red"));
    tunic->addItem(QStringLiteral("Azul (Zora)"), QStringLiteral("blue"));
    for (int i = 0; i < tunic->count(); ++i) {
        const std::string v = tunic->itemData(i).toString().toStdString();
        if (!now.tunic.empty() && CoopHook::ValidTunic(now.tunic) && v == now.tunic) tunic->setCurrentIndex(i);
    }
    form->addRow(use_profile);
    form->addRow(QStringLiteral("Nombre (1 a 16 letras):"), name);
    form->addRow(QStringLiteral("T\u00fanica que ven los dem\u00e1s (de adulto):"), tunic);
    form->addRow(new QLabel(QStringLiteral("El nombre nuevo se usa al volver a abrir el emulador; la t\u00fanica, al momento.")));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    std::optional<std::string> new_name;
    if (!use_profile->isChecked()) {
        const std::string typed = CoopHook::NameFromProfile(name->text().toStdString());
        if (typed.empty()) {
            QMessageBox::warning(main, QStringLiteral("Co-op"), QStringLiteral("El nombre debe tener de 1 a 16 letras."));
            return;
        }
        new_name = typed;
    }
    const std::string tunic_word = tunic->currentData().toString().toStdString();
    text = CoopHook::SetIniValue(text, "name", new_name);
    text = CoopHook::SetIniValue(text, "tunic",
                                 tunic_word.empty() ? std::nullopt : std::optional<std::string>(tunic_word));
    {
        std::ofstream out(ini, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            QMessageBox::warning(main, QStringLiteral("Co-op"), QStringLiteral("No se pudo guardar coop.ini."));
            return;
        }
    }
    CoopHook::SetTunic(tunic_word);
}

void AddMenu(QWidget* main) {
    auto* window = qobject_cast<QMainWindow*>(main);
    if (window == nullptr || window->menuBar() == nullptr) {
        return;
    }
    QMenu* menu = window->menuBar()->addMenu(QStringLiteral("Co-op"));
    QAction* act = menu->addAction(QStringLiteral("Nombre y t\u00fanica\u2026"));
    QObject::connect(act, &QAction::triggered, main, [main] { NameAndTunic(main); });
}

} // namespace

void Attach(QWidget* main, QWidget* render) {
    AddMenu(main);  // also with co-op off: the name can be set before a server is set

    CoopHook::Start();
    if (CoopHook::OverlayLines().empty()) {
        return;  // co-op is off
    }
    new Panel(main, render);
    // Shows only while the hook has the camera; the panel stays either way.
    new MarkerLayer(main, render);
    // Chat lines, the chat box (Enter) and the player list (Tab held).
    auto* lines = new ChatLayer(main, render);
    auto* box = new ChatBox(main, render, lines);
    auto* list = new PlayerList(main, render);
    new Keys(main, render, box, list);
}

} // namespace CoopWindow
