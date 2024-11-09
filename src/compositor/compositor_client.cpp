#include "compositor_client.h"

#include <spdlog/spdlog.h>
#include <vector>

#include "../constants.h"
#include "packets/begin_session_request.h"
#include "packets/begin_session_response.h"
#include "packets/packet.h"
#include "packets/release_frame_packet.h"
#include "packets/submit_frame_packet.h"

#include <cereal/archives/binary.hpp>
#include <cereal/types/polymorphic.hpp>
#include <optional>
compositor_client::compositor_client(std::unique_ptr<unix_socket::connection> conn, compositor_client_config cfg)
    : conn(std::move(conn)), cfg(cfg)
{
}

void compositor_client::start()
{
    running = true;
    spdlog::debug("Starting client thread");
    client_thread = std::thread([this] {
        while (running) {
            try {
                size_t size = 0;
                conn->read(reinterpret_cast<char*>(&size), sizeof(size));
                if (size > 4096) {
                    spdlog::warn("Packet size too large: {}", size);
                    continue;
                }

                spdlog::debug("Reading packet of size {}", size);

                std::vector<char> buffer(size);
                conn->read(buffer.data(), size);

                std::istringstream iss(std::string(buffer.begin(), buffer.end()));
                cereal::BinaryInputArchive archive(iss);

                std::shared_ptr<packet> packet;
                archive(packet);

                spdlog::debug("Handling packet");
                handle_packet(packet);
            } catch (const std::exception& e) {
                spdlog::error("Error handling packet: {}", e.what());
                stop();
            }
        }
        conn->close();
        spdlog::debug("Client thread finished");
    });
}

void compositor_client::stop()
{
    state = client_state::DISCONNECTED;
    running = false;
}

std::vector<uint64_t> compositor_client::create_swapchain_images() {
    // get page size
    size_t page_size = sysconf(_SC_PAGE_SIZE);
    
    // page aligned image size
    auto image_size = SCREEN_WIDTH * SCREEN_HEIGHT * 4;
    aligned_image_size = (image_size + page_size - 1) & ~(page_size - 1);

    size_t total_size = aligned_image_size * swapchain_image_count;

    // create shm channel
    shared_memory = std::make_unique<shm_channel>(session_name, total_size, false);

    std::vector<uint64_t> offsets;
    for (size_t i = 0; i < swapchain_image_count; i++) {
        offsets.push_back(i * aligned_image_size);
    }
    return offsets;
}

void compositor_client::handle_packet(const std::shared_ptr<packet>& packet)
{
    if (auto req = std::dynamic_pointer_cast<begin_session_request>(packet)) {
        spdlog::info("Application {} requested session creation", req->application_name);
        if (state != client_state::CONNECTED) {
            spdlog::warn("Invalid client state: {}", static_cast<int>(state));
            stop();
            return;
        }

        application_name = req->application_name;
        window_title = req->window_title;
        prefer_full_screen = req->prefer_full_screen;

        std::string random_string(8, '\0');
        std::generate(random_string.begin(), random_string.end(), []() {
            return 'a' + rand() % 26;
        });
        session_name = "sess_" + random_string;
        swapchain_image_count = std::clamp(static_cast<uint32_t>(req->swapchain_image_count), 1u, 2u);
        auto swapchain_image_offsets = create_swapchain_images();
        swapchain_extent = {SCREEN_WIDTH, SCREEN_HEIGHT - cfg.title_bar_height};
        composite_region = {{0, cfg.title_bar_height}, {SCREEN_WIDTH, SCREEN_HEIGHT}};

        spdlog::debug("Created shared memory channel with id {} and size {}", session_name, aligned_image_size * swapchain_image_count);

        framebuffer_in_flight.resize(swapchain_image_count, false);
        submission_info.resize(swapchain_image_count);

        spdlog::debug("Created swapchain with {} images", swapchain_image_count);

        state = client_state::SESSION_STARTED;

        auto resp = std::make_shared<begin_session_response>();
        resp->swapchain_image_count = swapchain_image_count;
        resp->session_name = session_name;
        resp->shared_memory_size = aligned_image_size * swapchain_image_count;
        resp->swapchain_image_offsets = std::move(swapchain_image_offsets);
        resp->swapchain_extent = swapchain_extent;
        send_packet(resp);
    } else if (auto req = std::dynamic_pointer_cast<submit_frame_packet>(packet)) {
        auto fb_id = req->framebuffer_id;
        if (framebuffer_in_flight.size() <= fb_id) {
            spdlog::warn("Invalid framebuffer id {} submitted by {}", fb_id, application_name);
            stop();
            return;
        }

        if (framebuffer_in_flight[fb_id]) {
            spdlog::warn("Framebuffer {} already in flight", fb_id);
            stop();
            return;
        }

        framebuffer_in_flight[fb_id] = true;
        submission_info[fb_id] = *req;
        submitted_frame_ids.push(fb_id);
    }
}

void compositor_client::send_packet(const std::shared_ptr<packet>& resp)
{
    std::lock_guard<std::mutex> lock(packet_write_mutex);
    std::ostringstream oss;
    cereal::BinaryOutputArchive archive(oss);
    archive(resp);

    auto data = oss.str();
    size_t size = data.size();

    try {
        conn->write(reinterpret_cast<const char*>(&size), sizeof(size));
        conn->write(data.data(), size);
    } catch (const std::exception& e) {
        spdlog::error("Failed to send packet: {}", e.what());
        stop();
    }
}

std::optional<std::tuple<uint32_t, submit_frame_packet, rect, uint64_t>> compositor_client::get_swapchain_image() {
    if (submitted_frame_ids.empty()) {
        return std::nullopt;
    }
    auto frame_id = submitted_frame_ids.front();
    submitted_frame_ids.pop();
    return std::make_tuple(frame_id, submission_info[frame_id], composite_region, reinterpret_cast<uint64_t>(shared_memory->data) + frame_id * aligned_image_size);
}

void compositor_client::release_swapchain_image(uint32_t frame_id) {
    if (framebuffer_in_flight.size() <= frame_id || !framebuffer_in_flight[frame_id]) {
        throw std::runtime_error("Invalid framebuffer id");
    }
    framebuffer_in_flight[frame_id] = false;
    
    auto resp = std::make_shared<release_frame_packet>();
    resp->framebuffer_id = frame_id;
    send_packet(resp);
}
