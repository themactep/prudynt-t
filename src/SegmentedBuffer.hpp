#ifndef SEGMENTED_BUFFER_HPP
#define SEGMENTED_BUFFER_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

// Segmented buffer: a chain of fixed-size blocks that grows without
// copying existing data.  Replaces std::vector<uint8_t> for the frame
// accumulation pattern where data is appended piecewise then consumed
// in one shot.
//
// Example:
//   SegmentedBuffer buf;
//   buf.append(data, len);   // zero-copy growth (new block if needed)
//   buf.append(more, len2);  // same
//   buf.copy_out(dst);       // linearize into contiguous output
//   buf.clear();             // reset, retain block storage
class SegmentedBuffer {
public:
  static constexpr size_t kBlockSize = 16384; // 16KB blocks

  SegmentedBuffer() = default;
  ~SegmentedBuffer() = default;

  // Non-copyable, movable
  SegmentedBuffer(const SegmentedBuffer &) = delete;
  SegmentedBuffer &operator=(const SegmentedBuffer &) = delete;
  SegmentedBuffer(SegmentedBuffer &&) noexcept = default;
  SegmentedBuffer &operator=(SegmentedBuffer &&) noexcept = default;

  // Return pointer to contiguous data.  If the data spans multiple blocks
  // this triggers a one-time linearization into a cache buffer.
  const uint8_t *data() const {
    if (total_size_ == 0) return nullptr;
    if (blocks_.size() <= 1) return blocks_[0].data();
    // Multi-block: linearize on first access
    if (linearized_.empty()) {
      const_cast<SegmentedBuffer *>(this)->copy_out(linearized_);
    }
    return linearized_.data();
  }

  // Append |len| bytes from |data|.  Grows by adding blocks as needed.
  void append(const uint8_t *data, size_t len) {
    if (!data || len == 0) return;

    size_t remaining = len;
    while (remaining > 0) {
      if (blocks_.empty() || write_offset_ == kBlockSize) {
        blocks_.emplace_back();
        write_offset_ = 0;
      }
      auto &block = blocks_.back();
      size_t to_copy = remaining;
      if (write_offset_ + to_copy > kBlockSize) {
        to_copy = kBlockSize - write_offset_;
      }
      std::copy(data, data + to_copy, block.data() + write_offset_);
      data += to_copy;
      write_offset_ += to_copy;
      total_size_ += to_copy;
      remaining -= to_copy;
    }
  }

  // Copy the entire buffer into a contiguous |dst| vector.
  void copy_out(std::vector<uint8_t> &dst) const {
    dst.resize(total_size_);
    uint8_t *ptr = dst.data();
    for (size_t i = 0; i < blocks_.size(); ++i) {
      size_t n = block_data_size(i);
      std::copy_n(blocks_[i].data(), n, ptr);
      ptr += n;
    }
  }

  // Reset without freeing block storage (blocks are reused).
  void clear() {
    total_size_ = 0;
    write_offset_ = 0;
    linearized_.clear();
    // Keep blocks_ allocated but mark them empty.
  }

  // Free all blocks.
  void release() {
    blocks_.clear();
    blocks_.shrink_to_fit();
    linearized_.clear();
    linearized_.shrink_to_fit();
    total_size_ = 0;
    write_offset_ = 0;
  }

  size_t size() const { return total_size_; }
  bool empty() const { return total_size_ == 0; }
  size_t capacity() const { return blocks_.size() * kBlockSize; }

private:
  struct Block {
    uint8_t data_[kBlockSize];
    uint8_t *data() { return data_; }
    const uint8_t *data() const { return data_; }
  };

  std::vector<Block> blocks_;
  mutable std::vector<uint8_t> linearized_; // lazily populated for multi-block access
  size_t write_offset_ = 0;
  size_t total_size_ = 0;

  size_t block_data_size(size_t idx) const {
    if (idx < blocks_.size() - 1) return kBlockSize;
    return write_offset_;
  }
};

#endif // SEGMENTED_BUFFER_HPP
