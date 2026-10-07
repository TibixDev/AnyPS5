#ifndef CORE_SHADER_RECOMPILER_RESULTMEMOADMISSION_HPP
#define CORE_SHADER_RECOMPILER_RESULTMEMOADMISSION_HPP

#include <array>
#include <cstddef>
#include <cstdint>

namespace ShaderRecompiler::Detail {

inline std::uint64_t ResultMemoIndex(std::uint64_t variant, std::uint64_t hash) {
    return (variant * 0x9e3779b97f4a7c15ull) ^ hash;
}

class ResultMemoAdmission {
public:
    bool Observe(std::uint64_t variant, std::uint64_t hash) {
        const Key key{variant, hash};
        auto bucket = Find(key);
        if (buckets[bucket] != 0) return true;
        if (size == entries.size()) {
            Erase(entries[next]);
            bucket = Find(key);
        } else {
            ++size;
        }
        entries[next] = key;
        buckets[bucket] = static_cast<std::uint16_t>(next + 1);
        next = (next + 1) % entries.size();
        return false;
    }

private:
    struct Key {
        std::uint64_t variant;
        std::uint64_t hash;
        bool operator==(const Key&) const = default;
    };
    static constexpr std::size_t BucketMask = 511;
    static std::size_t Home(const Key& key) {
        const auto hash = ResultMemoIndex(key.variant, key.hash);
        return (hash ^ (hash >> 33u)) & BucketMask;
    }
    std::size_t Find(const Key& key) const {
        auto bucket = Home(key);
        while (buckets[bucket] != 0 && entries[buckets[bucket] - 1] != key) bucket = (bucket + 1) & BucketMask;
        return bucket;
    }
    void Erase(const Key& key) {
        auto hole = Find(key);
        auto scan = (hole + 1) & BucketMask;
        while (buckets[scan] != 0) {
            const auto home = Home(entries[buckets[scan] - 1]);
            if (((scan - home) & BucketMask) >= ((scan - hole) & BucketMask)) {
                buckets[hole] = buckets[scan];
                hole = scan;
            }
            scan = (scan + 1) & BucketMask;
        }
        buckets[hole] = 0;
    }
    std::array<Key, 256> entries{};
    std::array<std::uint16_t, BucketMask + 1> buckets{};
    std::size_t next = 0;
    std::size_t size = 0;
};

}

#endif
