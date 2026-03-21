/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <rex/assert.h>
#include <rex/kernel.h>
#include <rex/memory.h>
#include <rex/thread.h>

struct AVCodec;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rex::audio {

struct XMA_CONTEXT_DATA {
  uint32_t input_buffer_0_packet_count : 12;
  uint32_t loop_count : 8;
  uint32_t input_buffer_0_valid : 1;
  uint32_t input_buffer_1_valid : 1;
  uint32_t output_buffer_block_count : 5;
  uint32_t output_buffer_write_offset : 5;

  uint32_t input_buffer_1_packet_count : 12;
  uint32_t loop_subframe_start : 2;
  uint32_t loop_subframe_end : 3;
  uint32_t loop_subframe_skip : 3;
  uint32_t subframe_decode_count : 4;
  uint32_t output_buffer_padding : 3;
  uint32_t sample_rate : 2;
  uint32_t is_stereo : 1;
  uint32_t unk_dword_1_c : 1;
  uint32_t output_buffer_valid : 1;

  uint32_t input_buffer_read_offset : 26;
  uint32_t error_status : 5;
  uint32_t error_set : 1;

  uint32_t loop_start : 26;
  uint32_t parser_error_status : 5;
  uint32_t parser_error_set : 1;

  uint32_t loop_end : 26;
  uint32_t packet_metadata : 5;
  uint32_t current_buffer : 1;

  uint32_t input_buffer_0_ptr;
  uint32_t input_buffer_1_ptr;
  uint32_t output_buffer_ptr;
  uint32_t work_buffer_ptr;

  uint32_t output_buffer_read_offset : 5;
  uint32_t : 25;
  uint32_t stop_when_done : 1;
  uint32_t interrupt_when_done : 1;

  uint32_t unk_dwords_10_15[6];

  explicit XMA_CONTEXT_DATA(const void* ptr) {
    memory::copy_and_swap(reinterpret_cast<uint32_t*>(this), reinterpret_cast<const uint32_t*>(ptr),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  void Store(void* ptr) {
    memory::copy_and_swap(reinterpret_cast<uint32_t*>(ptr), reinterpret_cast<const uint32_t*>(this),
                          sizeof(XMA_CONTEXT_DATA) / 4);
  }

  bool IsInputBufferValid(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_valid : input_buffer_1_valid;
  }

  bool IsCurrentInputBufferValid() const { return IsInputBufferValid(current_buffer); }

  bool IsAnyInputBufferValid() const { return input_buffer_0_valid || input_buffer_1_valid; }

  uint32_t GetInputBufferAddress(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_ptr : input_buffer_1_ptr;
  }

  uint32_t GetCurrentInputBufferAddress() const { return GetInputBufferAddress(current_buffer); }

  uint32_t GetInputBufferPacketCount(uint8_t buffer_index) const {
    return buffer_index == 0 ? input_buffer_0_packet_count : input_buffer_1_packet_count;
  }

  uint32_t GetCurrentInputBufferPacketCount() const {
    return GetInputBufferPacketCount(current_buffer);
  }

  bool IsStreamingContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 1;
  }

  bool IsConsumeOnlyContext() const {
    return (input_buffer_0_packet_count | input_buffer_1_packet_count) == 0;
  }
};
static_assert_size(XMA_CONTEXT_DATA, 64);

#pragma pack(push, 1)
struct Xma2ExtraData {
  uint8_t raw[34];
};
static_assert_size(Xma2ExtraData, 34);
#pragma pack(pop)

class XmaContext {
 public:
  static constexpr uint32_t kBytesPerPacket = 2048;
  static constexpr uint32_t kBytesPerPacketHeader = 4;
  static constexpr uint32_t kBytesPerPacketData = kBytesPerPacket - kBytesPerPacketHeader;

  static constexpr uint32_t kBitsPerPacket = kBytesPerPacket * 8;
  static constexpr uint32_t kBitsPerPacketHeader = 32;
  static constexpr uint32_t kBitsPerFrameHeader = 15;

  static constexpr uint32_t kBytesPerSample = 2;
  static constexpr uint32_t kSamplesPerFrame = 512;
  static constexpr uint32_t kSamplesPerSubframe = 128;
  static constexpr uint32_t kBytesPerFrameChannel = kSamplesPerFrame * kBytesPerSample;
  static constexpr uint32_t kBytesPerSubframeChannel = kSamplesPerSubframe * kBytesPerSample;

  static constexpr uint32_t kOutputBytesPerBlock = 256;
  static constexpr uint32_t kOutputMaxSizeBytes = 31 * kOutputBytesPerBlock;

  static constexpr uint32_t kLastFrameMarker = 0x7FFF;

  XmaContext();
  virtual ~XmaContext();

  virtual int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
    (void)id;
    (void)memory;
    (void)guest_ptr;
    return 0;
  }
  virtual bool Work() { return false; }

  virtual void Enable() {}
  virtual bool Block(bool poll);
  virtual void Clear() {}
  virtual void Disable() {}
  virtual void Release() {}

  memory::Memory* memory() const { return memory_; }

  uint32_t id() { return id_; }
  uint32_t guest_ptr() { return guest_ptr_; }
  bool is_allocated() { return is_allocated_.load(std::memory_order_acquire); }
  bool is_enabled() { return is_enabled_.load(std::memory_order_acquire); }

  void set_is_allocated(bool is_allocated) {
    is_allocated_.store(is_allocated, std::memory_order_release);
  }
  void set_is_enabled(bool is_enabled) { is_enabled_.store(is_enabled, std::memory_order_release); }

  void SignalWorkDone() {
    if (work_completion_event_) {
      work_completion_event_->Set();
    }
  }
  void WaitForWorkDone() {
    if (work_completion_event_) {
      rex::thread::Wait(work_completion_event_.get(), false);
    }
  }

 protected:
  static void ConvertFrame(const uint8_t** samples, bool is_two_channel, uint8_t* output_buffer);

  memory::Memory* memory_ = nullptr;
  std::unique_ptr<rex::thread::Event> work_completion_event_;

  uint32_t id_ = 0;
  uint32_t guest_ptr_ = 0;
  std::mutex lock_;
  std::atomic<bool> is_allocated_ = false;
  std::atomic<bool> is_enabled_ = false;

  AVPacket* av_packet_ = nullptr;
  const AVCodec* av_codec_ = nullptr;
  AVCodecContext* av_context_ = nullptr;
  AVFrame* av_frame_ = nullptr;
};

}  // namespace rex::audio
