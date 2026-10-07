#include "hostservices.h"
#include "interpreter.h"
#include <iostream>
#include <set>
#include <stdexcept>

using State = HostServices::State;
static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static HostServices::Completion await(HostServices& runtime) {
    auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < end) {
        auto completions = runtime.poll();
        if (!completions.empty()) {
            check(completions.size() == 1, "expected exactly one completion");
            return completions.front();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("completion did not arrive");
}
int main() {
    try {
        Interpreter vm(nullptr, nullptr);
        bool rejectedOwner = false;
        std::thread wrongOwner([&] {
            try { vm.collectGarbage(); } catch (const std::logic_error&) { rejectedOwner = true; }
        });
        wrongOwner.join();
        check(rejectedOwner, "foreign thread reached the Smalltalk heap");
        HostServices runtime;
        std::string binary("a\0\xff", 3);
        auto id = runtime.submit("echo", binary, 1000);
        auto echo = await(runtime);
        check(echo.id == id && echo.result.bytes == binary && echo.result.state == State::Succeeded, "binary roundtrip");
        check(runtime.poll().empty(), "completion delivered twice");

        std::atomic<std::uint64_t> clock{100};
        HostServices::Options settings;
        settings.capacity = 2;
        settings.workers = 1;
        settings.clock = [&] { return clock.load(); };
        HostServices controlled(settings);
        auto first = controlled.submit("delayEcho", "10000\na", 1000);
        auto second = controlled.submit("delayEcho", "10000\nb", 1000);
        bool rejected = false;
        try { controlled.submit("echo", "c", 1000); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "capacity must include queued and running jobs");
        check(controlled.cancel(first), "cancel accepted request");
        check(!controlled.cancel(first), "cancel must be idempotent");
        auto cancelled = await(controlled);
        check(cancelled.id == first && cancelled.result.state == State::Cancelled, "cancellation result");
        clock = 1101;
        auto timedOut = await(controlled);
        check(timedOut.id == second && timedOut.result.state == State::TimedOut, "injected-clock deadline");
        check(!controlled.cancel(first), "retired token must stay retired");
        auto interrupted = controlled.submit("delayEcho", "10000\nsnapshot", 1000);
        controlled.interruptAll();
        check(await(controlled).result.state == State::Interrupted, "snapshot interruption");
        auto next = controlled.submit("echo", "new", 1000);
        check(next != interrupted, "request IDs reused");
        check(await(controlled).result.bytes == "new", "runtime usable after snapshot");

        // Both native tasks must enter before either can finish: real parallelism.
        std::atomic<int> entered{0};
        std::atomic<bool> release{false};
        runtime.addService("parallel", [&](const std::string&, const HostServices::Task& task) {
            ++entered;
            while (!release && !task.stopped()) task.waitFor(1);
            HostServices::Result result; result.bytes = "parallel"; return result;
        });
        runtime.submit("parallel", "", 2000);
        runtime.submit("parallel", "", 2000);
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (entered != 2 && std::chrono::steady_clock::now() < end) std::this_thread::yield();
        check(entered == 2, "two worker tasks did not run concurrently");
        release = true;
        std::size_t finished = 0;
        while (finished < 2 && std::chrono::steady_clock::now() < end) {
            finished += runtime.poll().size(); std::this_thread::yield();
        }
        check(finished == 2, "parallel completion delivery");

        runtime.addService("failure", [](const std::string&, const HostServices::Task&) -> HostServices::Result {
            throw std::runtime_error("expected native failure");
        });
        runtime.submit("failure", "", 1000);
        check(await(runtime).result.error == "expected native failure", "native exception translation");
        runtime.addService("oversize", [](const std::string&, const HostServices::Task&) {
            HostServices::Result result; result.bytes.assign(16385, 'x'); return result;
        });
        runtime.submit("oversize", "", 1000);
        check(await(runtime).result.state == State::Failed, "oversized result rejection");
        runtime.addService("invalid", [](const std::string&, const HostServices::Task&) {
            HostServices::Result result; result.state = State::Pending; return result;
        });
        runtime.submit("invalid", "", 1000);
        check(await(runtime).result.state == State::Failed, "invalid terminal result rejection");
        rejected = false;
        try { runtime.submit("echo", std::string(16385, 'x'), 1000); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "oversized input rejection");

        // A producer can emit far more than the buffer capacity. It must stop
        // making progress until the consumer drains that bounded buffer.
        HostServices::Options streamOptions;
        streamOptions.maxBytes = 64;
        streamOptions.workers = 1;
        streamOptions.capacity = 2;
        HostServices streams(streamOptions);
        std::atomic<int> writes{0};
        streams.addService("stream", [&](const std::string&, const HostServices::Task& task) {
            std::string bytes(64, 'x');
            for (int i = 0; i < 100; ++i) { task.write(bytes.data(), bytes.size()); ++writes; }
            return HostServices::Result{};
        });
        auto streamId = streams.submit("stream", "", 2000, 6400);
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!writes && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        check(writes == 1 && streams.bufferedBytes(streamId) == 64, "producer exceeded bounded buffer");
        check(streams.pending() == 1, "unread stream lost its capacity reservation");
        std::string received;
        bool retired = false;
        while (!retired && std::chrono::steady_clock::now() < until) {
            check(streams.bufferedBytes(streamId) <= 64, "stream buffer grew beyond capacity");
            received += streams.read(streamId).bytes;
            for (const auto& event : streams.poll()) {
                if (event.retired) { check(event.result.state == State::Succeeded, "stream failed"); retired = true; }
            }
            std::this_thread::yield();
        }
        check(retired && received == std::string(6400, 'x'), "stream lost or duplicated bytes");
        check(streams.poll().empty(), "stream retired twice");

        writes = 0;
        auto cancelledStream = streams.submit("stream", "", 2000, 6400);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!writes && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        check(streams.cancel(cancelledStream), "cancel backpressured producer");
        auto cancelEvent = await(streams);
        check(cancelEvent.retired && cancelEvent.result.state == State::Cancelled, "stream cancellation did not retire");
        // A later task proves cancellation released the blocked worker.
        streams.submit("echo", "worker released", 1000);
        check(await(streams).result.bytes == "worker released", "producer remained blocked after cancellation");

        streams.addService("short", [](const std::string&, const HostServices::Task& task) {
            task.write("abcd", 4); return HostServices::Result{};
        });
        auto shortId = streams.submit("short", "", 1000, 4);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        bool succeededUnread = false;
        while (!succeededUnread && std::chrono::steady_clock::now() < until)
            for (const auto& event : streams.poll())
                if (event.result.state == State::Succeeded) { succeededUnread = true; check(!event.retired, "unread bytes retired"); }
        check(succeededUnread && streams.bufferedBytes(shortId) == 4, "unread completed stream not retained");
        streams.interruptAll();
        check(await(streams).result.state == State::Interrupted, "snapshot retained unread native bytes");
        streams.submit("short", "", 1000, 3);
        auto limited = await(streams);
        check(limited.retired && limited.result.state == State::Failed && limited.result.error == "download exceeds maximumBytes", "download limit not enforced");
        streams.addService("badStatus", [](const std::string&, const HostServices::Task& task) {
            task.status(100000); return HostServices::Result{};
        });
        streams.submit("badStatus", "", 1000, 1);
        check(await(streams).result.state == State::Failed, "invalid streaming status accepted");

        std::atomic<std::uint64_t> streamClock{0};
        streamOptions.clock = [&] { return streamClock.load(); };
        HostServices expiring(streamOptions);
        expiring.addService("blocked", [](const std::string&, const HostServices::Task& task) {
            std::string data(128, 'x'); task.write(data.data(), data.size()); return HostServices::Result{};
        });
        auto expiringId = expiring.submit("blocked", "", 10, 128);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!expiring.bufferedBytes(expiringId) && std::chrono::steady_clock::now() < until) std::this_thread::yield();
        streamClock = 11;
        auto expired = await(expiring);
        check(expired.retired && expired.result.state == State::TimedOut, "deadline did not unblock writer");
        expiring.addService("short", [](const std::string&, const HostServices::Task& task) {
            task.write("x", 1); return HostServices::Result{};
        });
        expiring.submit("short", "", 10, 1);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        bool completedUnread = false;
        while (!completedUnread && std::chrono::steady_clock::now() < until)
            for (const auto& event : expiring.poll())
                if (event.result.state == State::Succeeded) completedUnread = true;
        check(completedUnread, "completed stream did not publish status");
        streamClock = 22;
        check(await(expiring).result.state == State::TimedOut, "abandoned completed stream did not expire");
        streams.shutdown();
        expiring.shutdown();

        // Race completion with cancellation; each accepted ID is delivered once.
        std::set<std::uint32_t> delivered;
        for (int batch = 0; batch < 10; ++batch) {
            for (int i = 0; i < 32; ++i) {
                auto request = runtime.submit("echo", "race", 1000);
                if (i % 2) runtime.cancel(request);
            }
            std::size_t count = 0;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (count < 32 && std::chrono::steady_clock::now() < deadline) {
                for (const auto& completion : runtime.poll()) {
                    check(delivered.insert(completion.id).second, "duplicate raced completion"); ++count;
                }
                std::this_thread::yield();
            }
            check(count == 32, "lost raced completion");
        }
        runtime.submit("delayEcho", "100000\nshutdown", 120000);
        runtime.shutdown();
        check(await(runtime).result.state == State::Closed, "shutdown resolves accepted jobs");
        rejected = false;
        try { runtime.submit("echo", "", 1); } catch (const std::runtime_error&) { rejected = true; }
        check(rejected, "submission after shutdown");
        std::cout << "host service tests passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
