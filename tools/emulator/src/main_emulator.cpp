// pixfrog UI emulator — runs the device's real menu/canvas/splash code on a PC
// against an SDL2 framebuffer instead of the ST7789 TFT.
//
// Two ways to drive it:
//   • interactive  : SDL window + keyboard (Up/Down/wheel = rotate, Enter/Space = click)
//   • agent / CI   : line protocol on stdin (left|right|click|shot <p>|set ...|state|quit)
//
// Pass --headless (or set SDL_VIDEODRIVER=dummy) to run without a window; the
// framebuffer and `shot` screenshots still work, fully piloted by stdin.

#include <SDL2/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include "config_store.h"
#include "ui.h"
#include "ui_internal.h"

#include "dmx_emu.h"
#include "encoder_emu.h"
#include "tft_emu.h"

namespace ui   = pixfrog::ui;
namespace det  = pixfrog::ui::detail;
using clock_t_ = std::chrono::steady_clock;

// ── ui:: globals normally provided by components/ui/src/ui.cpp ────────────────
// menu.cpp reads ui::get_ip() / ui::is_link_up() for the HOME dashboard.
namespace pixfrog::ui {
namespace {
uint32_t g_ip  = 0;
bool g_link_up = false;
}  // namespace
void set_ip(uint32_t host_order_ip) {
    g_ip = host_order_ip;
}
uint32_t get_ip() {
    return g_ip;
}
void set_link_up(bool up) {
    g_link_up = up;
}
bool is_link_up() {
    return g_link_up;
}
}  // namespace pixfrog::ui

namespace {

constexpr int kFbW  = 320;
constexpr int kFbH  = 240;
constexpr int kZoom = 3;  // window = 960x720

// ── stdin command queue (filled by reader thread, drained on main thread) ─────
std::mutex g_cmd_mtx;
std::deque<std::string> g_cmds;
std::atomic<bool> g_running{ true };

void stdin_reader() {
    std::string line;
    char buf[512];
    while (g_running.load()) {
        if (!std::fgets(buf, sizeof(buf), stdin)) {
            // EOF: let the main loop finish, then stop.
            std::lock_guard<std::mutex> lk(g_cmd_mtx);
            g_cmds.emplace_back("quit");
            return;
        }
        line = buf;
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (line.empty()) continue;
        std::lock_guard<std::mutex> lk(g_cmd_mtx);
        g_cmds.push_back(line);
    }
}

bool pop_cmd(std::string& out) {
    std::lock_guard<std::mutex> lk(g_cmd_mtx);
    if (g_cmds.empty()) return false;
    out = g_cmds.front();
    g_cmds.pop_front();
    return true;
}

// ── Screenshot: build a surface from the RGB565 FB and save as BMP ────────────
void save_shot(const char* path) {
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        const_cast<uint16_t*>(emu_fb_ptr()), kFbW, kFbH, 16, kFbW * 2, SDL_PIXELFORMAT_RGB565);
    if (!surf) {
        std::printf("error: surface: %s\n", SDL_GetError());
        std::fflush(stdout);
        return;
    }
    const int rc = SDL_SaveBMP(surf, path);
    SDL_FreeSurface(surf);
    std::printf(rc == 0 ? "ok shot %s\n" : "error: save %s\n", path);
    std::fflush(stdout);
}

void print_state() {
    const char* name = "?";
    int cursor = 0, channel = 0;
#ifdef PIXFROG_EMULATOR
    det::menu_debug_state(&name, &cursor, &channel);
#endif
    std::printf("{\"screen\":\"%s\",\"cursor\":%d,\"channel\":%d}\n", name, cursor, channel);
    std::fflush(stdout);
}

uint32_t parse_ip(const char* s) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 0;
    return (a << 24) | (b << 16) | (c << 8) | d;
}

// Execute one agent command. Returns false on "quit".
bool exec_cmd(const std::string& line) {
    if (line == "left") {
        emu_push_event(EmuEvent::RotateLeft);
    } else if (line == "right") {
        emu_push_event(EmuEvent::RotateRight);
    } else if (line == "click") {
        emu_push_event(EmuEvent::Click);
    } else if (line == "longclick") {
        emu_push_event(EmuEvent::LongPress);
    } else if (line.rfind("shot", 0) == 0) {
        const char* p = line.c_str() + 4;
        while (*p == ' ')
            ++p;
        save_shot(*p ? p : "shot.bmp");
    } else if (line.rfind("splash", 0) == 0) {
        // splash <ms> [path] — render the boot animation at t=ms and shot it.
        // Rendered + captured here because the menu loop repaints every frame.
        const char* p = line.c_str() + 6;
        while (*p == ' ')
            ++p;
        char* end           = nullptr;
        const uint32_t t_ms = static_cast<uint32_t>(std::strtoul(p, &end, 10));
        p                   = end;
        while (*p == ' ')
            ++p;
        det::splash_render(t_ms, false);
        save_shot(*p ? p : "splash.bmp");
    } else if (line == "state") {
        print_state();
    } else if (line.rfind("set ip ", 0) == 0) {
        ui::set_ip(parse_ip(line.c_str() + 7));
    } else if (line == "set link up") {
        ui::set_link_up(true);
    } else if (line == "set link down") {
        ui::set_link_up(false);
    } else if (line.rfind("set fps ", 0) == 0) {
        emu_dmx_set_stats(static_cast<uint32_t>(std::strtoul(line.c_str() + 8, nullptr, 10)), 0);
    } else if (line.rfind("set pkts ", 0) == 0) {
        emu_dmx_set_pkts(std::strtoull(line.c_str() + 9, nullptr, 10));
    } else if (line.rfind("set active ", 0) == 0) {
        int ch = std::atoi(line.c_str() + 11);
        emu_dmx_set_active(ch, true);
    } else if (line == "quit") {
        return false;
    } else {
        std::printf("error: unknown cmd '%s'\n", line.c_str());
        std::fflush(stdout);
    }
    return true;
}

void map_key(SDL_Keycode k) {
    switch (k) {
    case SDLK_UP:
    case SDLK_LEFT: emu_push_event(EmuEvent::RotateLeft); break;
    case SDLK_DOWN:
    case SDLK_RIGHT: emu_push_event(EmuEvent::RotateRight); break;
    case SDLK_RETURN:
    case SDLK_SPACE: emu_push_event(EmuEvent::Click); break;
    case SDLK_BACKSPACE: emu_push_event(EmuEvent::LongPress); break;
    default: break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool headless = false;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--headless") == 0) headless = true;
    if (headless) SDL_SetHint(SDL_HINT_VIDEODRIVER, "dummy");

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* win   = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex  = nullptr;
    if (!headless) {
        win = SDL_CreateWindow("pixfrog emulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               kFbW * kZoom, kFbH * kZoom, 0);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, kFbW,
                                kFbH);
    }

    pixfrog::config::init();
    det::TftConfig tcfg{};
    tcfg.width  = kFbW;
    tcfg.height = kFbH;
    det::tft_init(tcfg);
    det::menu_init();

    std::thread reader(stdin_reader);

    auto present = [&]() {
        if (headless) return;
        SDL_UpdateTexture(tex, nullptr, emu_fb_ptr(), kFbW * 2);
        SDL_RenderClear(ren);
        SDL_RenderCopy(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);
    };

    // ── Splash phase (mirrors ui.cpp task_main) ───────────────────────────────
    // Skipped in headless so agent/CI runs start deterministically at HOME.
    if (!headless) {
        const auto t0 = clock_t_::now();
        bool done     = false;
        while (g_running.load() && !done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT)
                    g_running = false;
                else if (ev.type == SDL_KEYDOWN)
                    map_key(ev.key.keysym.sym);
            }
            std::string cmd;
            while (pop_cmd(cmd))
                if (!exec_cmd(cmd)) g_running = false;

            const bool clicked  = (det::encoder_poll() == det::Event::Click);
            const uint32_t t_ms = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - t0)
                    .count());
            done = det::splash_render(t_ms, clicked);
            present();
            std::this_thread::sleep_for(std::chrono::milliseconds(headless ? 1 : 16));
        }
    }

    // ── Main menu loop ─────────────────────────────────────────────────────────
    const uint32_t idle_ms = pixfrog::config::get_global().home_timeout_s * 1000u;
    auto last_event        = clock_t_::now();

    while (g_running.load()) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                g_running = false;
            else if (ev.type == SDL_KEYDOWN)
                map_key(ev.key.keysym.sym);
            else if (ev.type == SDL_MOUSEWHEEL)
                emu_push_event(ev.wheel.y > 0 ? EmuEvent::RotateLeft : EmuEvent::RotateRight);
        }
        // One command per iteration: input commands push an encoder event that
        // is dispatched + rendered below in the SAME iteration, so a following
        // `shot`/`state` (next iteration) observes the up-to-date screen.
        std::string cmd;
        if (pop_cmd(cmd))
            if (!exec_cmd(cmd)) g_running = false;

        det::Event e;
        while ((e = det::encoder_poll()) != det::Event::None) {
            det::menu_dispatch(e);
            last_event = clock_t_::now();
        }

        if (idle_ms &&
            std::chrono::duration_cast<std::chrono::milliseconds>(clock_t_::now() - last_event)
                    .count() > idle_ms) {
            det::menu_on_idle_timeout();
            last_event = clock_t_::now();
        }

        det::menu_render();
        det::canvas_flush();
        present();
        std::this_thread::sleep_for(std::chrono::milliseconds(headless ? 1 : 16));
    }

    g_running = false;
    if (reader.joinable()) reader.detach();  // fgets may block; don't hang on exit

    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
