/*
 * Oscairn effects (doc/oscairn-plan.md). GPL-3.0-or-later, see OscairnEffects.cpp.
 *
 * Work that must not run on the audio thread (plan, decision 2): an engine's worker thread and a
 * lock-free exchange between it and the audio thread.
 *
 *  - The audio thread hands a job its latest request through a TripleBuffer and calls
 *    Worker::wake: an atomic increment and notify, no lock, no allocation, no waiting.
 *  - The worker thread services every registered job after each wake. A job takes only the
 *    newest request (older ones it never saw are skipped) and publishes its result through a
 *    second TripleBuffer, which the audio thread reads at a block boundary.
 *  - Jobs are added and removed on the main thread, which may wait for the worker to finish the
 *    job it is servicing; the audio thread never touches the job list.
 */
#ifndef OSCAIRN_CLAP_OFFTHREAD_H
#define OSCAIRN_CLAP_OFFTHREAD_H

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace oscairn_fx
{
// The newest value from one writer thread to one reader thread, wait-free on both sides.
template <typename T> class TripleBuffer
{
  public:
    T &writeSlot() { return slot[back]; }
    // Makes the write slot the newest value; a value the reader never took is dropped.
    void publish() { back = mid.exchange((uint8_t)(back | freshBit), std::memory_order_acq_rel) & 3; }
    bool fresh() const { return mid.load(std::memory_order_acquire) & freshBit; }
    // The newest value, or nullptr when nothing was published since the last read.
    const T *read()
    {
        if (!fresh())
            return nullptr;
        front = mid.exchange(front, std::memory_order_acq_rel) & 3;
        return &slot[front];
    }

  private:
    static constexpr uint8_t freshBit = 4;
    T slot[3]{};
    std::atomic<uint8_t> mid{1};
    uint8_t back{0}, front{2};
};

struct Job
{
    // On the worker thread: do any pending work; true if there was some.
    virtual bool service() noexcept = 0;

  protected:
    ~Job() = default;
};

class Worker
{
  public:
    Worker() = default;
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;
    ~Worker()
    {
        if (!thread.joinable())
            return;
        stopping.store(true, std::memory_order_release);
        wake();
        thread.join();
    }
    // Main thread. Starts the thread with the first job; may throw std::system_error.
    void add(Job *j)
    {
        std::lock_guard<std::mutex> g(m);
        jobs.push_back(j);
        if (!thread.joinable())
            thread = std::thread([this] { loop(); });
    }
    // Main thread. Returns once the worker is not servicing j and never will again.
    void remove(Job *j)
    {
        std::lock_guard<std::mutex> g(m);
        jobs.erase(std::remove(jobs.begin(), jobs.end(), j), jobs.end());
    }
    // Any thread, including the audio thread: no lock and no allocation. notify_one is a
    // futex/ulock wake when the worker waits, and returns at once otherwise.
    void wake() noexcept
    {
        signal.fetch_add(1, std::memory_order_release);
        signal.notify_one();
    }

  private:
    void loop()
    {
        for (;;)
        {
            const uint32_t seen = signal.load(std::memory_order_acquire);
            if (stopping.load(std::memory_order_acquire))
                return;
            {
                std::lock_guard<std::mutex> g(m);
                for (auto *j : jobs)
                    j->service();
            }
            signal.wait(seen, std::memory_order_acquire);
        }
    }
    std::mutex m;
    std::vector<Job *> jobs;
    std::atomic<uint32_t> signal{0};
    std::atomic<bool> stopping{false};
    std::thread thread;
};
} // namespace oscairn_fx

#endif
