#include <cstddef>
#include <cstdint>
#include <vector>

#include "kvstore/types.h"

// methods required by the filter or methods that can be performed on the filter
namespace kvstore {
    namespace quotient {
        std::uint64_t Hash(KeyType key);
        QuotientFilterBuilder Merge(const QuotientFilter& qf1, const QuotientFilter& qf2);
    }
}

// the filter itself and mutating methods
class QuotientFilterBuilder {
    public:
        QuotientFilterBuilder(std::uint32_t q,
                     std::uint32_t r);
        void Add(KeyType key);
        bool MayContain(KeyType key);

    private:
        
}