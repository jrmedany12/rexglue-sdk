/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <bit>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/xma_context_legacy.h>
#include <rex/audio/xma/xma_context_new.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory/ring_buffer.h>
#include <rex/string/buffer.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/thread_state.h>
#include <rex/system/xthread.h>

extern "C" {
#include "libavutil/log.h"
}  // extern "C"

REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, "Audio", "Verbose FFmpeg output (debug and above)");
REXCVAR_DEFINE_BOOL(use_dedicated_xma_thread, true, "Audio",
                    "Run XMA decoding on a dedicated thread. When false, Work() runs on the MMIO "
                    "thread during kick (Canary-style; can help ordering/priority issues).");
REXCVAR_DEFINE_STRING(
    xma_decoder, "new", "Audio",
    "XMA decoder: \"new\" (default, Canary-style subframe path) or \"old\" (legacy frame loop).");

namespace rex::audio {

XmaDecoder::XmaDecoder(runtime::FunctionDispatcher* function_dispatcher)
    : memory_(function_dispatcher->memory()), function_dispatcher_(function_dispatcher) {}

XmaDecoder::~XmaDecoder() = default;

void av_log_callback(void* avcl, int level, const char* fmt, va_list va) {
  if (!REXCVAR_GET(ffmpeg_verbose) && level > AV_LOG_WARNING) {
    return;
  }

  string::StringBuffer buff;
  buff.AppendVarargs(fmt, va);
  auto msg = buff.to_string_view();

  switch (level) {
    case AV_LOG_ERROR:
      REXAPU_ERROR("ffmpeg: {}", msg);
      break;
    case AV_LOG_WARNING:
      REXAPU_WARN("ffmpeg: {}", msg);
      break;
    case AV_LOG_INFO:
      REXAPU_INFO("ffmpeg: {}", msg);
      break;
    case AV_LOG_VERBOSE:
    case AV_LOG_DEBUG:
    default:
      REXAPU_DEBUG("ffmpeg: {}", msg);
      break;
  }
}

X_STATUS XmaDecoder::Setup(system::KernelState* kernel_state) {
  av_log_set_callback(av_log_callback);

  memory()->AddVirtualMappedRange(
      0x7FEA0000, 0xFFFF0000, 0x0000FFFF, this,
      reinterpret_cast<runtime::MMIOReadCallback>(MMIOReadRegisterThunk),
      reinterpret_cast<runtime::MMIOWriteCallback>(MMIOWriteRegisterThunk));
  REXAPU_DEBUG("XMA: Registered MMIO handlers at 0x7FEA0000-0x7FEAFFFF");

  context_data_first_ptr_ = memory()->SystemHeapAlloc(sizeof(XMA_CONTEXT_DATA) * kContextCount, 256,
                                                      memory::kSystemHeapPhysical);
  context_data_last_ptr_ = context_data_first_ptr_ + (sizeof(XMA_CONTEXT_DATA) * kContextCount - 1);
  register_file_[XmaRegister::ContextArrayAddress] =
      memory()->GetPhysicalAddress(context_data_first_ptr_);

  const bool use_legacy = (REXCVAR_GET(xma_decoder) == "old");
  for (size_t i = 0; i < kContextCount; ++i) {
    if (use_legacy) {
      contexts_[i] = std::make_unique<XmaContextLegacy>();
    } else {
      contexts_[i] = std::make_unique<XmaContextNew>();
    }

    uint32_t guest_ptr = context_data_first_ptr_ + static_cast<uint32_t>(i * sizeof(XMA_CONTEXT_DATA));
    if (contexts_[i]->Setup(static_cast<uint32_t>(i), memory(), guest_ptr)) {
      assert_always();
    }
  }
  register_file_[XmaRegister::NextContextIndex] = 1;
  context_bitmap_.Resize(kContextCount);

  worker_running_ = true;
  work_event_ = rex::thread::Event::CreateAutoResetEvent(false);
  assert_not_null(work_event_);
  worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state, 128 * 1024, 0, [this]() {
        if (REXCVAR_GET(use_dedicated_xma_thread)) {
          WorkerThreadMain();
        } else {
          // Kick path runs Work() on the MMIO thread; keep a minimal loop so Pause()/Resume()
          // can still synchronize (pause_fence_ is signaled from here in WorkerThreadMain).
          while (worker_running_) {
            if (paused_) {
              pause_fence_.Signal();
              resume_fence_.Wait();
            }
            rex::thread::Wait(work_event_.get(), false);
          }
        }
        return 0;
      }));
  worker_thread_->set_name("XMA Decoder");

  worker_thread_->Create();

  return X_STATUS_SUCCESS;
}

void XmaDecoder::WorkerThreadMain() {
  while (worker_running_) {
    bool did_work = false;
    for (uint32_t n = 0; n < kContextCount && worker_running_; n++) {
      XmaContext& context = *contexts_[n];
      bool worked = context.Work();
      if (worked) {
        context.SignalWorkDone();
      }
      did_work = did_work || worked;
    }

    if (paused_) {
      pause_fence_.Signal();
      resume_fence_.Wait();
    }

    if (did_work) {
      continue;
    }
    rex::thread::Wait(work_event_.get(), false);
  }
}

void XmaDecoder::Shutdown() {
  if (!worker_thread_) {
    return;
  }

  worker_running_ = false;

  if (work_event_) {
    work_event_->Set();
  }

  if (paused_) {
    Resume();
  }

  auto result = rex::thread::Wait(worker_thread_->thread(), false, std::chrono::milliseconds(2000));
  if (result == rex::thread::WaitResult::kTimeout) {
    REXAPU_WARN("XMA: Worker thread did not exit within 2s, abandoning");
  }
  worker_thread_.reset();

  for (auto& ctx : contexts_) {
    ctx.reset();
  }

  if (context_data_first_ptr_) {
    memory()->SystemHeapFree(context_data_first_ptr_);
  }

  context_data_first_ptr_ = 0;
  context_data_last_ptr_ = 0;
}

int XmaDecoder::GetContextId(uint32_t guest_ptr) {
  static_assert_size(XMA_CONTEXT_DATA, 64);
  if (guest_ptr < context_data_first_ptr_ || guest_ptr > context_data_last_ptr_) {
    return -1;
  }
  assert_zero(guest_ptr & 0x3F);
  return static_cast<int>((guest_ptr - context_data_first_ptr_) >> 6);
}

uint32_t XmaDecoder::AllocateContext() {
  size_t index = context_bitmap_.Acquire();
  if (index == static_cast<size_t>(-1)) {
    return 0;
  }

  XmaContext& context = *contexts_[index];
  assert_false(context.is_allocated());
  context.set_is_allocated(true);
  return context.guest_ptr();
}

void XmaDecoder::ReleaseContext(uint32_t guest_ptr) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = *contexts_[context_id];
  assert_true(context.is_allocated());
  context.Release();
  context_bitmap_.Release(static_cast<uint32_t>(context_id));
}

bool XmaDecoder::BlockOnContext(uint32_t guest_ptr, bool poll) {
  auto context_id = GetContextId(guest_ptr);
  assert_true(context_id >= 0);

  XmaContext& context = *contexts_[context_id];
  return context.Block(poll);
}

uint32_t XmaDecoder::ReadRegister(uint32_t addr) {
  auto r = (addr & 0xFFFF) / 4;

  assert_true(r < XmaRegisterFile::kRegisterCount);

  switch (r) {
    case XmaRegister::ContextArrayAddress:
      break;
    case XmaRegister::CurrentContextIndex: {
      uint32_t& current_context_index = register_file_[XmaRegister::CurrentContextIndex];
      uint32_t& next_context_index = register_file_[XmaRegister::NextContextIndex];
      current_context_index = next_context_index;
      next_context_index = (next_context_index + 1) % kContextCount;
      break;
    }
    default:
      const auto register_info = register_file_.GetRegisterInfo(r);
      if (register_info) {
        REXAPU_WARN("XMA: Read from unhandled register ({:04X}, {})", r, register_info->name);
      } else {
        REXAPU_WARN("XMA: Read from unknown register ({:04X})", r);
      }
      break;
  }

  return rex::byte_swap(register_file_[r]);
}

void XmaDecoder::WriteRegister(uint32_t addr, uint32_t value) {
  SCOPE_profile_cpu_f("apu");

  uint32_t r = (addr & 0xFFFF) / 4;
  value = rex::byte_swap(value);

  assert_true(r < XmaRegisterFile::kRegisterCount);
  register_file_[r] = value;

  if (r >= XmaRegister::Context0Kick && r <= XmaRegister::Context9Kick) {
    const uint32_t base_context_id = (r - XmaRegister::Context0Kick) * 32;
    const uint32_t kicked_value = value;
    while (value) {
      const uint32_t context_id = base_context_id + std::countr_zero(value);
      auto& context = *contexts_[context_id];
      context.Enable();
      if (!REXCVAR_GET(use_dedicated_xma_thread)) {
        context.Work();
      }
      value &= value - 1;
    }
    work_event_->SetBoostPriority();
    if (REXCVAR_GET(use_dedicated_xma_thread)) {
      uint32_t remaining = kicked_value;
      while (remaining) {
        const uint32_t context_id = base_context_id + std::countr_zero(remaining);
        contexts_[context_id]->WaitForWorkDone();
        remaining &= remaining - 1;
      }
    }
  } else if (r >= XmaRegister::Context0Lock && r <= XmaRegister::Context9Lock) {
    const uint32_t base_context_id = (r - XmaRegister::Context0Lock) * 32;
    while (value) {
      const uint32_t context_id = base_context_id + std::countr_zero(value);
      auto& context = *contexts_[context_id];
      context.Disable();
      context.Block(false);
      value &= value - 1;
    }
  } else if (r >= XmaRegister::Context0Clear && r <= XmaRegister::Context9Clear) {
    const uint32_t base_context_id = (r - XmaRegister::Context0Clear) * 32;
    while (value) {
      const uint32_t context_id = base_context_id + std::countr_zero(value);
      XmaContext& context = *contexts_[context_id];
      context.Clear();
      value &= value - 1;
    }
  } else {
    switch (r) {
      default: {
        const auto register_info = register_file_.GetRegisterInfo(r);
        if (register_info) {
          REXAPU_WARN("XMA: Write to unhandled register ({:04X}, {}): {:08X}", r,
                      register_info->name, value);
        } else {
          REXAPU_WARN("XMA: Write to unknown register ({:04X}): {:08X}", r, value);
        }
        break;
      }
#pragma warning(suppress : 4065)
    }
  }
}

void XmaDecoder::Pause() {
  if (paused_) {
    return;
  }
  paused_ = true;

  if (work_event_) {
    work_event_->Set();
  }
  pause_fence_.Wait();
}

void XmaDecoder::Resume() {
  if (!paused_) {
    return;
  }
  paused_ = false;

  resume_fence_.Signal();
}

}  // namespace rex::audio
