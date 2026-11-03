#include "blockdedup/chunking/rolling_hash.hpp"

namespace blockdedup::chunking {
    RollingHash::RollingHash(std::size_t window_size)
        : window_size_(window_size), hash_(0), base_(257), top_power_(1) {
        for (std::size_t i = 0; i < window_size_; ++i) {
            top_power_ *= base_;
        }
    }

}