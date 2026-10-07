#include "hostservices.h"
#include <algorithm>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#ifdef CRAWLTALK_CURL
#include <curl/curl.h>
#endif

namespace {
std::uint64_t monotonicMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
HostServices::Result terminal(HostServices::State state) {
    HostServices::Result result;
    result.state = state;
    result.error = HostServices::stateName(state);
    return result;
}
#ifdef CRAWLTALK_CURL
struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            throw std::runtime_error("curl initialization failed");
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};
struct CurlTransfer {
    const HostServices::Task& task;
    CURL* curl;
    std::string error;
};
std::size_t receive(char* ptr, std::size_t size, std::size_t count, void* user) {
    auto& transfer = *static_cast<CurlTransfer*>(user);
    if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
    std::size_t length = size * count;
    try {
        long status = 0;
        curl_easy_getinfo(transfer.curl, CURLINFO_RESPONSE_CODE, &status);
        transfer.task.status(static_cast<int>(status));
        transfer.task.write(ptr, length);
    } catch (const std::exception& e) { transfer.error = e.what(); return 0; }
    catch (...) { return 0; }
    return length;
}
int progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return static_cast<CurlTransfer*>(user)->task.stopped() ? 1 : 0;
}
HostServices::Result httpGet(const std::string& url, const HostServices::Task& task) {
    if (!task.streaming()) throw std::runtime_error("HTTP requires a streaming request");
    if (!(curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_ASYNCHDNS))
        throw std::runtime_error("httpGet requires libcurl with asynchronous DNS");
    if (url.find('\0') != std::string::npos ||
        (url.compare(0, 7, "http://") && url.compare(0, 8, "https://")))
        throw std::runtime_error("httpGet requires an HTTP or HTTPS URL");
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) throw std::runtime_error("curl handle allocation failed");
    CurlTransfer transfer{task, curl.get(), {}};
    char error[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, static_cast<long>(std::max<std::uint64_t>(1, task.remainingMilliseconds())));
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT_MS, 5000L);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &transfer);
    curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error);
    // TLS verification stays enabled. Redirects are returned to Smalltalk.
    CURLcode code = curl_easy_perform(curl.get());
    HostServices::Result result;
    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    result.status = static_cast<int>(status);
    if (code != CURLE_OK) {
        result.state = code == CURLE_OPERATION_TIMEDOUT ? HostServices::State::TimedOut : HostServices::State::Failed;
        result.error = !transfer.error.empty() ? transfer.error : (error[0] ? error : curl_easy_strerror(code));
    }
    return result;
}
#endif
}

const char* HostServices::stateName(State state) {
    switch (state) {
        case State::Pending: return "pending";
        case State::Succeeded: return "succeeded";
        case State::Failed: return "failed";
        case State::Cancelled: return "cancelled";
        case State::TimedOut: return "timed out";
        case State::Interrupted: return "interrupted by snapshot";
        case State::Closed: return "closed";
    }
    return "invalid state";
}
bool HostServices::Task::stopped() const { return control->stopped || clock() >= deadline; }
void HostServices::Task::write(const char* bytes, std::size_t length) const {
    if (!writer) throw std::logic_error("request is not streaming");
    writer(bytes, length);
}
void HostServices::Task::status(int code) const {
    if (code < 0 || code > 999) throw std::runtime_error("invalid native service status");
    if (publishStatus) publishStatus(code);
}
std::uint64_t HostServices::Task::remainingMilliseconds() const {
    auto now = clock(); return now < deadline ? deadline - now : 0;
}
bool HostServices::Task::waitFor(std::uint32_t milliseconds) const {
    auto until = clock() + milliseconds;
    std::unique_lock<std::mutex> lock(control->mutex);
    while (!stopped() && clock() < until)
        control->changed.wait_for(lock, std::chrono::milliseconds(2));
    return !stopped();
}
HostServices::HostServices() : HostServices(Options{}) {}
HostServices::HostServices(Options settings) : options(std::move(settings)) {
    if (!options.workers || !options.capacity || !options.maxBytes || options.maxBytes > 16384)
        throw std::invalid_argument("invalid host service limits");
    if (!options.clock) options.clock = monotonicMillis;
    std::random_device random;
    std::ostringstream token;
    for (int i = 0; i < 4; ++i) token << std::hex << std::setw(8) << std::setfill('0') << random();
    session = token.str();
    addService("echo", [](const std::string& bytes, const Task&) {
        Result result; result.bytes = bytes; return result;
    });
    addService("delayEcho", [](const std::string& bytes, const Task& task) {
        auto split = bytes.find('\n');
        if (split == std::string::npos) throw std::runtime_error("delayEcho expects milliseconds, LF, payload");
        std::size_t parsed = 0;
        auto delay = std::stoul(bytes.substr(0, split), &parsed);
        if (parsed != split || delay > 120000) throw std::runtime_error("invalid delay");
        task.waitFor(static_cast<std::uint32_t>(delay));
        Result result; result.bytes = bytes.substr(split + 1); return result;
    });
#ifdef CRAWLTALK_CURL
    static CurlGlobal global;
    addService("httpGet", httpGet);
#endif
    try {
        for (std::size_t i = 0; i < options.workers; ++i) workers.emplace_back([this] { work(); });
    } catch (...) { shutdown(); throw; }
}
HostServices::~HostServices() { shutdown(); }
void HostServices::addService(const std::string& name, Service service) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closing || !service || services.count(name)) throw std::invalid_argument("invalid or duplicate service");
    services.emplace(name, std::move(service));
}
std::uint32_t HostServices::submit(const std::string& service, std::string bytes, std::uint32_t timeoutMs,
                                 std::uint32_t maximumDownload) {
    std::lock_guard<std::mutex> lock(mutex);
    if (closing) throw std::runtime_error("host services shut down");
    if (jobs.size() >= options.capacity) throw std::runtime_error("host request capacity exhausted");
    if (bytes.size() > options.maxBytes) throw std::runtime_error("request exceeds byte limit");
    if (!timeoutMs || timeoutMs > 120000) throw std::runtime_error("timeout must be 1..120000 milliseconds");
    auto serviceAt = services.find(service);
    if (serviceAt == services.end()) throw std::runtime_error("unknown host service: " + service);
    if (nextId > std::numeric_limits<std::uint32_t>::max()) throw std::runtime_error("host request IDs exhausted");
    auto job = std::make_shared<Job>();
    job->id = static_cast<std::uint32_t>(nextId++);
    job->bytes = std::move(bytes);
    job->deadline = options.clock() + timeoutMs;
    job->service = serviceAt->second;
    job->streaming = maximumDownload != 0;
    job->maximumDownload = maximumDownload;
    jobs.emplace(job->id, job);
    queue.push_back(job);
    ready.notify_one();
    return job->id;
}
void HostServices::write(const std::shared_ptr<Job>& job, const char* bytes, std::size_t length) {
    std::unique_lock<std::mutex> lock(mutex);
    if (length > job->maximumDownload - job->produced)
        throw std::runtime_error("download exceeds maximumBytes");
    while (length) {
        while (!job->terminal && job->buffer.size() == options.maxBytes && options.clock() < job->deadline)
            spaceAvailable.wait_for(lock, std::chrono::milliseconds(2));
        if (job->terminal || options.clock() >= job->deadline) throw std::runtime_error("stream stopped");
        auto count = std::min(length, options.maxBytes - job->buffer.size());
        // Edge-triggered readiness; filling an already readable buffer sends no
        // extra notifications. The VM acknowledges readiness on every read.
        if (job->buffer.empty()) job->dirty = true;
        job->buffer.append(bytes, count);
        job->produced += count;
        bytes += count; length -= count;
    }
}
HostServices::Read HostServices::read(std::uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    auto at = jobs.find(id);
    if (at == jobs.end()) throw std::runtime_error("retired stream");
    auto& job = at->second;
    if (!job->streaming) throw std::runtime_error("request is not streaming");
    if (options.clock() >= job->deadline && (!job->terminal || !job->buffer.empty())) stopLocked(job, State::TimedOut);
    Read result;
    result.bytes.swap(job->buffer);
    result.end = job->terminal && result.bytes.empty();
    if (job->terminal) job->dirty = true; // retire after the final buffered chunk
    spaceAvailable.notify_all();
    return result;
}
std::size_t HostServices::bufferedBytes(std::uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex);
    auto at = jobs.find(id);
    return at == jobs.end() ? 0 : at->second->buffer.size();
}
void HostServices::finishLocked(const std::shared_ptr<Job>& job, Result result) {
    if (job->terminal) return;
    if (result.state == State::Pending || static_cast<int>(result.state) < 0 ||
        static_cast<int>(result.state) > static_cast<int>(State::Closed) || result.status < 0 || result.status > 999) {
        result = terminal(State::Failed); result.error = "invalid native service result";
    }
    if (result.bytes.size() > options.maxBytes) {
        result = terminal(State::Failed); result.error = "response exceeds byte limit";
    }
    if (job->streaming && !result.bytes.empty()) {
        result = terminal(State::Failed); result.error = "streaming services must use Task::write";
    }
    if (job->streaming && result.state != State::Succeeded) job->buffer.clear();
    if (job->streaming && !result.status) result.status = job->result.status;
    if (result.error.size() > 1024) result.error.resize(1024);
    job->terminal = true;
    job->dirty = true;
    job->result = std::move(result);
    job->control->stopped = true;
    job->control->changed.notify_all();
    spaceAvailable.notify_all();
}
void HostServices::stopLocked(const std::shared_ptr<Job>& job, State state) {
    // Completed transfers with unread native bytes still own a stream resource.
    if (job->streaming) { job->buffer.clear(); job->terminal = false; }
    finishLocked(job, terminal(state));
}
void HostServices::finish(const std::shared_ptr<Job>& job, Result result) {
    std::lock_guard<std::mutex> lock(mutex);
    if (options.clock() >= job->deadline) result = terminal(State::TimedOut);
    finishLocked(job, std::move(result));
}
bool HostServices::cancel(std::uint32_t id, State reason) {
    if (reason != State::Cancelled && reason != State::Closed) throw std::invalid_argument("invalid cancellation state");
    std::lock_guard<std::mutex> lock(mutex);
    auto at = jobs.find(id);
    if (at == jobs.end()) return false;
    if (at->second->terminal && (!at->second->streaming ||
        (at->second->result.state != State::Succeeded && reason != State::Closed))) return false;
    stopLocked(at->second, reason);
    queue.erase(std::remove(queue.begin(), queue.end(), at->second), queue.end());
    return true;
}
void HostServices::interruptAll() {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : jobs) {
        if (!entry.second->terminal || (entry.second->streaming && !entry.second->buffer.empty()))
            stopLocked(entry.second, State::Interrupted);
    }
    queue.clear();
}
std::vector<HostServices::Completion> HostServices::poll() {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<Completion> completed;
    auto now = options.clock();
    for (auto at = jobs.begin(); at != jobs.end();) {
        auto& job = at->second;
        if (now >= job->deadline && (!job->terminal || (job->streaming && !job->buffer.empty())))
            stopLocked(job, State::TimedOut);
        bool retired = job->terminal && (!job->streaming || job->buffer.empty());
        if (job->dirty || retired) {
            Result result = job->result;
            if (!job->terminal) result.state = State::Pending;
            completed.push_back({job->id, std::move(result), retired});
            job->dirty = false;
        }
        if (retired) {
            queue.erase(std::remove(queue.begin(), queue.end(), job), queue.end());
            at = jobs.erase(at);
        } else ++at;
    }
    return completed;
}
std::size_t HostServices::pending() const {
    std::lock_guard<std::mutex> lock(mutex);
    return jobs.size();
}
void HostServices::work() {
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock<std::mutex> lock(mutex);
            ready.wait(lock, [&] { return closing || !queue.empty(); });
            if (closing) return;
            job = queue.front(); queue.pop_front();
        }
        Task task(job->control, options.clock, job->deadline, options.maxBytes);
        if (job->streaming) {
            task.writer = [this, job](const char* bytes, std::size_t length) { write(job, bytes, length); };
            task.publishStatus = [this, job](int status) {
                std::lock_guard<std::mutex> lock(mutex);
                if (!job->terminal && job->result.status != status) { job->result.status = status; job->dirty = true; }
            };
        }
        Result result;
        try {
            if (task.stopped()) result = terminal(State::TimedOut);
            else result = job->service(job->bytes, task);
        } catch (const std::exception& e) { result.state = State::Failed; result.error = e.what(); }
        catch (...) { result.state = State::Failed; result.error = "native service failed"; }
        finish(job, std::move(result));
    }
}
void HostServices::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex);
        closing = true;
        for (const auto& entry : jobs) stopLocked(entry.second, State::Closed);
        queue.clear();
        ready.notify_all();
    }
    for (auto& worker : workers) if (worker.joinable()) worker.join();
}
