// Background decode pool.
//
// Jobs carry a priority and a generation. When the user moves to another image
// the generation is bumped and everything queued for the old one is dropped
// without being decoded, so navigation never queues up behind stale work.
#include "pg.h"

void Loader::start(HWND notify) {
    notify_ = notify;
    unsigned n = std::thread::hardware_concurrency();
    if (n == 0) n = 4;
    n = clampi((int)n - 1, 2, 8);
    for (unsigned i = 0; i < n; ++i)
        threads_.emplace_back([this] { worker(); });
}

void Loader::stop() {
    quit_.store(true);
    cv_.notify_all();
    for (auto& t : threads_) if (t.joinable()) t.join();
    threads_.clear();
}

void Loader::bumpGeneration() {
    gen_.fetch_add(1, std::memory_order_acq_rel);
    std::lock_guard<std::mutex> lk(qm_);
    // Drop everything that is not for the new generation and not a thumbnail.
    uint64_t g = gen_.load(std::memory_order_acquire);
    std::deque<DecodeJob> keep;
    for (auto& j : queue_)
        if (j.kind == JobKind::Thumb || j.generation == g) keep.push_back(std::move(j));
    int dropped = (int)queue_.size() - (int)keep.size();
    queue_.swap(keep);
    if (dropped > 0) pending_.fetch_sub(dropped, std::memory_order_relaxed);
}

void Loader::submit(DecodeJob job) {
    {
        std::lock_guard<std::mutex> lk(qm_);
        job.seq = seq_.fetch_add(1, std::memory_order_relaxed);
        // Keep the queue ordered by (priority, seq) so the visible image always
        // wins over preloads and offscreen thumbnails.
        auto it = std::upper_bound(queue_.begin(), queue_.end(), job,
            [](const DecodeJob& a, const DecodeJob& b) {
                if (a.priority != b.priority) return a.priority < b.priority;
                return a.seq < b.seq;
            });
        queue_.insert(it, std::move(job));
        pending_.fetch_add(1, std::memory_order_relaxed);
    }
    cv_.notify_one();
}

bool Loader::pop(DecodeResult& out) {
    std::lock_guard<std::mutex> lk(dm_);
    if (done_.empty()) return false;
    out = std::move(done_.front());
    done_.erase(done_.begin());
    return true;
}

void Loader::worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    for (;;) {
        DecodeJob job;
        {
            std::unique_lock<std::mutex> lk(qm_);
            cv_.wait(lk, [this] { return quit_.load() || !queue_.empty(); });
            if (quit_.load()) break;
            job = std::move(queue_.front());
            queue_.pop_front();
        }

        bool stale = (job.kind != JobKind::Thumb) && job.generation != gen_.load(std::memory_order_acquire);
        if (!stale) {
            DecodeResult res;
            runDecodeJob(job, res, gen_);
            {
                std::lock_guard<std::mutex> lk(dm_);
                done_.push_back(std::move(res));
            }
            if (notify_) PostMessageW(notify_, WM_PG_DECODED, 0, 0);
        }
        pending_.fetch_sub(1, std::memory_order_relaxed);
    }
    CoUninitialize();
}
