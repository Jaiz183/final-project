#include "kvstore/quotient_filter.h"
#include <immintrin.h>
#include <algorithm>
#include <cmath>

// use same 64 bit hash function as bloom filter
std::uint64_t Mix(std::uint64_t x)
{
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

// according to paper, SELECT(v,i) = TZCNT(PDEP(2^i,v))
uint8_t select_word(uint64_t bitstring, uint8_t idx)
{
    return _tzcnt_u64(_pdep_u64((uint64_t)1 << idx, bitstring));
}

// according to paper, RANK(v,i) = POPCOUNT(v & (2^i -1))
uint8_t rank_word(uint64_t bitstring, uint8_t idx)
{
    return std::__popcount(_bzhi_u64(bitstring, idx));
}

uint32_t ceil_div(uint32_t x, uint32_t y)
{
    return (x + y - 1) / y;
}

namespace kvstore
{
    namespace quotient
    {
        std::uint64_t Hash(KeyType key)
        {
            uint64_t full_hash = Mix((uint64_t)key);
            return full_hash;
        }

        // TODO: merge 2 quotient filters efficiently instead of triggering a rebuild
    };
    // define constructor
    QuotientFilterBuilder::QuotientFilterBuilder(uint32_t r_pwr, uint32_t n_des)
    {
        if (r_pwr < 0 || r_pwr > 5)
        {
            throw Error("Remainder size not supported. Only non-fractional powers of 2 up to 64 are supported");
        }

        // use ceil(log2(n)) as quotient size since quotient indexes the hashmap
        q = std::__bit_width(n_des);
        if (q > 64)
        {
            throw Error("Quotient size not supported. Only non-negative values up to 64 are supported.");
        }

        n = 1 << q;
        r = 1 << r_pwr;

        // alloc hashmap
        // align to a round number of pages, there will be some empty space
        uint32_t map_num_pages = ceil_div(r * n, 8 * kPageSize);
        map.assign(n, false);

        // allocate offsets
        num_blks = ceil_div(n, 64);
        offset.assign(num_blks, false);

        // allocate runend, occ as words
        // each entry is a word and represents a block's offsets
        runend.assign(num_blks, 0);
        occupied.assign(num_blks, 0);

        // set up masks for getting quot., remainder later
        uint64_t full_mask = ~uint64_t(0);
        uint64_t q_mask = full_mask << (64 - q);
        uint64_t r_mask = (full_mask << r) - 1;
    }

    void
    QuotientFilterBuilder::Add(KeyType key)
    {
        uint64_t full_hash = quotient::Hash(key);
        uint64_t quotient = (q_mask & full_hash) >> (64 - q);
        uint64_t remainder = r_mask & full_hash;
    }

    // idx is the most significant (leftmost) bit
    uint64_t bits_to_int(const std::vector<bool> &bit_arr, uint64_t idx, uint64_t length)
    {
        uint64_t x = 0;
        for (uint64_t i = 0; i < length; ++i)
            x = (x << 1) | uint64_t(bit_arr[idx + i]);
        return x;
    }

    bool QuotientFilterBuilder::MayContain(KeyType key)
    {
        // get idx, then search for remainder
        uint64_t full_hash = quotient::Hash(key);
        uint64_t quotient = (q_mask & full_hash) >> (64 - q);
        uint64_t remainder = r_mask & full_hash;

        // get block num.
        int64_t block_num = quotient / 64;
        int64_t block_rem = quotient % 64; // idx in blk

        // get block info.
        uint64_t is_occ_blk = occupied[block_num];
        uint64_t is_runend_blk = runend[block_num];
        uint8_t off = offset[block_num];

        // check if slot is occupied and return
        bool is_occ = (!block_rem ? is_occ_blk : (is_occ_blk >> block_rem)) & (uint64_t)1;
        if (!is_occ)
        {
            return false;
        }

        // get how many runs until target run after first run of this blk
        uint8_t runs_till_slot = rank_word(is_occ, block_rem) - 1;
        // # runs completed after first run in this blk
        uint16_t num_runs_completed = rank_word(is_runend_blk, 63) - rank_word(is_runend_blk, off);
        uint64_t last_run_idx;
        if (num_runs_completed >= runs_till_slot)
        {
            last_run_idx = select_word(is_runend_blk, rank_word(is_runend_blk, off) + runs_till_slot) + block_num * 64;
        }
        else
        {
            for (size_t i = block_num + 1; i < num_blks; i++)
            {
                uint64_t curr_runend_blk = runend[i];
                num_runs_completed += rank_word(curr_runend_blk, 63);
                // we finally have enough runs in this slot
                if (num_runs_completed >= runs_till_slot)
                {
                    last_run_idx = select_word(curr_runend_blk, runs_till_slot - num_runs_completed + rank_word(curr_runend_blk, 63)) + block_num * 64;
                }
            }
        }

        // iterate backwards until we find a remainder that's equal to search or terminate when another runend is seen
        for (size_t i = last_run_idx; i >= 1; i -= 1)
        {
            uint64_t run_blk_idx = i;
            uint64_t curr_runend_blk = runend[run_blk_idx];
            for (size_t j = 0; i < 64; j++)
            {
                uint64_t elt_idx = i * 64 - j; // idx of an elt in the slot we're exploring
                uint64_t map_idx = elt_idx * r;
                uint64_t rem = bits_to_int(map, map_idx - r, r);
                // first ensure that we aren't done with the run
                if (rem == remainder)
                {
                    return true;
                }
                // shift right to get next slot's runend
                curr_runend_blk >>= 1;
                uint64_t curr_runend_bit = curr_runend_blk & (uint64_t)1;
                // if we reached a runend, we can stop because next int read will be from another slot
                if (curr_runend_bit)
                {
                    return false;
                }
            }
        }

        return false;
    }

    QuotientFilterBuilder::~QuotientFilterBuilder()
    {
    }
};