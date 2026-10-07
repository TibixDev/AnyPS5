#ifndef CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERREADMASK_HPP
#define CORE_LIBS_PRX_LIBSCEAGCDRIVER_EXECUTION_INCLUDE_SHADERREADMASK_HPP

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace AgcDriver {

template<std::size_t TSize>
class ShaderReadMask {
public:
    void Set(std::size_t index) {
        if (index >= TSize) throw std::out_of_range("shader read mask index");
        words[index / 64] |= std::uint64_t{1} << (index % 64);
    }

    void Reset() { words.fill(0); }

    template<typename TVisitor>
    void ForEachRun(TVisitor&& visit) const {
        std::size_t first = 0, end = 0;
        for (std::size_t block = 0; block < words.size(); ++block) {
            auto bits = words[block];
            while (bits != 0) {
                const auto offset = std::countr_zero(bits);
                const auto limit = offset + std::countr_one(bits >> offset);
                const auto begin = block * 64 + offset;
                if (begin != end) {
                    if (first != end) visit(first, end - first);
                    first = begin;
                }
                end = block * 64 + limit;
                bits = limit == 64 ? 0 : bits & (~std::uint64_t{0} << limit);
            }
        }
        if (first != end) visit(first, end - first);
    }

private:
    std::array<std::uint64_t, (TSize + 63) / 64> words{};
};

}

#endif
