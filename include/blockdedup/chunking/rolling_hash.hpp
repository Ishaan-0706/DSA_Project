#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace blockdedup::chunking {
class RollingHash {
  public:
    RollingHash(std::size_t window_size);
    std::uint32_t get_hash();
    void roll(std::uint8_t byte);
    void reset();

  private:
    std::size_t window_size_;
    std::deque<std::uint8_t> window_;
    std::uint32_t hash_;
    std::uint32_t base_;
    std::uint32_t top_power_;
};
class Chunker {
  public:
    Chunker(std::size_t window_size, std::size_t min_size, std::size_t max_size,
            std::uint64_t mask);

    std::vector<std::size_t> ProcessData(const std::vector<std::uint8_t>& data);

  private:
    RollingHash hasher_;
    std::size_t min_size_;
    std::size_t max_size_;
    std::uint64_t mask_;
};

}  