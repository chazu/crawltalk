#include <cstring>
#include "interpreter.h"
#include "posixfilesystem.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <deque>
#include <algorithm>
#include <random>

// Deterministic clock/input host. All image and scheduler access stays here.
class HeadlessHost : public IHardwareAbstractionLayer {
public:
    Interpreter* vm = nullptr;
    std::uint32_t ticks = 0, deadline = 0;
    int semaphore = 0, width = 0, height = 0, inputSemaphore = 0;
    bool quit = false;
    std::deque<std::uint16_t> input;
    std::deque<std::pair<std::uint32_t, std::uint16_t>> events;
    std::size_t injected = 0;
    std::string image = "snapshot.im";
    bool headless() const override { return true; }
    void set_input_semaphore(int s) override { inputSemaphore = s; }
    std::uint32_t get_smalltalk_epoch_time() override { return 3968870400u + ticks / 1000; }
    std::uint32_t get_msclock() override { return ticks; }
    void signal_at(int s, std::uint32_t time) override { semaphore = s; deadline = time; }
    void set_cursor_image(std::uint16_t*) override {}
    void set_cursor_location(int, int) override {}
    void get_cursor_location(int* x, int* y) override { *x = *y = 0; }
    void set_link_cursor(bool) override {}
    bool set_display_size(int w, int h) override { width = w; height = h; return true; }
    void display_changed(int, int, int, int) override {}
    bool next_input_word(std::uint16_t* word) override {
        if (input.empty()) return false;
        *word = input.front(); input.pop_front(); return true;
    }
    void error(const char* text) override { throw std::runtime_error(text); }
    void signal_quit() override { quit = true; }
    void exit_to_debugger() override { throw std::runtime_error("image requested debugger"); }
    const char* get_image_name() override { return image.c_str(); }
    void set_image_name(const char* text) override { image = text; }
    void advance(std::uint32_t step) {
        ticks += step;
        while (inputSemaphore && !events.empty() && events.front().first <= ticks) {
            input.push_back(events.front().second);
            events.pop_front();
            ++injected;
            vm->asynchronousSignal(inputSemaphore);
        }
        if (semaphore && static_cast<std::int32_t>(ticks - deadline) >= 0) {
            vm->asynchronousSignal(semaphore);
            semaphore = 0;
        }
        vm->checkLowMemoryConditions();
        vm->pollHostServices();
    }
};

static std::string quoted(const std::string& input) {
    std::string result = "'";
    for (char c : input) { result += c == '\n' ? '\r' : c; if (c == '\'') result += c; }
    return result + "'";
}
static std::uint64_t number(const std::string& text, std::uint64_t maximum = UINT64_MAX) {
    if (text.empty() || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; }))
        throw std::runtime_error("expected an unsigned decimal number");
    auto value = std::stoull(text);
    if (value > maximum) throw std::runtime_error("numeric option out of range");
    return value;
}
class FileInSource {
public:
    FileInSource(IFileSystem& fs, const std::string& path) : files(fs) {
        std::ifstream source(path, std::ios::binary);
        if (!source) throw std::runtime_error("cannot open file-in: " + path);
        std::random_device random;
        int fd = -1;
        for (int attempt = 0; attempt < 4 && fd == -1; ++attempt) {
            name = "crawltalk-filein-" + std::to_string(random()) + ".st";
            fd = files.create_file_exclusive(name.c_str());
        }
        if (fd == -1) throw std::runtime_error("cannot stage file-in");
        try {
            char buffer[4096];
            bool previousCR = false;
            while (source) {
                source.read(buffer, sizeof(buffer));
                std::string normalized;
                for (std::streamsize i = 0; i < source.gcount(); ++i) {
                    char c = buffer[i];
                    if (c != '\n' || !previousCR) normalized += c == '\n' ? '\r' : c;
                    previousCR = c == '\r';
                }
                std::size_t offset = 0;
                while (offset < normalized.size()) {
                    int written = files.write(fd, normalized.data() + offset, static_cast<int>(normalized.size() - offset));
                    if (written <= 0) throw std::runtime_error("cannot write staged file-in");
                    offset += static_cast<std::size_t>(written);
                }
            }
            if (source.bad()) throw std::runtime_error("cannot read file-in");
            int closed = files.close_file(fd); fd = -1;
            if (closed == -1) throw std::runtime_error("cannot close staged file-in");
        } catch (...) {
            if (fd != -1) files.close_file(fd);
            files.delete_file(name.c_str());
            throw;
        }
    }
    ~FileInSource() { files.delete_file(name.c_str()); }
    std::string expression() const {
        return "(FileStream oldFileNamed: " + quoted(name) + ") fileIn. true";
    }
private:
    IFileSystem& files;
    std::string name;
};
int main(int argc, char** argv) {
    try {
        HeadlessHost host;
        std::string directory, expect, display;
        bool hasExpect = false, gc = false, gcAtStart = false;
        struct Action { bool fileIn; std::string text; };
        std::vector<Action> actions;
        std::uint64_t limit = 50000000, boot = 2000000;
        std::uint32_t tickStep = 1, cyclesPerTick = 1800, wallMs = 30000;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i == argc) throw std::runtime_error("missing value for " + arg);
                return argv[i];
            };
            if (arg == "--directory") directory = value();
            else if (arg == "--image") host.image = value();
            else if (arg == "--eval") actions.push_back({false, value()});
            else if (arg == "--file-in") actions.push_back({true, value()});
            else if (arg == "--expect") { expect = value(); hasExpect = true; }
            else if (arg == "--cycles") limit = number(value());
            else if (arg == "--boot-cycles") boot = number(value());
            else if (arg == "--tick-step") tickStep = static_cast<std::uint32_t>(number(value(), UINT32_MAX));
            else if (arg == "--cycles-per-tick") cyclesPerTick = static_cast<std::uint32_t>(number(value(), 1000000));
            else if (arg == "--wall-ms") wallMs = static_cast<std::uint32_t>(number(value(), UINT32_MAX));
            else if (arg == "--events") {
                std::ifstream file(value());
                if (!file) throw std::runtime_error("cannot open event script");
                std::string line;
                while (std::getline(file, line)) {
                    if (line.empty() || line[0] == '#') continue;
                    std::istringstream row(line);
                    std::uint64_t time, word;
                    std::string extra;
                    if (!(row >> time >> word) || (row >> extra) || time > UINT32_MAX || word > 65535 ||
                        (!host.events.empty() && time < host.events.back().first))
                        throw std::runtime_error("events require sorted: millisecond unsigned-word");
                    host.events.emplace_back(static_cast<std::uint32_t>(time), static_cast<std::uint16_t>(word));
                }
            }
            else if (arg == "--display") display = value();
            else if (arg == "--gc") gc = true;
            else if (arg == "--gc-at-start") gcAtStart = true;
            else throw std::runtime_error("unknown option: " + arg);
        }
        if (directory.empty()) throw std::runtime_error("usage: crawltalk-headless --directory DIR [--file-in FILE] [--eval SOURCE] [--expect VALUE] [--gc] [--display FILE.pbm]");
        if (!limit || !cyclesPerTick || !tickStep || !wallMs) throw std::runtime_error("execution limits and clock steps must be positive");
        PosixST80FileSystem files(directory);
        auto vm = std::make_unique<Interpreter>(&host, &files);
        host.vm = vm.get();
        if (!vm->init()) throw std::runtime_error("cannot load image");
        if (gcAtStart) vm->collectGarbage();
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(wallMs);
        std::uint64_t cycles = 0;
        auto cycle = [&]() {
            if (cycles++ >= limit)
                throw std::runtime_error("image execution limit exceeded");
            if (cycles % cyclesPerTick == 0) {
                if (std::chrono::steady_clock::now() > end) throw std::runtime_error("image wall-time limit exceeded");
                host.advance(tickStep);
            }
            vm->cycle();
        };
        for (std::uint64_t i = 0; i < boot && !host.quit; ++i) cycle();
        std::string result;
        for (const auto& action : actions) {
            if (host.quit) throw std::runtime_error("image quit before evaluation");
            std::unique_ptr<FileInSource> staged;
            if (action.fileIn) staged.reset(new FileInSource(files, action.text));
            std::string source = staged ? staged->expression() : action.text;
            result.clear();
            vm->beginEvaluation(source);
            while (!vm->evaluationFinished() && !host.quit) cycle();
            if (host.quit) {
                if (hasExpect) throw std::runtime_error("image quit before evaluation returned");
                break;
            }
            result = vm->finishEvaluation();
            if (gc) vm->collectGarbage();
            std::cout << "RESULT " << result << '\n';
        }
        if (!display.empty() && host.width && host.height) {
            std::ofstream out(display);
            int bits = vm->getDisplayBits(host.width, host.height);
            out << "P1\n" << host.width << ' ' << host.height << '\n';
            for (int y = 0; y < host.height; ++y) {
                for (int x = 0; x < host.width; ++x)
                    out << ((vm->fetchWord_ofDislayBits(y*((host.width+15)/16)+x/16, bits) >> (15-x%16)) & 1) << ' ';
                out << '\n';
            }
        }
        if (hasExpect && result != expect) throw std::runtime_error("expected " + expect + ", got " + result);
        std::cout << "OK cycles=" << cycles << " display=" << host.width << 'x' << host.height
                  << " events=" << host.injected << '\n';
    } catch (const std::exception& e) {
        std::cerr << "ERROR " << e.what() << '\n';
        return 1;
    }
}
