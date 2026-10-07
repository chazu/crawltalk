#include "objmemory.h"
#include "posixfilesystem.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>

class FailingFiles : public PosixST80FileSystem {
public:
    using PosixST80FileSystem::PosixST80FileSystem;
    int failure = 0, temporary = -1;
    int create_file_exclusive(const char* name) override {
        return temporary = PosixST80FileSystem::create_file_exclusive(name);
    }
    int write(int fd, const char* bytes, int length) override {
        if (fd == temporary && failure == 1)
            return PosixST80FileSystem::write(fd, bytes, length / 2);
        return PosixST80FileSystem::write(fd, bytes, length);
    }
    bool file_flush(int fd) override {
        return failure != 2 && PosixST80FileSystem::file_flush(fd);
    }
    int close_file(int fd) override {
        int result = PosixST80FileSystem::close_file(fd);
        return fd == temporary && failure == 3 ? -1 : result;
    }
    bool rename_file(const char* from, const char* to) override {
        return failure != 4 && PosixST80FileSystem::rename_file(from, to);
    }
};
static std::string contents(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("snapshot-test needs an isolated image directory");
        FailingFiles files(argv[1]);
        auto memory = std::make_unique<ObjectMemory>(nullptr, nullptr);
        if (!memory->loadSnapshot(&files, "snapshot.im")) throw std::runtime_error("seed load failed");
        const auto original = contents(files.path_for_file("snapshot.im"));
        for (int failure = 1; failure <= 4; ++failure) {
            files.failure = failure;
            if (memory->saveSnapshot(&files, "snapshot.im")) throw std::runtime_error("failed save reported success");
            if (contents(files.path_for_file("snapshot.im")) != original) throw std::runtime_error("previous snapshot damaged");
            files.enumerate_files([](const char* name) {
                if (std::string(name).find(".tmp.") != std::string::npos) throw std::runtime_error("staging file leaked");
            });
        }
        files.failure = 0;
        if (!memory->saveSnapshot(&files, "snapshot.im")) throw std::runtime_error("successful save failed");
        auto restored = std::make_unique<ObjectMemory>(nullptr, nullptr);
        if (!restored->loadSnapshot(&files, "snapshot.im")) throw std::runtime_error("replacement image invalid");
        std::cout << "snapshot write, flush, close, rename failures preserve previous image\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
