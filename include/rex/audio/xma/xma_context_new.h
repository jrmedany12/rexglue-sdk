/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <array>
#include <cstdint>

#include <rex/audio/xma/context.h>
#include <rex/memory/ring_buffer.h>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace rex::audio {

// [XMA fix] XmaContextNew is a port of Xenia Canary's rewritten XMA decoder.
// The old decoder (XmaContextLegacy) processed one full 512-sample frame at a
// time which broke games that expect output in 128-sample subframe chunks.
// This decoder tracks output at subframe granularity, handles loop start/end
// boundaries at subframe precision via loop_frame_output_limit_ and
// loop_start_skip_pending_, and only writes back the context fields it actually
// changed (StoreContextMerged) instead of overwriting the whole 64-byte struct
// — avoiding race conditions when the game updates the same memory concurrently.
// This is the default decoder. Use xma_decoder=old to fall back to the legacy one.
struct XmaPacketInfo {
  uint8_t frame_count_ = 0;
  uint8_t current_frame_ = 0;
  uint32_t current_frame_size_ = 0;

  bool isLastFrameInPacket() const {
    return frame_count_ == 0 || current_frame_ == frame_count_ - 1;
  }
};

static constexpr int kIdToSampleRate[4] = {24000, 32000, 44100, 48000};

class XmaContextNew : public XmaContext {
 public:
  static constexpr uint32_t kMaxFrameSizeinBits = 0x4000 - kBitsPerPacketHeader;

  XmaContextNew();
  ~XmaContextNew() override;

  int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) override;
  bool Work() override;

  void Enable() override;
  void Clear() override;
  void Disable() override;
  void Release() override;

 private:
  void ClearLocked(XMA_CONTEXT_DATA* data);
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  static int16_t GetPacketNumber(size_t size, size_t bit_offset);

  XmaPacketInfo GetPacketInfo(uint8_t* packet, uint32_t frame_offset);

  uint32_t GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size);

  const uint8_t* GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                               uint32_t current_input_packet_count);

  uint32_t GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                   uint32_t current_input_packet_count);

  uint8_t* GetCurrentInputBuffer(XMA_CONTEXT_DATA* data);
  static uint32_t GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data);

  void Decode(XMA_CONTEXT_DATA* data);
  void Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data);

  void UpdateLoopStatus(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(int sample_rate, bool is_two_channel);
  void PreparePacket(uint32_t frame_size, uint32_t frame_padding);

  memory::RingBuffer PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data);

  bool DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet, AVFrame* av_frame);

  void StoreContextMerged(const XMA_CONTEXT_DATA& data, const XMA_CONTEXT_DATA& initial_data,
                          uint8_t* context_ptr);

  std::array<uint8_t, kBytesPerPacketData * 2> input_buffer_;
  std::array<uint8_t, 1 + 4096> xma_frame_;
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;

  int32_t remaining_subframe_blocks_in_output_buffer_ = 0;
  uint8_t current_frame_remaining_subframes_ = 0;
  uint8_t loop_frame_output_limit_ = 0;
  bool loop_start_skip_pending_ = false;
};

}  // namespace rex::audio
