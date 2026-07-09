#pragma once
#include <algorithm>
#include <cstdio>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <cstring>
#elif defined(__APPLE__)
#include <pthread.h>
#include <cstring>
#endif

namespace dzIPC::ThreadDispatch
{
    struct ThreadOptions
    {
        bool enable{false};
        int priority{20};
        int cpu_id{-1};
        bool realtime{true};
    };

    inline ThreadOptions make_realtime_options(bool enable, int cpu_id = -1, int priority = 20)
    {
        ThreadOptions opt;
        opt.enable = enable;
        opt.cpu_id = enable ? cpu_id : -1;
        opt.priority = priority;
        opt.realtime = true;
        return opt;
    }

    inline void warn(bool verbose, const std::string& name, const char* action, const char* reason)
    {
        if (!verbose)
        {
            return;
        }
        std::printf("\033[33m[Warning] Failed to %s for thread '%s': %s\033[0m\n", action, name.c_str(), reason);
    }

    inline int clamp_linux_fifo_priority(int priority)
    {
#if defined(__linux__)
        const int min_prio = sched_get_priority_min(SCHED_FIFO);
        const int max_prio = sched_get_priority_max(SCHED_FIFO);
        if (min_prio < 0 || max_prio < 0)
        {
            return priority;
        }
        return std::max(min_prio, std::min(priority, max_prio));
#else
        return priority;
#endif
    }

    inline bool set_thread_priority(std::thread* th, int priority, bool verbose, const std::string& name,
                                    bool realtime = true)
    {
        if (!th || !th->joinable())
        {
            return false;
        }

#if defined(_WIN32)
        if (realtime)
        {
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        }
        int win_prio = THREAD_PRIORITY_NORMAL;
        if (priority >= 30)
        {
            win_prio = THREAD_PRIORITY_HIGHEST;
        }
        else if (priority >= 10)
        {
            win_prio = THREAD_PRIORITY_ABOVE_NORMAL;
        }
        BOOL ok = SetThreadPriority(th->native_handle(), win_prio);
        if (!ok)
        {
            warn(verbose, name, "set thread priority", "SetThreadPriority failed");
        }
        return ok != 0;

#elif defined(__linux__)
        sched_param sp{};
        sp.sched_priority = realtime ? clamp_linux_fifo_priority(priority) : 0;
        const int policy = realtime ? SCHED_FIFO : SCHED_OTHER;
        int rc = pthread_setschedparam(th->native_handle(), policy, &sp);
        if (rc != 0)
        {
            warn(verbose, name, "set thread priority", std::strerror(rc));
        }
        return rc == 0;

#elif defined(__APPLE__)
        (void)priority;
        (void)realtime;
        warn(verbose, name, "set thread priority", "unsupported on macOS in this wrapper");
        return false;

#else
        (void)priority;
        (void)verbose;
        (void)name;
        (void)realtime;
        return false;
#endif
    }

    inline bool set_current_thread_priority(int priority, bool verbose, const std::string& name, bool realtime = true)
    {
#if defined(_WIN32)
        if (realtime)
        {
            SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
        }
        int win_prio = THREAD_PRIORITY_NORMAL;
        if (priority >= 30)
        {
            win_prio = THREAD_PRIORITY_HIGHEST;
        }
        else if (priority >= 10)
        {
            win_prio = THREAD_PRIORITY_ABOVE_NORMAL;
        }
        BOOL ok = SetThreadPriority(GetCurrentThread(), win_prio);
        if (!ok)
        {
            warn(verbose, name, "set current thread priority", "SetThreadPriority failed");
        }
        return ok != 0;

#elif defined(__linux__)
        sched_param sp{};
        sp.sched_priority = realtime ? clamp_linux_fifo_priority(priority) : 0;
        const int policy = realtime ? SCHED_FIFO : SCHED_OTHER;
        int rc = pthread_setschedparam(pthread_self(), policy, &sp);
        if (rc != 0)
        {
            warn(verbose, name, "set current thread priority", std::strerror(rc));
        }
        return rc == 0;

#elif defined(__APPLE__)
        (void)priority;
        (void)realtime;
        warn(verbose, name, "set current thread priority", "unsupported on macOS in this wrapper");
        return false;

#else
        (void)priority;
        (void)verbose;
        (void)name;
        (void)realtime;
        return false;
#endif
    }

    inline bool set_thread_affinity(std::thread* th, int cpu_id, bool verbose, const std::string& name)
    {
        if (!th || !th->joinable() || cpu_id < 0)
        {
            return false;
        }

#if defined(_WIN32)
        constexpr int kBits = static_cast<int>(sizeof(DWORD_PTR) * 8);
        if (cpu_id >= kBits)
        {
            warn(verbose, name, "set thread affinity", "cpu_id is outside DWORD_PTR mask range");
            return false;
        }
        DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu_id;
        DWORD_PTR prev = SetThreadAffinityMask(th->native_handle(), mask);
        if (prev == 0)
        {
            warn(verbose, name, "set thread affinity", "SetThreadAffinityMask failed");
        }
        return prev != 0;

#elif defined(__linux__)
        if (cpu_id >= CPU_SETSIZE)
        {
            warn(verbose, name, "set thread affinity", "cpu_id is outside CPU_SETSIZE");
            return false;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        int rc = pthread_setaffinity_np(th->native_handle(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            warn(verbose, name, "set thread affinity", std::strerror(rc));
        }
        return rc == 0;

#elif defined(__APPLE__)
        warn(verbose, name, "set thread affinity", "unsupported on macOS");
        return false;

#else
        (void)th;
        (void)cpu_id;
        (void)verbose;
        (void)name;
        return false;
#endif
    }

    inline bool set_current_thread_affinity(int cpu_id, bool verbose, const std::string& name)
    {
        if (cpu_id < 0)
        {
            return false;
        }

#if defined(_WIN32)
        constexpr int kBits = static_cast<int>(sizeof(DWORD_PTR) * 8);
        if (cpu_id >= kBits)
        {
            warn(verbose, name, "set current thread affinity", "cpu_id is outside DWORD_PTR mask range");
            return false;
        }
        DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpu_id;
        DWORD_PTR prev = SetThreadAffinityMask(GetCurrentThread(), mask);
        if (prev == 0)
        {
            warn(verbose, name, "set current thread affinity", "SetThreadAffinityMask failed");
        }
        return prev != 0;

#elif defined(__linux__)
        if (cpu_id >= CPU_SETSIZE)
        {
            warn(verbose, name, "set current thread affinity", "cpu_id is outside CPU_SETSIZE");
            return false;
        }
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_id, &cpuset);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
        if (rc != 0)
        {
            warn(verbose, name, "set current thread affinity", std::strerror(rc));
        }
        return rc == 0;

#elif defined(__APPLE__)
        warn(verbose, name, "set current thread affinity", "unsupported on macOS");
        return false;

#else
        (void)cpu_id;
        (void)verbose;
        (void)name;
        return false;
#endif
    }

    inline bool apply_thread_options(std::thread* th, const ThreadOptions& opt, bool verbose, const std::string& name)
    {
        if (!opt.enable)
        {
            return true;
        }
        bool ok = true;
        if (opt.cpu_id >= 0)
        {
            ok = set_thread_affinity(th, opt.cpu_id, verbose, name) && ok;
        }
        if (opt.priority > 0)
        {
            ok = set_thread_priority(th, opt.priority, verbose, name, opt.realtime) && ok;
        }
        return ok;
    }

    inline bool apply_current_thread_options(const ThreadOptions& opt, bool verbose, const std::string& name)
    {
        if (!opt.enable)
        {
            return true;
        }
        bool ok = true;
        if (opt.cpu_id >= 0)
        {
            ok = set_current_thread_affinity(opt.cpu_id, verbose, name) && ok;
        }
        if (opt.priority > 0)
        {
            ok = set_current_thread_priority(opt.priority, verbose, name, opt.realtime) && ok;
        }
        return ok;
    }
}
