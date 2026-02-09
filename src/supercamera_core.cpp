#include "supercamera_core.hpp"

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#if __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#elif __has_include(<libusb.h>)
#include <libusb.h>
#else
#error "libusb header not found"
#endif

namespace supercamera {
namespace {

using vid_pid_t = std::pair<uint16_t, uint16_t>;

class UsbSupercamera {
    static constexpr vid_pid_t USB_VENDOR_PRODUCT_ID_LIST[] = {
        {0x2ce3, 0x3828},
        {0x0329, 0x2022},
    };
    static constexpr int INTERFACE_A_NUMBER = 0;
    static constexpr int INTERFACE_B_NUMBER = 1;
    static constexpr int INTERFACE_B_ALTERNATE_SETTING = 1;
    static constexpr unsigned char ENDPOINT_1 = 1;
    static constexpr unsigned char ENDPOINT_2 = 2;
    static constexpr unsigned int USB_TIMEOUT = 1000;

    libusb_context *ctx_ = nullptr;
    libusb_device_handle *handle_ = nullptr;

    int usb_read(unsigned char endpoint, ByteVector &buf, size_t max_size) {
        int transferred = 0;
        buf.resize(max_size);
        int ret = libusb_bulk_transfer(
            handle_, LIBUSB_ENDPOINT_IN | endpoint, buf.data(), static_cast<int>(buf.size()), &transferred,
            USB_TIMEOUT);
        if (ret != 0) {
            buf.clear();
            return ret;
        }
        buf.resize(static_cast<size_t>(transferred));
        return 0;
    }

    int usb_write(unsigned char endpoint, const ByteVector &buf) {
        int transferred = 0;
        int ret = libusb_bulk_transfer(
            handle_, LIBUSB_ENDPOINT_OUT | endpoint, const_cast<uint8_t *>(buf.data()),
            static_cast<int>(buf.size()), &transferred, USB_TIMEOUT);
        if (ret != 0) {
            return ret;
        }
        if (transferred != static_cast<int>(buf.size())) {
            return LIBUSB_ERROR_IO;
        }
        return 0;
    }

    static bool is_supported_device(const libusb_device_descriptor &desc, std::span<const vid_pid_t> vid_pid_list) {
        for (const auto &vid_pid : vid_pid_list) {
            if (desc.idVendor == vid_pid.first && desc.idProduct == vid_pid.second) {
                return true;
            }
        }
        return false;
    }

    static size_t count_supported_devices(libusb_context *ctx, std::span<const vid_pid_t> vid_pid_list) {
        struct libusb_device **devs = nullptr;
        if (libusb_get_device_list(ctx, &devs) < 0) {
            return 0;
        }

        size_t count = 0;
        size_t i = 0;
        struct libusb_device *dev = nullptr;
        while ((dev = devs[i++]) != nullptr) {
            struct libusb_device_descriptor desc;
            int ret = libusb_get_device_descriptor(dev, &desc);
            if (ret < 0) {
                continue;
            }
            if (is_supported_device(desc, vid_pid_list)) {
                ++count;
            }
        }

        libusb_free_device_list(devs, 1);
        return count;
    }

    static libusb_device_handle *open_device_with_vid_pid_list(
        libusb_context *ctx,
        std::span<const vid_pid_t> vid_pid_list,
        uint16_t device_index) {
        struct libusb_device **devs = nullptr;
        if (libusb_get_device_list(ctx, &devs) < 0) {
            return nullptr;
        }

        libusb_device_handle *dev_handle = nullptr;
        uint16_t current_index = 0;

        size_t i = 0;
        struct libusb_device *dev = nullptr;
        while ((dev = devs[i++]) != nullptr) {
            struct libusb_device_descriptor desc;
            int ret = libusb_get_device_descriptor(dev, &desc);
            if (ret < 0 || !is_supported_device(desc, vid_pid_list)) {
                continue;
            }
            if (current_index == device_index) {
                ret = libusb_open(dev, &dev_handle);
                if (ret < 0) {
                    dev_handle = nullptr;
                }
                break;
            }
            ++current_index;
        }

        libusb_free_device_list(devs, 1);
        return dev_handle;
    }

public:
    explicit UsbSupercamera(uint16_t device_index) {
        try {
            int ret = libusb_init(&ctx_);
            if (ret < 0) {
                throw std::runtime_error("fatal: libusb_init failed");
            }

            const size_t count = count_supported_devices(ctx_, std::span(USB_VENDOR_PRODUCT_ID_LIST));
            handle_ = open_device_with_vid_pid_list(ctx_, std::span(USB_VENDOR_PRODUCT_ID_LIST), device_index);
            if (handle_ == nullptr) {
                std::ostringstream ss;
                ss << "fatal: usb device index " << device_index
                   << " not found (available: " << count << ")";
                throw std::runtime_error(ss.str());
            }

            ret = libusb_claim_interface(handle_, INTERFACE_A_NUMBER);
            if (ret < 0) {
                throw std::runtime_error("fatal: usb_claim_interface A failed");
            }

            ret = libusb_claim_interface(handle_, INTERFACE_B_NUMBER);
            if (ret < 0) {
                throw std::runtime_error("fatal: usb_claim_interface B failed");
            }

            ret = libusb_set_interface_alt_setting(handle_, INTERFACE_B_NUMBER, INTERFACE_B_ALTERNATE_SETTING);
            if (ret < 0) {
                throw std::runtime_error("fatal: libusb_set_interface_alt_setting failed");
            }

            ret = libusb_clear_halt(handle_, ENDPOINT_1);
            if (ret < 0) {
                throw std::runtime_error("fatal: libusb_clear_halt EP1 failed");
            }

            const ByteVector ep2_buf = {0xFF, 0x55, 0xFF, 0x55, 0xEE, 0x10};
            ret = usb_write(ENDPOINT_2, ep2_buf);
            if (ret != 0) {
                throw std::runtime_error("fatal: start sequence EP2 failed");
            }

            const ByteVector start_stream = {0xBB, 0xAA, 5, 0, 0};
            ret = usb_write(ENDPOINT_1, start_stream);
            if (ret != 0) {
                throw std::runtime_error("fatal: start stream command failed");
            }
        } catch (...) {
            if (handle_ != nullptr) {
                libusb_close(handle_);
                handle_ = nullptr;
            }
            if (ctx_ != nullptr) {
                libusb_exit(ctx_);
                ctx_ = nullptr;
            }
            throw;
        }
    }

    ~UsbSupercamera() {
        if (handle_ != nullptr) {
            libusb_close(handle_);
            handle_ = nullptr;
        }
        if (ctx_ != nullptr) {
            libusb_exit(ctx_);
            ctx_ = nullptr;
        }
    }

    int read_frame(ByteVector &read_buf) {
        return usb_read(ENDPOINT_1, read_buf, 0x400);
    }

    static size_t available_devices() {
        libusb_context *ctx = nullptr;
        if (libusb_init(&ctx) < 0 || ctx == nullptr) {
            return 0;
        }
        const size_t count = count_supported_devices(ctx, std::span(USB_VENDOR_PRODUCT_ID_LIST));
        libusb_exit(ctx);
        return count;
    }
};

class UPPCameraParser {
    static_assert(std::endian::native == std::endian::little);

    struct [[gnu::packed]] upp_usb_frame_t {
        uint16_t magic;
        uint8_t cid;
        uint16_t length;
    };

    struct [[gnu::packed]] upp_cam_frame_t {
        uint8_t fid;
        uint8_t cam_num;
        unsigned char has_g:1;
        unsigned char button_press:1;
        unsigned char other:6;
        uint32_t g_sensor;
    };

    static constexpr uint16_t UPP_USB_MAGIC = 0xBBAA;
    static constexpr uint8_t UPP_CAMID_7 = 7;
    static constexpr uint8_t UPP_CAMID_11 = 11;

    ByteVector camera_buffer_;
    uint16_t source_id_ = 0;
    upp_cam_frame_t cam_header_ = {};
    uint32_t frame_id_ = 0;

    FrameCallback frame_callback_;
    ButtonCallback button_callback_;

    static uint64_t now_us() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
    }

    void emit_frame() {
        if (camera_buffer_.empty()) {
            return;
        }

        CapturedFrame frame = {
            .jpeg = camera_buffer_,
            .source_id = source_id_,
            .frame_id = frame_id_++,
            .timestamp_us = now_us(),
        };
        frame_callback_(frame);
        camera_buffer_.clear();
    }

public:
    UPPCameraParser(FrameCallback frame_callback, ButtonCallback button_callback, uint16_t source_id)
        : source_id_(source_id),
          frame_callback_(std::move(frame_callback)),
          button_callback_(std::move(button_callback)) {}

    void flush_pending() {
        emit_frame();
    }

    void handle_upp_frame(const ByteVector &data) {
        const size_t usb_header_len = sizeof(upp_usb_frame_t);
        if (data.size() < usb_header_len) {
            return;
        }

        upp_usb_frame_t frame = {};
        std::memcpy(&frame, data.data(), usb_header_len);

        if (frame.magic != UPP_USB_MAGIC) {
            return;
        }
        if ((frame.cid != UPP_CAMID_7) && (frame.cid != UPP_CAMID_11)) {
            return;
        }
        if (usb_header_len + frame.length > data.size()) {
            return;
        }

        const size_t cam_header_len = sizeof(upp_cam_frame_t);
        if (data.size() - usb_header_len < cam_header_len) {
            return;
        }
        if (frame.length < cam_header_len) {
            return;
        }

        upp_cam_frame_t cam_part = {};
        std::memcpy(&cam_part, data.data() + usb_header_len, cam_header_len);

        if (!camera_buffer_.empty() && cam_header_.fid != cam_part.fid) {
            emit_frame();
        }

        if (camera_buffer_.empty()) {
            cam_header_ = cam_part;
            if (!((cam_header_.cam_num < 2) && (cam_header_.has_g == 0) && (cam_header_.other == 0))) {
                return;
            }
        } else {
            if (!((cam_header_.fid == cam_part.fid)
                  && (cam_header_.cam_num == cam_part.cam_num)
                  && (cam_header_.has_g == cam_part.has_g)
                  && (cam_header_.other == cam_part.other))) {
                return;
            }
        }

        if (cam_part.button_press && button_callback_) {
            button_callback_();
        }

        const auto cam_data_start = data.begin() + static_cast<std::ptrdiff_t>(usb_header_len + cam_header_len);
        const auto cam_data_end = data.begin() + static_cast<std::ptrdiff_t>(usb_header_len + frame.length);
        if (cam_data_start > cam_data_end) {
            return;
        }
        camera_buffer_.insert(camera_buffer_.end(), cam_data_start, cam_data_end);
    }
};

} // namespace

struct SupercameraCapture::Impl {
    UsbSupercamera usb;
    explicit Impl(uint16_t source_id) : usb(source_id) {}
};

SupercameraCapture::SupercameraCapture(uint16_t source_id, ButtonCallback button_callback)
    : impl_(std::make_unique<Impl>(source_id)),
      source_id_(source_id),
      button_callback_(std::move(button_callback)) {}

SupercameraCapture::~SupercameraCapture() = default;

void SupercameraCapture::request_stop() {
    stop_requested_ = true;
}

void SupercameraCapture::run(const FrameCallback &frame_callback) {
    if (!frame_callback) {
        throw std::invalid_argument("frame callback is required");
    }

    stop_requested_ = false;
    UPPCameraParser parser(frame_callback, button_callback_, source_id_);
    ByteVector read_buf;

    while (!stop_requested_) {
        const int ret = impl_->usb.read_frame(read_buf);
        if (ret == 0) {
            parser.handle_upp_frame(read_buf);
            continue;
        }
        if (ret == LIBUSB_ERROR_NO_DEVICE) {
            break;
        }
    }

    parser.flush_pending();
}

size_t SupercameraCapture::available_devices() {
    return UsbSupercamera::available_devices();
}

} // namespace supercamera
