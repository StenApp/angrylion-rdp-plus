#include "parallel.h"

#include <functional>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

class Parallel
{
public:
    Parallel(uint32_t num_workers) {
        // create worker threads
        for (uint32_t worker_id = 0; worker_id < num_workers; worker_id++) {
            m_workers.emplace_back(std::thread(&Parallel::do_work, this, worker_id));
        }
    }

    ~Parallel() {
        // wait for all workers to finish their current work
        wait();

        // set a dummy task in case nothing has been done yet
        if (!m_task) {
            m_task = [](uint32_t) {};
        }

        // exit worker main loops
        m_accept_work = false;
        m_signal_work.notify_all();

        // join worker threads to make sure they have finished
        for (auto& thread : m_workers) {
            thread.join();
        }

        // destroy all worker threads
        m_workers.clear();
    }

    void run(std::function<void(uint32_t)>&& task) {
        // don't allow more tasks if workers are stopping
        if (!m_accept_work) {
            throw std::runtime_error("Workers are exiting and no longer accept work");
        }

        // prepare task for workers and send signal so they start working
        m_task = task;
        m_workers_active = m_workers.size();
        m_signal_work.notify_all();

        // wait for all workers to finish
        wait();
    }

private:
    std::function<void(uint32_t)> m_task;
    std::vector<std::thread> m_workers;
    std::mutex m_mutex;
    std::condition_variable m_signal_work;
    std::condition_variable m_signal_done;
    std::atomic_size_t m_workers_active;
    std::atomic_bool m_accept_work{true};

    void do_work(int32_t worker_id) {
        std::unique_lock<std::mutex> ul(m_mutex);
        while (m_accept_work) {
            m_signal_work.wait(ul);
            ul.unlock();
            m_task(worker_id);
            ul.lock();
            m_workers_active--;
            m_signal_done.notify_all();
        }
    }

    void wait() {
        std::unique_lock<std::mutex> ul(m_mutex);
        while (m_workers_active) {
            m_signal_done.wait(ul);
        }
    }

    void operator=(const Parallel&) = delete;
    Parallel(const Parallel&) = delete;
};

static std::unique_ptr<Parallel> parallel;
static uint32_t worker_num;

void parallel_init(uint32_t num)
{
    // auto-select number of workers based on the number of cores
    if (num == 0) {
        num = std::thread::hardware_concurrency();
    }

    parallel = std::make_unique<Parallel>(num);

    worker_num = num;
}

void parallel_run(void task(uint32_t))
{
    parallel->run(task);
}

uint32_t parallel_worker_num()
{
    return worker_num;
}

void parallel_close()
{
    parallel.reset();
}
