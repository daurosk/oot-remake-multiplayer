// The item cheat window (coop_cheats.h), only with LICHII_COOP_CHEATS=1. It
// starts itself when the Qt application is made, so no patch calls it.

#include <QApplication>
#include <QComboBox>
#include <QCompleter>
#include <QCoreApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include "coop_cheats.h"

namespace {

void AddQuick(QVBoxLayout* layout, const char* label, const char* flag) {
    auto* button = new QPushButton(QString::fromUtf8(label));
    QObject::connect(button, &QPushButton::clicked, [flag] { CoopCheats::Give(flag); });
    layout->addWidget(button);
}

void Open() {
    auto* window = new QWidget(nullptr, Qt::Window);
    window->setWindowTitle(QStringLiteral("Co-op item cheats (debug)"));
    window->setAttribute(Qt::WA_QuitOnClose, false);
    window->setAttribute(Qt::WA_DeleteOnClose, true);
    auto* layout = new QVBoxLayout(window);

    layout->addWidget(new QLabel(QStringLiteral("Gives the local player an item, as a pickup "
                                                "would. The save must be loaded.")));
    AddQuick(layout, "Gold Skulltula token (+1)", "PouchData.Item_Token_Gold.StockNum");
    AddQuick(layout, "Heart piece (+1)", "PouchData.Obj_Heart_Piece.StockNum");
    AddQuick(layout, "Small key, Forest Temple (+1)", "DungeonInfo.ForestTemple.SmallKeyNum");
    AddQuick(layout, "Hookshot", "PouchData.Item_Hookshot.StockNum");
    AddQuick(layout, "Song of Storms", "IsGet.Item_Melody_StormSong");

    auto* row = new QHBoxLayout();
    auto* items = new QComboBox();
    items->setEditable(true);
    items->setInsertPolicy(QComboBox::NoInsert);
    QStringList names;
    for (const auto& name : CoopCheats::Items()) {
        names << QString::fromStdString(name);
    }
    items->addItems(names);
    items->completer()->setFilterMode(Qt::MatchContains);
    items->completer()->setCompletionMode(QCompleter::PopupCompletion);
    items->setMinimumContentsLength(40);
    auto* give = new QPushButton(QStringLiteral("Give"));
    QObject::connect(give, &QPushButton::clicked,
                     [items] { CoopCheats::Give(items->currentText().toStdString()); });
    row->addWidget(items, 1);
    row->addWidget(give);
    layout->addLayout(row);

    auto* result = new QLabel();
    result->setWordWrap(true);
    layout->addWidget(result);
    auto* timer = new QTimer(window);
    QObject::connect(timer, &QTimer::timeout, [result] {
        result->setText(QString::fromStdString(CoopCheats::LastResult()));
    });
    timer->start(300);

    QObject::connect(qApp, &QCoreApplication::aboutToQuit, window, &QWidget::close);
    window->show();
}

void Start() {
    if (CoopCheats::On()) {
        // After the main window, once the event loop runs.
        QTimer::singleShot(2000, [] { Open(); });
    }
}

} // namespace

Q_COREAPP_STARTUP_FUNCTION(Start)
