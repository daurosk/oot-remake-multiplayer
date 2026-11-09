// The co-op window: a small see-through panel over the game in the top-left
// corner of the render area, listing the other players (name, scene,
// distance). Built into the Qt frontend only when LICHII_COOP is on.
#pragma once

class QWidget;

namespace CoopWindow {

// Starts co-op (CoopHook::Start) and, when it is on, shows the panel over
// `render`, owned by `main` (deleted with it). Call once, on the GUI thread.
void Attach(QWidget* main, QWidget* render);

} // namespace CoopWindow
