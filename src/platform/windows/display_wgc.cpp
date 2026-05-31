/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Windows Game Capture (WGC) IPC display implementation with shared session helper and DXGI fallback.
 */

// standard includes
#include <algorithm>
#include <atomic>
#include <chrono>
#include <winsock2.h>
#include <dxgi1_2.h>
#include <optional>
#include <wrl/client.h>

// local includes
#include "ipc/ipc_session.h"
#include "ipc/misc_utils.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/windows/display.h"
#include "src/platform/windows/display_vram.h"
#include "src/platform/windows/misc.h"
#include "src/utility.h"

// platform includes
#include <winrt/base.h>

namespace platf::dxgi {
  namespace {
    struct wgc_dxgi_fallback_state_t {
      bool secure_desktop_active;
      bool recent_desktop_switch;
    };

    std::atomic<uint64_t> g_wgc_snapshot_copies {0};
    std::atomic<uint64_t> g_wgc_slow_snapshot_locks {0};
    std::atomic<uint64_t> g_wgc_slow_snapshot_copies {0};

    class adapter_luid_override_guard {
    public:
      explicit adapter_luid_override_guard(const std::optional<LUID> &luid) {
        previous_ = get_dxgi_adapter_luid_override();
        if (luid.has_value()) {
          set_dxgi_adapter_luid_override(luid);
        }
      }

      ~adapter_luid_override_guard() {
        set_dxgi_adapter_luid_override(previous_);
      }

    private:
      std::optional<LUID> previous_;
    };

    std::optional<wgc_dxgi_fallback_state_t> get_wgc_dxgi_fallback_state() {
      wgc_dxgi_fallback_state_t state {
        .secure_desktop_active = platf::dxgi::is_secure_desktop_active(),
        .recent_desktop_switch = recent_wgc_desktop_switch_grace_active()
      };

      if (!state.secure_desktop_active && !state.recent_desktop_switch) {
        return std::nullopt;
      }

      return state;
    }

    void log_wgc_dxgi_fallback_reason(const char *path_name, const wgc_dxgi_fallback_state_t &state) {
      if (state.secure_desktop_active && state.recent_desktop_switch) {
        BOOST_LOG(debug) << "Secure desktop detected and the desktop-switch grace window is active; "
                         << "using DXGI fallback for WGC capture (" << path_name << ")";
      } else if (state.secure_desktop_active) {
        BOOST_LOG(debug) << "Secure desktop detected, using DXGI fallback for WGC capture (" << path_name << ")";
      } else {
        BOOST_LOG(debug) << "Recent desktop switch grace window active, using DXGI fallback for WGC capture ("
                         << path_name << ")";
      }
    }

    std::chrono::milliseconds effective_wgc_timeout(std::chrono::milliseconds timeout) {
      if (timeout.count() == 0) {
        return std::chrono::milliseconds(6);
      }

      return timeout;
    }

    bool forward_cached_wgcc_frame(capture_e status, const std::shared_ptr<platf::img_t> &cached_frame, std::shared_ptr<platf::img_t> &img_out) {
      if (status != capture_e::timeout || config::video.capture != "wgcc" || !cached_frame) {
        return false;
      }

      const auto now = std::chrono::steady_clock::now();
      img_out = cached_frame;
      img_out->frame_timestamp = now;
      img_out->host_processing_timestamp = now;
      img_out->capture_pacing_timestamp = now;
      return true;
    }
  }  // namespace

  display_wgc_ipc_vram_t::display_wgc_ipc_vram_t() = default;

  display_wgc_ipc_vram_t::~display_wgc_ipc_vram_t() {
    if (_frame_locked && _ipc_session) {
      _ipc_session->release();
      _frame_locked = false;
    }
  }

  int display_wgc_ipc_vram_t::init(const ::video::config_t &config, const std::string &display_name) {
    _config = config;
    _display_name = display_name;

    if (display_base_t::init(config, display_name, true /* skip_dd_test: WGC doesn't use Desktop Duplication */)) {
      return -1;
    }

    capture_format = DXGI_FORMAT_UNKNOWN;

    _ipc_session = std::make_unique<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get())) {
      return -1;
    }

    return 0;
  }

  capture_e display_wgc_ipc_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    const auto snapshot_start = std::chrono::steady_clock::now();

    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    timeout = effective_wgc_timeout(timeout);

    const auto wait_for_frame_start = std::chrono::steady_clock::now();
    auto capture_status = _ipc_session->wait_for_frame(timeout);
    const auto wait_for_frame_time = std::chrono::steady_clock::now() - wait_for_frame_start;

    if (capture_status != capture_e::ok) {
      if (forward_cached_wgcc_frame(capture_status, last_cached_frame, img_out)) {
        return capture_e::ok;
      }

      return capture_status;
    }

    D3D11_TEXTURE2D_DESC desc;
    if (!_ipc_session->peek_shared_texture_desc(desc)) {
      return capture_e::reinit;
    }

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed ["sv << width_before_rotation << 'x' << height_before_rotation << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }

    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    std::shared_ptr<platf::img_t> img;

    const auto pull_image_start = std::chrono::steady_clock::now();
    if (!pull_free_image_cb(img)) {
      return capture_e::interrupted;
    }
    const auto pull_image_time = std::chrono::steady_clock::now() - pull_image_start;

    auto d3d_img = std::static_pointer_cast<img_d3d_t>(img);
    if (complete_img(d3d_img.get(), false)) {
      return capture_e::error;
    }

    const auto capture_mutex_wait_start = std::chrono::steady_clock::now();
    HRESULT status = d3d_img->capture_mutex->AcquireSync(0, 3000);
    const auto capture_mutex_wait = std::chrono::steady_clock::now() - capture_mutex_wait_start;
    if (status == WAIT_ABANDONED) {
      BOOST_LOG(error) << "Capture texture keyed mutex was abandoned; continuing with lock held";
    } else if (status != S_OK) {
      BOOST_LOG(error) << "Failed to lock capture texture [0x"sv << util::hex(status).to_string_view() << ']';
      return capture_e::error;
    }

    auto release_capture_mutex = util::fail_guard([&]() {
      const HRESULT release_status = d3d_img->capture_mutex->ReleaseSync(0);
      if (FAILED(release_status)) {
        BOOST_LOG(warning) << "Failed to release capture texture mutex [0x"sv << util::hex(release_status).to_string_view() << ']';
      }
    });

    texture2d_t src;
    uint64_t frame_qpc = 0;
    winrt::com_ptr<ID3D11Texture2D> gpu_tex;

    const auto ipc_lock_start = std::chrono::steady_clock::now();
    capture_status = _ipc_session->lock_frame(gpu_tex, frame_qpc);
    const auto ipc_lock_time = std::chrono::steady_clock::now() - ipc_lock_start;

    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    _frame_locked = true;
    auto release_ipc_frame = util::fail_guard([&]() {
      if (_ipc_session && _frame_locked) {
        _ipc_session->release();
        _frame_locked = false;
      }
    });

    gpu_tex.copy_to(&src);

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame_qpc);

    const auto copy_start = std::chrono::steady_clock::now();
    device_ctx->CopyResource(d3d_img->capture_texture.get(), src.get());
    const auto copy_submit = std::chrono::steady_clock::now() - copy_start;

    d3d_img->blank = false;

    const auto ipc_release_start = std::chrono::steady_clock::now();
    _ipc_session->release();
    const auto ipc_release_time = std::chrono::steady_clock::now() - ipc_release_start;

    _frame_locked = false;
    release_ipc_frame.disable();

    const auto capture_release_start = std::chrono::steady_clock::now();
    const HRESULT release_status = d3d_img->capture_mutex->ReleaseSync(0);
    const auto capture_release_time = std::chrono::steady_clock::now() - capture_release_start;

    release_capture_mutex.disable();

    if (FAILED(release_status)) {
      BOOST_LOG(warning) << "Failed to release capture texture mutex [0x"
                         << util::hex(release_status).to_string_view() << ']';
    }

    const auto copy_count = g_wgc_snapshot_copies.fetch_add(1, std::memory_order_relaxed) + 1;

    const auto capture_mutex_wait_ms = std::chrono::duration<double, std::milli>(capture_mutex_wait).count();
    const auto copy_submit_ms = std::chrono::duration<double, std::milli>(copy_submit).count();
    const auto wait_for_frame_ms = std::chrono::duration<double, std::milli>(wait_for_frame_time).count();
    const auto pull_image_ms = std::chrono::duration<double, std::milli>(pull_image_time).count();
    const auto ipc_lock_ms = std::chrono::duration<double, std::milli>(ipc_lock_time).count();
    const auto ipc_release_ms = std::chrono::duration<double, std::milli>(ipc_release_time).count();
    const auto capture_release_ms = std::chrono::duration<double, std::milli>(capture_release_time).count();
    const auto snapshot_total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - snapshot_start).count();

    const bool slow_lock = capture_mutex_wait_ms > 1.0;
    const bool slow_copy = copy_submit_ms > 1.0;
    const bool slow_snapshot =
      wait_for_frame_ms > 8.0 ||
      pull_image_ms > 1.0 ||
      ipc_lock_ms > 1.0 ||
      ipc_release_ms > 1.0 ||
      capture_release_ms > 1.0 ||
      snapshot_total_ms > 8.0;

    if (slow_lock) {
      g_wgc_slow_snapshot_locks.fetch_add(1, std::memory_order_relaxed);
    }
    if (slow_copy) {
      g_wgc_slow_snapshot_copies.fetch_add(1, std::memory_order_relaxed);
    }
    if (copy_count == 1 || copy_count % 600 == 0 || slow_lock || slow_copy || slow_snapshot) {
      BOOST_LOG(debug) << "WGC snapshot timing: frame=" << copy_count
                       << " wait_for_frame_ms=" << wait_for_frame_ms
                       << " pull_image_ms=" << pull_image_ms
                       << " capture_mutex_wait_ms=" << capture_mutex_wait_ms
                       << " ipc_lock_ms=" << ipc_lock_ms
                       << " copy_submit_ms=" << copy_submit_ms
                       << " ipc_release_ms=" << ipc_release_ms
                       << " capture_release_ms=" << capture_release_ms
                       << " total_ms=" << snapshot_total_ms
                       << " slow_locks=" << g_wgc_slow_snapshot_locks.load(std::memory_order_relaxed)
                       << " slow_copies=" << g_wgc_slow_snapshot_copies.load(std::memory_order_relaxed);
    }

    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    img->capture_pacing_timestamp = host_processing_timestamp;
    img_out = img;
    last_cached_frame = img;

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_vram_t::acquire_next_frame(std::chrono::milliseconds timeout, texture2d_t &src, uint64_t &frame_qpc, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    if (_frame_locked) {
      BOOST_LOG(warning)
        << "WGC IPC frame was still locked at the start of acquire_next_frame(); "
        << "releasing stale frame lock and continuing.";

      _ipc_session->release();
      _frame_locked = false;
    }

    winrt::com_ptr<ID3D11Texture2D> gpu_tex;

    auto status = _ipc_session->acquire(effective_wgc_timeout(timeout), gpu_tex, frame_qpc);

    if (status != capture_e::ok) {
      return status;
    }

    _frame_locked = true;
    auto release_ipc_frame = util::fail_guard([&]() {
      if (_ipc_session && _frame_locked) {
        _ipc_session->release();
        _frame_locked = false;
      }
    });

    gpu_tex.copy_to(&src);
    release_ipc_frame.disable();

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_vram_t::release_snapshot() {
    if (_ipc_session && _frame_locked) {
      _ipc_session->release();
      _frame_locked = false;
    }
    return capture_e::ok;
  }

  int display_wgc_ipc_vram_t::dummy_img(platf::img_t *img_base) {
    return complete_img(img_base, true);
  }

  std::shared_ptr<display_t> display_wgc_ipc_vram_t::create(const ::video::config_t &config, const std::string &display_name) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("VRAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_vram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    } else {
      BOOST_LOG(debug) << "Using WGC IPC implementation (VRAM)";
      auto disp = std::make_shared<display_wgc_ipc_vram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    }

    return nullptr;
  }

  display_wgc_ipc_ram_t::display_wgc_ipc_ram_t() = default;

  display_wgc_ipc_ram_t::~display_wgc_ipc_ram_t() = default;

  int display_wgc_ipc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    _config = config;
    _display_name = display_name;

    if (display_base_t::init(config, display_name, true /* skip_dd_test: WGC doesn't use Desktop Duplication */)) {
      return -1;
    }

    capture_format = DXGI_FORMAT_UNKNOWN;

    _ipc_session = std::make_unique<ipc_session_t>();
    if (_ipc_session->init(config, display_name, device.get())) {
      return -1;
    }

    return 0;
  }

  capture_e display_wgc_ipc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (!_ipc_session) {
      return capture_e::error;
    }

    if (_ipc_session->should_swap_to_dxgi()) {
      return capture_e::reinit;
    }

    if (_ipc_session->should_reinit()) {
      return capture_e::reinit;
    }

    _ipc_session->initialize_if_needed();
    if (!_ipc_session->is_initialized()) {
      BOOST_LOG(warning) << "WGC IPC helper failed to initialize; requesting capture reinit.";
      return capture_e::reinit;
    }

    winrt::com_ptr<ID3D11Texture2D> gpu_tex;
    uint64_t frame_qpc = 0;
    timeout = effective_wgc_timeout(timeout);
    auto status = _ipc_session->acquire(timeout, gpu_tex, frame_qpc);

    if (status != capture_e::ok) {
      if (forward_cached_wgcc_frame(status, last_cached_frame, img_out)) {
        return capture_e::ok;
      }

      return status;
    }

    D3D11_TEXTURE2D_DESC desc;
    gpu_tex->GetDesc(&desc);

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format [" << dxgi_format_to_string(capture_format) << ']';
    }

    if (desc.Width != width_before_rotation || desc.Height != height_before_rotation) {
      BOOST_LOG(info) << "Capture size changed [" << width_before_rotation << 'x' << height_before_rotation << " -> " << desc.Width << 'x' << desc.Height << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed [" << dxgi_format_to_string(capture_format) << " -> " << dxgi_format_to_string(desc.Format) << ']';
      _ipc_session->release();
      return capture_e::reinit;
    }

    if (!texture ||
        width_before_rotation != _last_width ||
        height_before_rotation != _last_height ||
        capture_format != _last_format) {
      D3D11_TEXTURE2D_DESC t {};
      t.Width = width_before_rotation;
      t.Height = height_before_rotation;
      t.Format = capture_format;
      t.ArraySize = 1;
      t.MipLevels = 1;
      t.SampleDesc = {1, 0};
      t.Usage = D3D11_USAGE_STAGING;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto hr = device->CreateTexture2D(&t, nullptr, &texture);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to create staging texture: " << hr;
        _ipc_session->release();
        return capture_e::error;
      }

      _last_width = width_before_rotation;
      _last_height = height_before_rotation;
      _last_format = capture_format;

      BOOST_LOG(info) << "[display_wgc_ipc_ram_t] Created staging texture: "
                      << width_before_rotation << "x" << height_before_rotation << ", format: " << capture_format;
    }

    device_ctx->CopyResource(texture.get(), gpu_tex.get());

    _ipc_session->release();

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }

    auto img = img_out.get();

    if (capture_format == DXGI_FORMAT_UNKNOWN) {
      if (dummy_img(img)) {
        return capture_e::error;
      }
    } else {
      auto hr = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info);
      if (FAILED(hr)) {
        BOOST_LOG(error) << "[display_wgc_ipc_ram_t] Failed to map staging texture: " << hr;
        return capture_e::error;
      }

      if (complete_img(img, false)) {
        device_ctx->Unmap(texture.get(), 0);
        img_info.pData = nullptr;
        return capture_e::error;
      }

      std::copy_n((std::uint8_t *) img_info.pData, height_before_rotation * img_info.RowPitch, img->data);

      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
    }

    const auto host_processing_timestamp = std::chrono::steady_clock::now();
    auto frame_timestamp = host_processing_timestamp - qpc_time_difference(qpc_counter(), frame_qpc);
    img->frame_timestamp = frame_timestamp;
    img->host_processing_timestamp = host_processing_timestamp;
    img->capture_pacing_timestamp = host_processing_timestamp;
    last_cached_frame = img_out;

    return capture_e::ok;
  }

  capture_e display_wgc_ipc_ram_t::release_snapshot() {
    return capture_e::ok;
  }

  int display_wgc_ipc_ram_t::dummy_img(platf::img_t *img_base) {
    return display_ram_t::dummy_img(img_base);
  }

  std::shared_ptr<display_t> display_wgc_ipc_ram_t::create(const ::video::config_t &config, const std::string &display_name) {
    if (auto fallback_state = get_wgc_dxgi_fallback_state()) {
      log_wgc_dxgi_fallback_reason("RAM", *fallback_state);
      adapter_luid_override_guard guard(get_last_wgc_adapter_luid());
      auto disp = std::make_shared<temp_dxgi_ram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    } else {
      BOOST_LOG(debug) << "Using WGC IPC implementation (RAM)";
      auto disp = std::make_shared<display_wgc_ipc_ram_t>();
      if (!disp->init(config, display_name)) {
        return disp;
      }
    }

    return nullptr;
  }

  capture_e temp_dxgi_vram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    return display_ddup_vram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

  capture_e temp_dxgi_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    if (auto now = std::chrono::steady_clock::now(); now - _last_check_time >= CHECK_INTERVAL) {
      _last_check_time = now;
      const bool secure_desktop_active = platf::dxgi::is_secure_desktop_active();
      if (!secure_desktop_active && !recent_wgc_desktop_switch_grace_active()) {
        BOOST_LOG(debug) << "DXGI Capture is no longer necessary, swapping back to WGC!";
        return capture_e::reinit;
      }
    }

    return display_ddup_ram_t::snapshot(pull_free_image_cb, img_out, timeout, cursor_visible);
  }

}  // namespace platf::dxgi
