#include "remotedesktop/screen_capturer.h"

#include <algorithm>
#include <cstring>

namespace device_agent::remotedesktop {

namespace {

bool validFrame(const ScreenFrame& frame) {
    const uint32_t min_stride = static_cast<uint32_t>(frame.width) * 4;
    return frame.width > 0 && frame.height > 0 && frame.stride >= min_stride &&
           frame.bgra.size() >= static_cast<size_t>(frame.stride) * frame.height;
}

std::vector<Rect> fullFrame(const ScreenFrame& frame) {
    if (frame.width == 0 || frame.height == 0) {
        return {};
    }
    return {Rect{0, 0, frame.width, frame.height}};
}

bool tileChanged(const ScreenFrame& previous,
                 const ScreenFrame& current,
                 uint16_t x,
                 uint16_t y,
                 uint16_t width,
                 uint16_t height) {
    const size_t bytes = static_cast<size_t>(width) * 4;
    for (uint16_t row = 0; row < height; ++row) {
        const size_t previous_offset = static_cast<size_t>(y + row) * previous.stride +
                                       static_cast<size_t>(x) * 4;
        const size_t current_offset = static_cast<size_t>(y + row) * current.stride +
                                      static_cast<size_t>(x) * 4;
        if (std::memcmp(previous.bgra.data() + previous_offset,
                        current.bgra.data() + current_offset,
                        bytes) != 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::vector<Rect> computeTileDirtyRects(const ScreenFrame& previous,
                                        const ScreenFrame& current,
                                        uint16_t tile_size,
                                        size_t max_rects,
                                        uint8_t full_frame_percent) {
    if (!validFrame(current)) {
        return {};
    }
    if (!validFrame(previous) || previous.width != current.width ||
        previous.height != current.height) {
        return fullFrame(current);
    }

    tile_size = std::max<uint16_t>(1, tile_size);
    max_rects = std::max<size_t>(1, max_rects);
    full_frame_percent = std::clamp<uint8_t>(full_frame_percent, 1, 100);

    std::vector<Rect> completed;
    std::vector<Rect> active;
    for (uint32_t y = 0; y < current.height; y += tile_size) {
        const auto tile_y = static_cast<uint16_t>(y);
        const auto tile_height = static_cast<uint16_t>(
            std::min<uint32_t>(tile_size, static_cast<uint32_t>(current.height) - y));
        std::vector<Rect> runs;
        bool in_run = false;
        uint16_t run_x = 0;
        uint16_t run_width = 0;
        for (uint32_t x = 0; x < current.width; x += tile_size) {
            const auto tile_x = static_cast<uint16_t>(x);
            const auto tile_width = static_cast<uint16_t>(
                std::min<uint32_t>(tile_size, static_cast<uint32_t>(current.width) - x));
            const bool changed = tileChanged(previous, current, tile_x, tile_y,
                                             tile_width, tile_height);
            if (changed) {
                if (!in_run) {
                    in_run = true;
                    run_x = tile_x;
                    run_width = 0;
                }
                run_width = static_cast<uint16_t>(run_width + tile_width);
            } else if (in_run) {
                runs.push_back(Rect{run_x, tile_y, run_width, tile_height});
                in_run = false;
            }
        }
        if (in_run) {
            runs.push_back(Rect{run_x, tile_y, run_width, tile_height});
        }

        std::vector<bool> extended(active.size(), false);
        std::vector<Rect> next_active;
        next_active.reserve(runs.size());
        for (const Rect& run : runs) {
            bool merged = false;
            for (size_t i = 0; i < active.size(); ++i) {
                Rect& prior = active[i];
                if (!extended[i] && prior.x == run.x && prior.width == run.width &&
                    static_cast<uint32_t>(prior.y) + prior.height == run.y) {
                    prior.height = static_cast<uint16_t>(prior.height + run.height);
                    next_active.push_back(prior);
                    extended[i] = true;
                    merged = true;
                    break;
                }
            }
            if (!merged) {
                next_active.push_back(run);
            }
        }
        for (size_t i = 0; i < active.size(); ++i) {
            if (!extended[i]) {
                completed.push_back(active[i]);
            }
        }
        active = std::move(next_active);
        if (completed.size() + active.size() > max_rects) {
            return fullFrame(current);
        }
    }
    completed.insert(completed.end(), active.begin(), active.end());

    uint64_t dirty_pixels = 0;
    for (const Rect& rect : completed) {
        dirty_pixels += static_cast<uint64_t>(rect.width) * rect.height;
    }
    const uint64_t frame_pixels = static_cast<uint64_t>(current.width) * current.height;
    if (dirty_pixels * 100 >= frame_pixels * full_frame_percent) {
        return fullFrame(current);
    }
    return completed;
}

}  // namespace device_agent::remotedesktop
