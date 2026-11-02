#include <cstddef>
#include <cstdint>
#include <vector>

#include "kvstore/types.h"

// methods required by the filter or methods that can be performed on the filter
namespace kvstore
{
    namespace quotient
    {
        std::uint64_t Hash(KeyType key, uint32_t q, uint32_t r);
        QuotientFilterBuilder Merge(const QuotientFilterBuilder &qf1, const QuotientFilterBuilder &qf2);
    }
    // the filter itself and mutating methods
    class QuotientFilterBuilder
    {
    public:
        // only support powers of 2 as remainder size
        QuotientFilterBuilder(std::uint32_t r_pwr, std::uint32_t n_des);
        void Add(KeyType key);
        bool MayContain(KeyType key);
        ~QuotientFilterBuilder();

    private:
        std::vector<bool> map;
        // whether a run ends at this bucket
        std::vector<uint64_t> occupied;
        // whether a run ends at this bucket
        std::vector<uint64_t> runend;
        // offset from first occ to its runend
        std::vector<std::uint8_t> offset;
        uint32_t n;
        uint32_t q;
        uint32_t r;
        uint64_t q_mask;
        uint64_t r_mask;
        uint64_t num_blks;
    };
};
