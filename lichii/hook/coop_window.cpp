#include "coop_window.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QFontMetrics>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "coop_hook.h"

namespace CoopWindow {
namespace {

constexpr int kPad = 8;
constexpr int kMargin = 16;
constexpr int kRefreshMs = 200;
constexpr int kMarkerRefreshMs = 16;  // markers follow the camera

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

// The markers over the game, when the hook knows the camera: a frameless,
// see-through tool window the size of the render area, like the panel.
class MarkerLayer : public QWidget {
public:
    MarkerLayer(QWidget* main, QWidget* render) : QWidget(main), render_(render) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowTransparentForInput |
                       Qt::WindowDoesNotAcceptFocus);
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        QFont f = font();
        f.setPixelSize(14);
        f.setWeight(QFont::DemiBold);
        setFont(f);
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
        const QFontMetrics fm(font());
        for (const auto& m : markers_.items) {
            const int a = std::clamp(static_cast<int>(m.alpha * 255.0f), 0, 255);
            const QColor fill = m.self ? QColor(255, 210, 60, a) : QColor(90, 200, 255, a);
            const QColor dark(0, 0, 0, a * 3 / 4);
            const QPointF c(ox + m.x * s, oy + m.y * s);
            const double r = std::max(3.0, m.size * s / 2);
            p.setPen(QPen(dark, 2));
            p.setBrush(fill);
            if (m.edge) {
                // A triangle at the edge pointing at the player.
                const double ca = std::cos(m.arrow_angle), sa = std::sin(m.arrow_angle);
                const QPointF tip(c.x() + ca * r, c.y() + sa * r);
                const QPointF l(c.x() - ca * r * 0.6 - sa * r * 0.8,
                                c.y() - sa * r * 0.6 + ca * r * 0.8);
                const QPointF rr(c.x() - ca * r * 0.6 + sa * r * 0.8,
                                 c.y() - sa * r * 0.6 - ca * r * 0.8);
                const QPointF tri[] = {tip, l, rr};
                p.drawPolygon(tri, 3);
            } else {
                p.drawEllipse(c, r, r);
            }
            const QString tag = QString::fromStdString(m.tag);
            const double tw = fm.horizontalAdvance(tag);
            const double tx = ox + m.tag_x * s - tw / 2;
            const double ty = m.tag_below ? oy + m.tag_y * s + fm.ascent()
                                          : oy + m.tag_y * s - fm.descent();
            p.setPen(dark);
            p.drawText(QPointF(tx + 1, ty + 1), tag);
            p.setPen(QColor(255, 255, 255, a));
            p.drawText(QPointF(tx, ty), tag);
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
                p.tag_x != q.tag_x || p.tag_y != q.tag_y || p.tag_below != q.tag_below) {
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

} // namespace

void Attach(QWidget* main, QWidget* render) {
    CoopHook::Start();
    if (CoopHook::OverlayLines().empty()) {
        return;  // co-op is off
    }
    new Panel(main, render);
    // Shows only while the hook has the camera; the panel stays either way.
    new MarkerLayer(main, render);
}

} // namespace CoopWindow
