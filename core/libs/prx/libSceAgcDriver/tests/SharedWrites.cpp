#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libc/include/GuestWriteWatch.hpp"
#include "prx/libc/include/GuestArena.hpp"
#include "prx/libc/include/general/VabiMacros.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <iostream>
#include <fstream>
#include <memory>
#include <string>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/resource.h>
#endif

extern "C" {
int APS5_VABI sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int APS5_VABI sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int APS5_VABI sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int APS5_VABI sceKernelMunmap(void*, std::size_t);
int APS5_VABI sceKernelMprotect(const void*, std::size_t, int);
}

namespace {
using namespace AgcDriver::GuestMemory;
void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
struct Mapping {
    std::size_t size;
    std::int64_t physical = -1;
    void* data = nullptr;
    explicit Mapping(std::size_t size) : size(size) {
        require(sceKernelAllocateDirectMemory(0, 0x10000000000ll, size, 65536, 3, &physical) == 0, "allocate direct memory failed");
        if (sceKernelMapDirectMemory(&data, size, 3, 0, physical, 65536) != 0) {
            sceKernelReleaseDirectMemory(physical, size);
            throw std::runtime_error("map direct memory failed");
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() {
        if (data != nullptr) sceKernelMunmap(data, size);
        if (physical >= 0) sceKernelReleaseDirectMemory(physical, size);
    }
    std::uint64_t Address() const { return reinterpret_cast<std::uint64_t>(data); }
};

void benchmark(bool labels = false) {
    require(WriteWatched(), "write tracking unavailable for benchmark");
    std::thread([] {}).join();
    Mapping mapping(2u << 20u);
    std::vector<std::byte> source(1u << 20u, std::byte{17});
    const auto sizes = labels ? std::array<std::size_t, 3>{4, 8, 16} : std::array<std::size_t, 3>{4096, 65536, 1048576};
    for (const auto bytes : sizes) {
        std::vector<double> times;
        std::uint64_t faults = 0;
        for (unsigned pass = 0; pass < 10; ++pass) {
#ifndef _WIN32
            rusage before{}, after{};
            getrusage(RUSAGE_THREAD, &before);
#endif
            const auto start = std::chrono::steady_clock::now();
            for (unsigned i = 0; i < 256; ++i) {
                source[0] = static_cast<std::byte>(i);
                BumpCollectEpoch();
                if (labels) {
                    CheckRange(mapping.data, bytes, 4, true);
                    StoreOwnBytes(mapping.Address(), std::span(source).first(bytes));
                } else Write(mapping.Address(), std::span(source).first(bytes), 1);
            }
            const auto us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 256;
#ifndef _WIN32
            getrusage(RUSAGE_THREAD, &after);
            if (pass != 0) faults += after.ru_minflt - before.ru_minflt;
#endif
            if (pass != 0) times.push_back(us);
            require(std::memcmp(mapping.data, source.data(), bytes) == 0, "copied data differs");
        }
        std::sort(times.begin(), times.end());
        std::cout << "bytes=" << bytes << " median_us=" << times[4] << " faults=" << faults << " operations=2304\n";
    }
}

void benchmarkVersions() {
    require(WriteWatched(), "write tracking unavailable for version benchmark");
    std::thread([] {}).join();
    Mapping mapping(64u << 20u);
    std::memset(mapping.data, 17, mapping.size);
    constexpr unsigned iterations = 16384;
    for (const auto blocks : {1u, 16u, 64u, 512u, 1024u}) {
        const std::size_t bytes = blocks * 65536u;
        for (const auto dirty : {0u, 1u, 2u}) {
            const auto generation = CollectWritesUncached(mapping.Address(), bytes);
            require(generation != 0, "version benchmark could not collect watched memory");
            if (dirty != 0) require(MarkWritten(mapping.Address() + (dirty == 1 ? 0 : bytes - 1), 1) > generation, "version benchmark write was not stamped");
            std::array<double, 9> times{};
            std::uint64_t unchanged = 0;
            for (unsigned pass = 0; pass <= times.size(); ++pass) {
                const auto started = std::chrono::steady_clock::now();
                for (unsigned i = 0; i < iterations; ++i) unchanged += UnchangedSince(mapping.Address(), bytes, generation);
                if (pass != 0) times[pass - 1] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() / iterations;
            }
            require(unchanged == (dirty == 0 ? (times.size() + 1) * iterations : 0), "version benchmark returned stale memory");
            std::ranges::sort(times);
            std::cout << "Version scan blocks=" << blocks << " dirty=" << dirty << " median_ns=" << times[4] << " p95_ns=" << times.back() << "\n";
        }
    }
}

void benchmarkCollect(const char* path) {
    require(WriteWatched(), "write tracking unavailable for collect benchmark");
    std::thread([] {}).join();
    std::ifstream input(path, std::ios::binary);
    const auto read = [&] {
        std::uint64_t value = 0;
        require(static_cast<bool>(input.read(reinterpret_cast<char*>(&value), sizeof(value))), "truncated collect fixture");
        return value;
    };
    const auto mappingCount = read();
    const auto requestCount = read();
    require(mappingCount <= 1024 && requestCount != 0 && requestCount <= 100000, "invalid collect fixture counts");
    std::vector<std::unique_ptr<Mapping>> mappings;
    std::size_t totalBytes = 0;
    for (std::size_t i = 0; i < mappingCount; ++i) {
        const auto bytes = read();
        require(bytes != 0 && bytes % 65536 == 0 && bytes <= (std::size_t{1} << 30) && totalBytes + bytes <= (std::size_t{2} << 30), "invalid collect fixture mapping");
        totalBytes += bytes;
        auto mapping = std::make_unique<Mapping>(bytes);
        std::memset(mapping->data, 17, bytes);
        require(CollectWritesUncached(mapping->Address(), bytes) != 0, "collect fixture mapping is unwatched");
        mappings.push_back(std::move(mapping));
    }
    struct Request { std::uint64_t address, bytes, epoch; bool memoized; };
    std::vector<Request> requests;
    for (std::size_t i = 0; i < requestCount; ++i) {
        const auto index = read();
        const auto offset = read();
        const auto bytes = read();
        const auto epoch = read();
        const auto memoized = read();
        require(index < mappings.size() && bytes != 0 && bytes <= mappings[index]->size && offset <= mappings[index]->size - bytes, "invalid collect fixture request");
        requests.push_back({mappings[index]->Address() + offset, bytes, epoch, memoized != 0});
    }
    for (const bool dirty : {false, true}) {
        std::array<double, 9> times{};
        std::uint64_t completed = 0;
        for (std::size_t pass = 0; pass <= times.size(); ++pass) {
            const auto started = std::chrono::steady_clock::now();
            for (unsigned repeat = 0; repeat < 16; ++repeat) {
                std::uint64_t epoch = 0;
                for (std::size_t i = 0; i < requests.size(); ++i) {
                    const auto& request = requests[i];
                    if (i == 0 || request.epoch != epoch) { BumpCollectEpoch(); epoch = request.epoch; }
                    if (dirty && i % 64 == 0) *reinterpret_cast<volatile std::uint8_t*>(request.address) = static_cast<std::uint8_t>(repeat + pass);
                    completed += (request.memoized ? CollectWrites(request.address, request.bytes) : CollectWritesUncached(request.address, request.bytes)) != 0;
                }
            }
            if (pass != 0) times[pass - 1] = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - started).count() / (16 * requests.size());
        }
        require(completed == (times.size() + 1) * 16 * requests.size(), "collect replay lost watched coverage");
        std::ranges::sort(times);
        std::cout << "Collect replay requests=" << requests.size() << " dirty=" << dirty << " median_ns=" << times[4] << " p95_ns=" << times.back() << "\n";
    }
}

void smallStores() {
    Mapping mapping(2u << 16u);
    auto* data = static_cast<std::byte*>(mapping.data);
    std::memset(data, 19, mapping.size);
    std::array<std::byte, 16> source;
    source.fill(std::byte{83});
    for (const auto offset : {4u, 4092u, 65532u}) for (const auto size : {4u, 8u, 16u}) {
        const auto address = mapping.Address() + offset;
        const auto before = CollectWritesUncached(address, size);
        const auto stored = StoreOwnBytes(address, std::span(source).first(size));
        require(std::memcmp(data + offset, source.data(), size) == 0, "small backing store differs");
        require(data[offset - 1] == std::byte{19} && data[offset + 16] == std::byte{19}, "small store changed neighboring bytes");
        if (before != 0) {
            require(stored > before && StoredOver(address, size, before), "small driver store was not stamped");
            require(UnchangedSinceCollected(address, size, before), "driver store was classified as a guest write");
            data[offset] = std::byte{97};
            StoreOwnBytes(address, std::span(source).first(size));
            require(!UnchangedSinceCollected(address, size, stored), "guest write before a driver store was lost");
            const auto after = CollectWritesUncached(address, size);
            data[offset] = std::byte{101};
            require(!UnchangedSinceCollected(address, size, after), "guest write after a driver store was lost");
        }
    }
    std::array<std::byte, 16> ordinary{};
    StoreOwnBytes(reinterpret_cast<std::uintptr_t>(ordinary.data()), source);
    require(ordinary == source, "small ordinary store fallback differs");
    require(StoreOwnBytes(0, std::span<const std::byte>{}) == 0, "empty store produced a version");
}

template<class F> void fails(F&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::runtime_error&) { failed = true; }
    require(failed, "invalid write was accepted");
}

#ifndef _WIN32
std::size_t directMappings() {
    std::ifstream maps("/proc/self/maps");
    require(maps.is_open(), "cannot inspect mapping lifetimes");
    std::size_t count = 0;
    for (std::string line; std::getline(maps, line);) if (line.find("/memfd:direct memory") != std::string::npos) ++count;
    return count;
}
#endif

void run() {
    constexpr std::size_t part = 65536;
    Mapping mapping(2u << 20u);
    auto* bytes = static_cast<std::byte*>(mapping.data);
    std::memset(bytes, 19, mapping.size);
    std::vector<std::byte> source(part, std::byte{73});
    const auto before = CollectWritesUncached(mapping.Address(), mapping.size);
    Write(mapping.Address() + 123, source, 1);
    require(std::memcmp(bytes + 123, source.data(), source.size()) == 0, "shared write differs");
    require(bytes[122] == std::byte{19} && bytes[123 + part] == std::byte{19}, "write changed neighboring bytes");
    if (before != 0) {
        require(!UnchangedSince(mapping.Address(), mapping.size, before), "driver write did not advance versions");
        require(Watched(mapping.Address(), mapping.size), "host view disabled guest tracking");
        const auto after = CollectWritesUncached(mapping.Address(), mapping.size);
        require(UnchangedSince(mapping.Address(), mapping.size, after), "fresh write version is stale");
        bytes[part + 456] = std::byte{91};
        CollectWritesUncached(mapping.Address(), mapping.size);
        require(!UnchangedSince(mapping.Address(), mapping.size, after), "subsequent guest write was lost");
#ifndef _WIN32
        std::size_t dirty = 0;
        const auto collect = [&] {
            return GuestWriteWatch::GuestWriteWatchCollect_nid_postfix(mapping.Address(), mapping.size,
                [](void* p, std::uintptr_t begin, std::uintptr_t end) { *static_cast<std::size_t*>(p) += end - begin; }, &dirty);
        };
        require(collect(), "guest mapping is no longer watched");
        dirty = 0;
        require(GuestArena::GuestArenaWriteSharedBacking_nid_postfix(mapping.Address(), source.data(), source.size()), "shared writer rejected direct backing");
        require(collect() && dirty == 0, "host write dirtied the tracked guest mapping");
        bytes[part * 4] = std::byte{31};
        require(collect() && dirty == 4096, "guest write tracking was disarmed");
#endif
    }
    require(sceKernelMprotect(mapping.data, part, 1) == 0, "read-only protection failed");
    fails([&] { Write(mapping.Address(), source, 1); });
    require(sceKernelMprotect(mapping.data, part, 3) == 0, "write protection restore failed");
    std::vector<std::byte> ordinary(part);
    Write(reinterpret_cast<std::uint64_t>(ordinary.data()), source, 1);
    require(ordinary == source, "ordinary memory fallback failed");
    {
        Mapping replacement(part);
        void* originalAlias = nullptr;
        require(sceKernelMapDirectMemory(&originalAlias, part, 3, 0, mapping.physical + part, part) == 0, "physical alias failed");
        std::memset(originalAlias, 29, part);
        void* middle = bytes + part;
        require(sceKernelMapDirectMemory(&middle, part, 3, 0x10, replacement.physical, part) == 0, "fixed backing replacement failed");
        std::vector<std::byte> across(part * 3);
        for (std::size_t i = 0; i < across.size(); ++i) across[i] = static_cast<std::byte>((i / part) + 41);
        Write(mapping.Address(), across, 1);
        require(std::memcmp(mapping.data, across.data(), across.size()) == 0, "write across backing boundaries differs");
        require(std::memcmp(replacement.data, across.data() + part, part) == 0, "physical alias did not see the write");
        require(std::all_of(static_cast<std::byte*>(originalAlias), static_cast<std::byte*>(originalAlias) + part, [](auto x) { return x == std::byte{29}; }), "replaced virtual range wrote the old backing");
#ifndef _WIN32
        require(sceKernelMunmap(middle, part) == 0, "partial unmap failed");
        const auto saved = bytes[0];
        across[0] = std::byte{99};
        require(!GuestArena::GuestArenaWriteSharedBacking_nid_postfix(mapping.Address(), across.data(), across.size()), "writer accepted an unmapped gap");
        require(bytes[0] == saved, "failed shared write changed its prefix");
#endif
        require(sceKernelMapDirectMemory(&middle, part, 3, 0x10, mapping.physical + part, part) == 0, "original backing restore failed");
        require(sceKernelMunmap(originalAlias, part) == 0, "alias unmap failed");
    }
    std::vector<std::thread> threads;
    for (unsigned i = 0; i < 8; ++i) threads.emplace_back([&, i] {
        std::vector<std::byte> input(part, static_cast<std::byte>(i + 57));
        for (unsigned repeat = 0; repeat < 32; ++repeat) Write(mapping.Address() + i * part, input, 1);
    });
    for (auto& thread : threads) thread.join();
    for (unsigned i = 0; i < 8; ++i) require(std::all_of(bytes + i * part, bytes + (i + 1) * part, [i](auto x) { return x == static_cast<std::byte>(i + 57); }), "concurrent write differs");
}

}

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--benchmark") benchmark();
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark-labels") benchmark(true);
        else if (argc == 2 && std::string_view(argv[1]) == "--benchmark-versions") benchmarkVersions();
        else if (argc == 3 && std::string_view(argv[1]) == "--benchmark-collect") benchmarkCollect(argv[2]);
        else {
#ifndef _WIN32
            const auto before = directMappings();
#endif
            run();
            smallStores();
#ifndef _WIN32
            require(directMappings() == before, "physical backing write views leaked");
#endif
            std::cout << "Shared backing write tests passed\n";
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
