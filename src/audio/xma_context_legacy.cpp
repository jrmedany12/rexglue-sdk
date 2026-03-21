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

#include <algorithm>
#include <cstring>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/helpers.h>
#include <rex/audio/xma/xma_context_legacy.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

namespace rex::audio {

using stream::BitStream;

XmaContextLegacy::XmaContextLegacy() = default;

XmaContextLegacy::~XmaContextLegacy() {
  if (av_context_) {
    if (avcodec_is_open(av_context_)) {
      avcodec_close(av_context_);
    }
    av_free(av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContextLegacy::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);

  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  return 0;
}

bool XmaContextLegacy::Work() {
  std::lock_guard<std::mutex> lock(lock_);
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  Decode(&data);
  data.Store(context_ptr);
  return true;
}

void XmaContextLegacy::Enable() {
  std::lock_guard<std::mutex> lock(lock_);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  REXAPU_TRACE("XmaContext: kicking context {} (buffer {} {}/{} bits)", id(),
               static_cast<uint32_t>(data.current_buffer),
               static_cast<uint32_t>(data.input_buffer_read_offset),
               (data.current_buffer == 0 ? data.input_buffer_0_packet_count
                                         : data.input_buffer_1_packet_count) *
                   kBitsPerPacket);

  data.Store(context_ptr);

  set_is_enabled(true);
}

bool XmaContextLegacy::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContextLegacy::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);

  data.input_buffer_0_valid = 0;
  data.input_buffer_1_valid = 0;
  data.output_buffer_valid = 0;

  data.output_buffer_read_offset = 0;
  data.output_buffer_write_offset = 0;

  data.Store(context_ptr);
}

void XmaContextLegacy::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_TRACE("XmaContext: disabling context {}", id());
  set_is_enabled(false);
}

void XmaContextLegacy::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContextLegacy::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = 0;
}

bool XmaContextLegacy::TrySetupNextLoop(XMA_CONTEXT_DATA* data, bool ignore_input_buffer_offset) {
  if (data->loop_count > 0 && data->loop_start < data->loop_end &&
      (ignore_input_buffer_offset || data->input_buffer_read_offset >= data->loop_end)) {
    data->input_buffer_read_offset = data->loop_start;
    if (data->loop_count < 255) {
      data->loop_count--;
    }
    return true;
  }
  return false;
}

void XmaContextLegacy::NextPacket(XMA_CONTEXT_DATA* /*data*/) {}

int XmaContextLegacy::GetSampleRate(int id) {
  switch (id) {
    case 0:
      return 24000;
    case 1:
      return 32000;
    case 2:
      return 44100;
    case 3:
      return 48000;
  }
  assert_always();
  return 0;
}

bool XmaContextLegacy::ValidFrameOffset(uint8_t* block, size_t size_bytes, size_t frame_offset_bits) {
  int packet_num = GetFramePacketNumber(block, size_bytes, frame_offset_bits);
  if (packet_num < 0) {
    return false;
  }

  uint8_t* packet = block + (static_cast<size_t>(packet_num) * kBytesPerPacket);
  size_t relative_offset_bits = frame_offset_bits % kBitsPerPacket;

  uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset == static_cast<uint32_t>(-1) || first_frame_offset > kBitsPerPacket) {
    return false;
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  while (true) {
    if (stream.offset_bits() == relative_offset_bits) {
      return true;
    }

    if (stream.BitsRemaining() < 15) {
      return false;
    }

    uint64_t size = stream.Read(15);
    if ((size - 15) > stream.BitsRemaining()) {
      return false;
    } else if (size == 0x7FFF) {
      return false;
    }

    stream.Advance(size - 16);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  return false;
}

void XmaContextLegacy::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->output_buffer_valid) {
    return;
  }

  if (!data->input_buffer_0_valid && !data->input_buffer_1_valid) {
    data->output_buffer_valid = 0;
    return;
  }

  uint8_t* in0 =
      data->input_buffer_0_valid ? memory()->TranslatePhysical(data->input_buffer_0_ptr) : nullptr;
  uint8_t* in1 =
      data->input_buffer_1_valid ? memory()->TranslatePhysical(data->input_buffer_1_ptr) : nullptr;
  uint8_t* current_input_buffer = data->current_buffer ? in1 : in0;

  REXAPU_TRACE("Processing context {} (offset {}, buffer {}, ptr {:p})", id(),
               static_cast<uint32_t>(data->input_buffer_read_offset),
               static_cast<uint32_t>(data->current_buffer),
               static_cast<void*>(current_input_buffer));

  size_t input_buffer_0_size = data->input_buffer_0_packet_count * kBytesPerPacket;
  size_t input_buffer_1_size = data->input_buffer_1_packet_count * kBytesPerPacket;
  size_t current_input_size = data->current_buffer ? input_buffer_1_size : input_buffer_0_size;
  size_t current_input_packet_count = current_input_size / kBytesPerPacket;

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);
  uint32_t output_capacity = data->output_buffer_block_count * kBytesPerSubframeChannel;
  uint32_t output_read_offset = data->output_buffer_read_offset * kBytesPerSubframeChannel;
  uint32_t output_write_offset = data->output_buffer_write_offset * kBytesPerSubframeChannel;

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);

  size_t output_remaining_bytes = output_rb.write_count();
  output_remaining_bytes -= output_remaining_bytes % (kBytesPerFrameChannel << data->is_stereo);

  assert_false(data->stop_when_done);
  assert_false(data->interrupt_when_done);
  bool reuse_input_buffer = false;

  while (output_remaining_bytes > 0) {
    if (!data->input_buffer_0_valid && !data->input_buffer_1_valid) {
      break;
    }

    reuse_input_buffer = TrySetupNextLoop(data, false);

    int packet_idx = 0;
    int frame_idx = 0;
    int frame_count = 0;
    uint8_t* packet = nullptr;
    bool frame_last_split = false;

    BitStream stream(current_input_buffer, current_input_size * 8);
    stream.SetOffset(data->input_buffer_read_offset);

    if (packets_skip_ > 0) {
      packet_idx = GetFramePacketNumber(current_input_buffer, current_input_size,
                                        data->input_buffer_read_offset);
      while (packets_skip_ > 0) {
        packets_skip_--;
        packet_idx++;
        if (packet_idx >= static_cast<int>(current_input_packet_count)) {
          if (!reuse_input_buffer) {
            reuse_input_buffer = TrySetupNextLoop(data, true);
          }
          if (!reuse_input_buffer) {
            SwapInputBuffer(data);
          }
          return;
        }
      }
      data->input_buffer_read_offset = packet_idx * static_cast<int>(kBitsPerPacket);
    }

    if (split_frame_len_) {
      packet_idx = GetFramePacketNumber(current_input_buffer, current_input_size,
                                        data->input_buffer_read_offset);
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      std::tie(frame_count, frame_last_split) = GetPacketFrameCount(packet);
      frame_idx = -1;

      stream = BitStream(current_input_buffer, (packet_idx + 1) * kBitsPerPacket);
      stream.SetOffset(packet_idx * static_cast<int>(kBitsPerPacket) + 32);

      if (split_frame_len_ > static_cast<int>(xma::kMaxFrameLength)) {
        auto offset = stream.offset_bits();
        stream.Copy(
            xma_frame_.data() + 1 + ((split_frame_len_partial_ + split_frame_padding_start_) / 8),
            15 - split_frame_len_partial_);
        stream.SetOffset(offset);
        BitStream slen(xma_frame_.data() + 1, 15 + split_frame_padding_start_);
        slen.Advance(split_frame_padding_start_);
        split_frame_len_ = static_cast<int>(slen.Read(15));
      }

      if (frame_count > 0) {
        assert_true(xma::GetPacketFrameOffset(packet) - 32 ==
                    static_cast<uint32_t>(split_frame_len_ - split_frame_len_partial_));
      }

      auto offset = stream.Copy(
          xma_frame_.data() + 1 + ((split_frame_len_partial_ + split_frame_padding_start_) / 8),
          split_frame_len_ - split_frame_len_partial_);
      assert_true(offset == (split_frame_padding_start_ + split_frame_len_partial_) % 8);
    } else {
      if (data->input_buffer_read_offset % kBitsPerPacket == 0) {
        int packet_number = GetFramePacketNumber(current_input_buffer, current_input_size,
                                                 data->input_buffer_read_offset);

        if (packet_number == -1) {
          return;
        }

        auto offset =
            xma::GetPacketFrameOffset(current_input_buffer + kBytesPerPacket * packet_number) +
            data->input_buffer_read_offset;
        if (offset == static_cast<uint32_t>(-1)) {
          SwapInputBuffer(data);
          REXAPU_ERROR("XmaContext {}: TODO partial frames? end?", id());
          assert_always("TODO");
          return;
        } else {
          data->input_buffer_read_offset = offset;
        }
      }

      if (!ValidFrameOffset(current_input_buffer, current_input_size, data->input_buffer_read_offset)) {
        REXAPU_DEBUG("XmaContext {}: Invalid read offset {}!", id(),
                     static_cast<uint32_t>(data->input_buffer_read_offset));
        SwapInputBuffer(data);
        return;
      }

      std::tie(packet_idx, frame_idx) =
          GetFrameNumber(current_input_buffer, current_input_size, data->input_buffer_read_offset);
      assert_true(packet_idx >= 0);
      assert_true(frame_idx >= 0);
      packet = current_input_buffer + packet_idx * kBytesPerPacket;
      std::tie(frame_count, frame_last_split) = GetPacketFrameCount(packet);
      assert_true(frame_count >= 0);

      PrepareDecoder(packet, data->sample_rate, bool(data->is_stereo));

      bool frame_is_split = frame_last_split && (frame_idx >= frame_count - 1);

      stream = BitStream(current_input_buffer, (packet_idx + 1) * kBitsPerPacket);
      stream.SetOffset(data->input_buffer_read_offset);
      split_frame_len_partial_ = static_cast<int>(stream.BitsRemaining());
      if (split_frame_len_partial_ >= 15) {
        split_frame_len_ = static_cast<int>(stream.Peek(15));
      } else {
        split_frame_len_ = static_cast<int>(xma::kMaxFrameLength) + 1;
      }
      assert_true(frame_is_split == (split_frame_len_ > split_frame_len_partial_));

      std::memset(xma_frame_.data(), 0, xma_frame_.size());

      {
        auto offset = stream.Copy(xma_frame_.data() + 1,
                                  std::min(split_frame_len_, split_frame_len_partial_));
        assert_true(offset < 8);
        split_frame_padding_start_ = static_cast<uint8_t>(offset);
      }

      if (frame_is_split) {
        packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
        while (packets_skip_ > 0) {
          packets_skip_--;
          packet += kBytesPerPacket;
          packet_idx++;
          if (packet_idx >= static_cast<int>(current_input_packet_count)) {
            if (!reuse_input_buffer) {
              reuse_input_buffer = TrySetupNextLoop(data, true);
            }
            if (!reuse_input_buffer) {
              SwapInputBuffer(data);
            }
            return;
          }
        }
        data->input_buffer_read_offset = packet_idx * static_cast<uint32_t>(kBitsPerPacket);
        continue;
      }
    }

    av_packet_->data = xma_frame_.data();
    av_packet_->size =
        static_cast<int>(1 + ((split_frame_padding_start_ + split_frame_len_) / 8) +
                         (((split_frame_padding_start_ + split_frame_len_) % 8) ? 1 : 0));

    auto padding_end = av_packet_->size * 8 - (8 + split_frame_padding_start_ + split_frame_len_);
    assert_true(padding_end < 8);
    xma_frame_[0] = ((split_frame_padding_start_ & 7) << 5) | ((padding_end & 7) << 2);

    split_frame_len_ = 0;
    split_frame_len_partial_ = 0;
    split_frame_padding_start_ = 0;

    auto ret = avcodec_send_packet(av_context_, av_packet_);
    if (ret < 0) {
      REXAPU_ERROR("XmaContext {}: Error sending packet for decoding", id());
      assert_always();
    }
    ret = avcodec_receive_frame(av_context_, av_frame_);
    if (ret < 0) {
      REXAPU_ERROR("XmaContext {}: Error during decoding", id());
      assert_always();
      return;
    }
    assert_true(ret == 0);

    {
      assert_true(av_context_->sample_fmt == AV_SAMPLE_FMT_FLTP);
      ConvertFrame((const uint8_t**)av_frame_->data, bool(data->is_stereo), raw_frame_.data());

      auto byte_count = kBytesPerFrameChannel << data->is_stereo;
      assert_true(output_remaining_bytes >= byte_count);
      output_rb.Write(raw_frame_.data(), byte_count);
      output_remaining_bytes -= byte_count;
      data->output_buffer_write_offset = output_rb.write_offset() / 256;

      uint32_t offset = data->input_buffer_read_offset;
      offset =
          static_cast<uint32_t>(GetNextFrame(current_input_buffer, current_input_size, offset));
      if (frame_idx + 1 >= frame_count) {
        packets_skip_ = xma::GetPacketSkipCount(packet) + 1;
        while (packets_skip_ > 0) {
          packets_skip_--;
          packet_idx++;
          if (packet_idx >= static_cast<int>(current_input_packet_count)) {
            if (!reuse_input_buffer) {
              reuse_input_buffer = TrySetupNextLoop(data, true);
            }
            if (!reuse_input_buffer) {
              SwapInputBuffer(data);
            }
            return;
          }
        }
        packet = current_input_buffer + packet_idx * kBytesPerPacket;
        offset = xma::GetPacketFrameOffset(packet) + packet_idx * static_cast<uint32_t>(kBitsPerPacket);
      }
      if (offset == 0 || frame_idx == -1) {
        if (packet_idx >= static_cast<int>(current_input_packet_count)) {
          if (!reuse_input_buffer) {
            reuse_input_buffer = TrySetupNextLoop(data, true);
          }
          if (!reuse_input_buffer) {
            SwapInputBuffer(data);
          }
          break;
        }
        offset = xma::GetPacketFrameOffset(packet) + packet_idx * static_cast<uint32_t>(kBitsPerPacket);
      }
      assert_true(data->input_buffer_read_offset < offset);
      data->input_buffer_read_offset = offset;
    }
  }

  if (output_rb.write_offset() == output_rb.read_offset()) {
    data->output_buffer_valid = 0;
  }
}

size_t XmaContextLegacy::GetNextFrame(uint8_t* block, size_t size, size_t bit_offset) {
  auto packet_idx = GetFramePacketNumber(block, size, bit_offset);

  BitStream stream(block, size * 8);
  stream.SetOffset(bit_offset);

  if (stream.BitsRemaining() < 15) {
    return 0;
  }

  uint64_t len = stream.Read(15);
  if ((len - 15) > stream.BitsRemaining()) {
    return 0;
  } else if (len >= xma::kMaxFrameLength) {
    return 0;
  }

  stream.Advance(len - (15 + 1));
  if (stream.Read(1) == 0) {
    return 0;
  }

  bit_offset += len;
  if (packet_idx < GetFramePacketNumber(block, size, bit_offset)) {
    return 0;
  }
  return bit_offset;
}

int XmaContextLegacy::GetFramePacketNumber(uint8_t* block, size_t size, size_t bit_offset) {
  size *= 8;
  if (bit_offset >= size) {
    assert_always();
    return -1;
  }

  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;

  return static_cast<int>(packet_number);
}

std::tuple<int, int> XmaContextLegacy::GetFrameNumber(uint8_t* block, size_t size,
                                                      size_t bit_offset) {
  auto packet_idx = GetFramePacketNumber(block, size, bit_offset);

  if (packet_idx < 0 || (packet_idx + 1) * static_cast<int>(kBytesPerPacket) > static_cast<int>(size)) {
    assert_always();
    return {packet_idx, -2};
  }

  if (bit_offset == 0) {
    return {packet_idx, -1};
  }

  uint8_t* pkt = block + (packet_idx * kBytesPerPacket);
  auto first_frame_offset = xma::GetPacketFrameOffset(pkt);
  BitStream bs(block, size * 8);
  bs.SetOffset(packet_idx * kBitsPerPacket + first_frame_offset);

  int fidx = 0;
  while (true) {
    if (bs.BitsRemaining() < 15) {
      break;
    }

    if (bs.offset_bits() == bit_offset) {
      break;
    }

    uint64_t fsz = bs.Read(15);
    if ((fsz - 15) > bs.BitsRemaining()) {
      break;
    } else if (fsz == 0x7FFF) {
      break;
    }

    bs.Advance(fsz - (15 + 1));

    if (bs.Read(1) == 0) {
      break;
    }
    fidx++;
  }
  return {packet_idx, fidx};
}

std::tuple<int, bool> XmaContextLegacy::GetPacketFrameCount(uint8_t* packet) {
  auto first_frame_offset = xma::GetPacketFrameOffset(packet);
  if (first_frame_offset > kBitsPerPacket - kBitsPerPacketHeader) {
    return {0, false};
  }

  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);
  int frame_count = 0;

  while (true) {
    frame_count++;
    if (stream.BitsRemaining() < 15) {
      return {frame_count, true};
    }

    uint64_t size = stream.Read(15);
    if ((size - 15) > stream.BitsRemaining()) {
      return {frame_count, true};
    } else if (size == 0x7FFF) {
      assert_always();
      return {frame_count, true};
    }

    stream.Advance(size - (15 + 1));

    if (stream.Read(1) == 0) {
      return {frame_count, false};
    }
  }
}

int XmaContextLegacy::PrepareDecoder(uint8_t* packet, int sample_rate, bool is_two_channel) {
  assert_true((packet[2] & 0x7) == 1 || (packet[2] & 0x7) == 0);

  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2u : 1u;
  if (av_context_->sample_rate != sample_rate ||
      static_cast<uint32_t>(av_context_->channels) != channels) {
    avcodec_close(av_context_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = static_cast<int>(channels);

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

}  // namespace rex::audio
