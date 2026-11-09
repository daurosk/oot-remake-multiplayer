#include "coop_window.h"

#include <algorithm>
#include <vector>

#include <QFontMetrics>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include "coop_hook.h"

namespace CoopWindow {
namespace {

constexpr int kPad = 8;
constexpr int kMargin = 16;
constexpr int kRefreshMs = 200;

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

} // namespace

void Attach(QWidget* main, QWidget* render) {
    CoopHook::Start();
    if (CoopHook::OverlayLines().empty()) {
        return;  // co-op is off
    }
    new Panel(main, render);
}

} // namespace CoopWindow
