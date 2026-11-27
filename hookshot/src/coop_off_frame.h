#pragma once

// Row 292: slow reads that co-op does now and then (this PC's networks, the process's memory) are done here, on a
// thread of their own, never in the frame hook or the host's service loop. A player's host stalled the game's main
// thread ~850 ms every 30 s (Longshot's "sead::MainThread held it 849.6 ms" lines) while the LAN announcement read
// the PC's network adapters (GetAdaptersAddresses) in the frame.
// Off the frame is not enough for every read: a heap walk (HeapSummary) here still froze all of the game's threads
// 80-100 ms each time (fps-spike-0121), so the memory line reads counters only and the heap split is opt-in.
//
// A Slot runs one job at a time. Start never waits for a job: while one still runs it says false and the caller
// keeps what it had (so jobs never pile up behind a slow read). The Slot's end waits for a job still running.

#include <atomic>
#include <functional>
#include <thread>
#include <utility>

namespace CoopOffFrame {

class Slot {
public:
    Slot() = default;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    ~Slot() { Join(); }

    // Starts `job` on its own thread; false (and `job` not run) while the last one still runs or no thread starts.
    bool Start(std::function<void()> job) {
        if (busy_.load(std::memory_order_acquire)) return false;
        Join();   // (the last one is done: its thread ends at once)
        busy_.store(true, std::memory_order_release);
        try {
            thread_ = std::thread([this, job = std::move(job)] {
                try {
                    job();
                } catch (...) {
                }
                busy_.store(false, std::memory_order_release);
            });
        } catch (...) {
            busy_.store(false, std::memory_order_release);
            return false;
        }
        return true;
    }
    bool Busy() const { return busy_.load(std::memory_order_acquire); }

private:
    void Join() {
        if (thread_.joinable()) thread_.join();
    }
    std::atomic<bool> busy_{false};
    std::thread thread_;
};

}  // namespace CoopOffFrame
