/*
 * Proof of concept for the 'Geek szitman supercamera' endoscope
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>
#include <version>

#ifndef __cpp_lib_format
#include <ctime>
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"
#endif
#include <opencv2/highgui.hpp>
#pragma GCC diagnostic pop

#include "supercamera_core.hpp"

#define KRST "\e[0m"
#define KMAJ "\e[0;35m"
#define KCYN "\e[0;36m"

static std::mutex gui_mtx;
static supercamera::ByteVector latest_frame;
static std::atomic_uint32_t latest_frame_id = 0;
static std::atomic_bool save_next_frame = false;
static std::atomic_bool exit_program = false;
static constexpr std::string_view pic_dir = "pics";

static void pic_callback(const supercamera::CapturedFrame &frame)
{
    std::cout << KCYN "PIC i:" << frame.frame_id << " size:" << frame.jpeg.size() << KRST << std::endl;

    if (save_next_frame) {
        save_next_frame = false;
        std::ostringstream filename;
        auto tp = std::chrono::system_clock::now();

#ifdef __cpp_lib_format
        std::string date = std::format("{:%FT%T}", std::chrono::floor<std::chrono::seconds>(tp));
#else
        std::time_t t = std::chrono::system_clock::to_time_t(tp);
        auto date = std::put_time(std::localtime(&t), "%FT%T");
#endif

        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count() % 1000;
        filename << pic_dir << "/frame_" << date
                 << "." << std::setfill('0') << std::setw(3) << millis << ".jpg";
        std::ofstream output(filename.str(), std::ios::binary);
        output.write(reinterpret_cast<const char *>(frame.jpeg.data()), static_cast<std::streamsize>(frame.jpeg.size()));
        std::cout << "Saved frame to " << filename.str() << std::endl;
    }

    {
        std::lock_guard lock(gui_mtx);
        latest_frame = frame.jpeg;
        latest_frame_id = frame.frame_id;
    }
}

static void button_callback() {
    std::cout << KMAJ "BUTTON PRESS" KRST << std::endl;
    save_next_frame = true;
}

static void gui() {
    constexpr const char *window_name = "Geek szitman supercamera - PoC";
    uint32_t frame_done = latest_frame_id.load();

    while (!exit_program) {
        int key = cv::waitKey(10);
        if (key == 'q' || key == '\e') {
            exit_program = true;
        }

        const uint32_t newest_frame = latest_frame_id.load();
        if (frame_done != newest_frame) {
            cv::Mat img;
            {
                std::lock_guard lock(gui_mtx);
                img = cv::imdecode(latest_frame, cv::IMREAD_COLOR);
                frame_done = newest_frame;
            }
            if (img.data != nullptr) {
                cv::namedWindow(window_name, cv::WINDOW_AUTOSIZE);
                cv::imshow(window_name, img);
            }
        }
    }

    cv::destroyWindow(window_name);
}

int main()
{
    try {
        std::filesystem::create_directory(pic_dir);

        supercamera::SupercameraCapture capture(0, button_callback);
        std::thread capture_thread([&capture] {
            try {
                capture.run(pic_callback);
            } catch (const std::exception &e) {
                std::cerr << e.what() << std::endl;
            }
            exit_program = true;
        });

        gui();

        capture.request_stop();
        capture_thread.join();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << std::endl;
        return 1;
    }
}
