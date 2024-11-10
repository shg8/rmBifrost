#include "lvgl_renderer.h"

#include <spdlog/spdlog.h>

std::weak_ptr<lvgl_renderer> lvgl_renderer::instance;

uint32_t lv_tick_cb()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void lvgl_renderer::lv_display_flush(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p)
{
    std::lock_guard<std::mutex> lock(fb_mutex);
    int fb_width = fb->width();
    int fb_height = fb->height();
    int fb_depth = fb->depth() / 8;
    assert(fb_depth == 4);

    point p1 = { area->x2, area->y2 };
    point p2 = { area->x1, area->y1 };
    bool pixel_changed = false;
    // copy the area from the lvgl buffer to the QImage buffer
    for (uint32_t y = area->y1; y <= area->y2; y++) {
        QRgb* line = reinterpret_cast<QRgb*>(fb->scanLine(y));
        int lvgl_idx = (y * fb_width + area->x1) * fb_depth;
        for (uint32_t x = area->x1; x <= area->x2; x++) {
            // find the smallest rectangle that contains pixel difference
            auto color = qRgba(color_p[lvgl_idx + 2], color_p[lvgl_idx + 1], color_p[lvgl_idx + 0], color_p[lvgl_idx + 3]);
            // alpha blending
            auto u = (255 - qAlpha(color)) / 255.0;
            auto a = qAlpha(color) / 255.0;
            auto composited_color = qRgba(qRed(line[x]) * u + qRed(color) * a,
                             qGreen(line[x]) * u + qGreen(color) * a,
                             qBlue(line[x]) * u + qBlue(color) * a,
                             a);

            if (line[x] != composited_color) {
                pixel_changed = true;
                p1.x = std::min(p1.x, x);
                p1.y = std::min(p1.y, y);
                p2.x = std::max(p2.x, x);
                p2.y = std::max(p2.y, y);
            }
            line[x] = composited_color;
            lvgl_idx += 4;
        }
    }

    spdlog::debug("requested flushing area: {}x{}-{}x{}; actual flushing area: {}x{}-{}x{}",
                  area->x1, area->y1, area->x2, area->y2, p1.x, p1.y, p2.x, p2.y);

    if (pixel_changed) {
        refresh_func(rect{ .p1 = p1, .p2 = p2 }, full_refresh_requested ? FULL : global_refresh_hint);
        full_refresh_requested = false;
    }
    lv_display_flush_ready(disp);
}

void lvgl_renderer::lv_display_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p)
{
    auto instance = lvgl_renderer::instance.lock();
    if (!instance)
        return;

    instance->lv_display_flush(disp, area, color_p);
}

void lvgl_renderer::initialize()
{
    instance = shared_from_this();
    std::lock_guard<std::mutex> lock(g_lvgl_mutex);

    lv_init();
    lv_tick_set_cb(lv_tick_cb);
    display = lv_display_create(fb->width(), fb->height());

    lv_indev_t* touch = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event3");
    lv_evdev_set_calibration(touch, 0, 0, 2058, 2826);
    lv_indev_set_display(touch, display);

    lv_indev_t* pen = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event2");
    lv_evdev_set_calibration(pen, 0, 0, 11172, 15328);
    lv_indev_set_display(pen, display);

    auto buf_size = fb->width() * fb->height() * fb->depth() / 8;
    compose_buffer = new uint8_t[buf_size];
    lv_display_set_buffers(display, compose_buffer, nullptr, buf_size, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, lv_display_flush_cb);

    lv_obj_set_style_bg_color(lv_screen_active(), LV_COLOR_MAKE(255, 255, 255), LV_STATE_DEFAULT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_ARGB8888);

    spdlog::info("lvgl renderer initialized.");
}

lvgl_renderer::~lvgl_renderer() {
    // TODO: free resources
}

void lvgl_renderer::tick()
{
    std::lock_guard<std::mutex> lock(g_lvgl_mutex);
    lv_timer_handler();
}

void lvgl_renderer::request_full_refresh()
{
    full_refresh_requested = true;
}

void lvgl_renderer::enable_transparent_bg()
{
    std::lock_guard<std::mutex> lock(g_lvgl_mutex);
    lv_obj_set_style_bg_opa(lv_screen_active(), LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_layer_bottom(), LV_OPA_TRANSP, LV_PART_MAIN);
}
