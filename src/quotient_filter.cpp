#include "kvstore/quotient_filter.h"

#include <algorithm>
#include <cmath>

// use same 64 bit hash function as bloom filter
std::uint64_t Mix(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

namespace kvstore {
    namespace quotient {
        std:uint64

        // TODO: merge 2 quotient filters efficiently instead of triggering a rebuild
        QuotientFilterBuilder merge(const QuotientFilter& qf1, const QuotientFilter& qf2) {
            ...
        }
    }
}

// define constructor
QuotientFilterBuilder:QuotientFilterBuilder(std::uint32_t q, std::uint32_t r) {
    
}