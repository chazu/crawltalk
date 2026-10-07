#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// No Smalltalk object references cross this interface. Byte strings may contain NUL.
class HostServices {
public:
    enum class State { Pending, Succeeded, Failed, Cancelled, TimedOut, Interrupted, Closed };
    struct Result { State state = State::Succeeded; std::string bytes, error; int status = 0; };
    struct Completion { std::uint32_t id; Result result; bool retired = true; };
    struct Read { std::string bytes; bool end = false; };
    using Clock = std::function<std::uint64_t()>;
    struct Options {
        std::size_t workers = 2, capacity = 64, maxBytes = 16384;
        Clock clock;
    };
    struct Control {
        std::atomic<bool> stopped{false};
        std::mutex mutex;
        std::condition_variable changed;
    };
    class Task {
    public:
        bool stopped() const;
        bool waitFor(std::uint32_t milliseconds) const;
        std::uint64_t remainingMilliseconds() const;
        void write(const char* bytes, std::size_t length) const;
        void status(int code) const;
        bool streaming() const { return static_cast<bool>(writer); }
        const std::size_t maxBytes;
    private:
        friend class HostServices;
        Task(std::shared_ptr<Control> c, Clock clock, std::uint64_t deadline, std::size_t limit)
            : maxBytes(limit), control(std::move(c)), clock(std::move(clock)), deadline(deadline) {}
        std::shared_ptr<Control> control;
        Clock clock;
        std::uint64_t deadline;
        std::function<void(const char*, std::size_t)> writer;
        std::function<void(int)> publishStatus;
    };
    using Service = std::function<Result(const std::string&, const Task&)>;
    HostServices();
    explicit HostServices(Options options);
    ~HostServices();
    HostServices(const HostServices&) = delete;
    HostServices& operator=(const HostServices&) = delete;
    void addService(const std::string& name, Service service);
    std::uint32_t submit(const std::string& service, std::string bytes, std::uint32_t timeoutMs,
                         std::uint32_t maximumDownload = 0);
    Read read(std::uint32_t id);
    std::size_t bufferedBytes(std::uint32_t id) const;
    bool cancel(std::uint32_t id, State reason = State::Cancelled);
    void interruptAll();
    std::vector<Completion> poll();
    void shutdown();
    const std::string& epoch() const { return session; }
    std::size_t pending() const;
    static const char* stateName(State state);
private:
    struct Job {
        std::uint32_t id;
        std::string bytes;
        std::uint64_t deadline;
        Service service;
        std::shared_ptr<Control> control = std::make_shared<Control>();
        bool terminal = false;
        bool streaming = false, dirty = false;
        std::string buffer;
        std::uint64_t produced = 0, maximumDownload = 0;
        Result result;
    };
    void work();
    void finish(const std::shared_ptr<Job>& job, Result result);
    void finishLocked(const std::shared_ptr<Job>& job, Result result);
    void stopLocked(const std::shared_ptr<Job>& job, State state);
    void write(const std::shared_ptr<Job>& job, const char* bytes, std::size_t length);
    Options options;
    std::string session;
    mutable std::mutex mutex;
    std::condition_variable ready;
    std::condition_variable spaceAvailable;
    std::map<std::string, Service> services;
    std::map<std::uint32_t, std::shared_ptr<Job>> jobs;
    std::deque<std::shared_ptr<Job>> queue;
    std::vector<std::thread> workers;
    std::uint64_t nextId = 1;
    bool closing = false;
};
