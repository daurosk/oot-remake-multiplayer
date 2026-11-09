// The co-op entry points the patched emulator calls. Built into the
// emulator's core library only when LICHII_COOP is on.
#pragma once

namespace Core {
class System;
}

namespace CoopHook {

// Runs once per emulated frame, after the guest's present, on the thread
// that handles the present.
void OnFrame(Core::System& system);

}  // namespace CoopHook
