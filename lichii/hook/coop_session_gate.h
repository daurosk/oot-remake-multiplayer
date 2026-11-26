#pragma once
// A throttled part's step in the session's frame (the boss glue's 100 ms tick, the time's quarter second): the
// session gone is seen on the first frame without it, before any throttle, so the part forgets the session at once.
// The ended session gets one last frame after a menu leave (coop_hook.cpp OnFrame): with the throttle first, that
// frame fell inside the tick most times and the part kept the session (its sync, and the next session's join step
// was skipped). Header only, so the unit tests hold it.

namespace CoopSessionGate {

enum class Step {
    forget,  // the session is gone (or not connected): forget it, once
    idle,    // no session, already forgotten
    wait,    // in a session, but the throttle says not yet
    join,    // in a session, the first run since it began: as `run`, after the part's join step
    run,     // in a session, due
};

class Gate {
public:
    // `in_session`: a connected session this frame. `due`: the throttle, asked only in a session (it may take
    // its tick when asked, as CoopEvery::Due does).
    template <class Due>
    Step Next(bool in_session, Due&& due) {
        if (!in_session) {
            if (connected_) {
                connected_ = false;
                return Step::forget;
            }
            return Step::idle;
        }
        if (!due()) {
            return Step::wait;
        }
        if (!connected_) {
            connected_ = true;
            return Step::join;
        }
        return Step::run;
    }
    bool Connected() const { return connected_; }

private:
    bool connected_ = false;
};

}  // namespace CoopSessionGate
