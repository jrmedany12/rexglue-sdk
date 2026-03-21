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
#include <cstdint>
#include <tuple>

#include <rex/audio/xma/context.h>

namespace rex::audio {

class XmaContextLegacy : public XmaContext {
 public:
  XmaContextLegacy();
  ~XmaContextLegacy() override;

  int Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) override;
  bool Work() override;

  void Enable() override;
  bool Block(bool poll) override;
  void Clear() override;
  void Disable() override;
  void Release() override;

 private:
  static void SwapInputBuffer(XMA_CONTEXT_DATA* data);
  static bool TrySetupNextLoop(XMA_CONTEXT_DATA* data, bool ignore_input_buffer_offset);
  static void NextPacket(XMA_CONTEXT_DATA* data);
  static int GetSampleRate(int id);
  static size_t GetNextFrame(uint8_t* block, size_t size, size_t bit_offset);
  static int GetFramePacketNumber(uint8_t* block, size_t size, size_t bit_offset);
  static std::tuple<int, int> GetFrameNumber(uint8_t* block, size_t size, size_t bit_offset);
  static std::tuple<int, bool> GetPacketFrameCount(uint8_t* packet);

  bool ValidFrameOffset(uint8_t* block, size_t size_bytes, size_t frame_offset_bits);
  void Decode(XMA_CONTEXT_DATA* data);
  int PrepareDecoder(uint8_t* packet, int sample_rate, bool is_two_channel);

  uint32_t packets_skip_ = 0;
  uint32_t split_frame_len_ = 0;
  uint32_t split_frame_len_partial_ = 0;
  uint8_t split_frame_padding_start_ = 0;
  std::array<uint8_t, 1 + 4096> xma_frame_;
  std::array<uint8_t, kBytesPerFrameChannel * 2> raw_frame_;
};

}  // namespace rex::audio
