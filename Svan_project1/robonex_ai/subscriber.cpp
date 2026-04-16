// ============================================================
//  subscriber.cpp  —  Svan AI Brain  →  Xterra SDK Bridge
//  Receives DDS JoyData_ from Python brain, forwards to robot
//  via Xterra SDK (CommunicationManager / joystick_interface)
//  Production-safe: watchdog, velocity clamp, graceful shutdown
// ============================================================

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <algorithm>
#include <cmath>

// ── CycloneDDS C++ API ──────────────────────────────────────
#include "dds/dds.hpp"
#include "JoyData.hpp"

// ── Xterra SDK ──────────────────────────────────────────────
// Adjust paths relative to your xterra/custom/include/ root
#include "CommunicationManager.hpp"
#include "joystick_interface.hpp"
#include "DDSMux.hpp"

using namespace org::eclipse::cyclonedds;

// ============================================================
//  SAFETY CONSTANTS  — DO NOT INCREASE without hardware test
// ============================================================
static constexpr float  MAX_VX         =  0.5f;   // m/s forward/back
static constexpr float  MAX_VY         =  0.4f;   // m/s left/right
static constexpr float  MAX_WZ         =  0.8f;   // rad/s yaw
static constexpr auto   WATCHDOG_MS    = std::chrono::milliseconds(500);
static constexpr int    LOOP_SLEEP_MS  = 20;       // 50 Hz

// ── Robot mode IDs (match grokrules.py) ─────────────────────
static constexpr int    MODE_SLEEP     = 0;
static constexpr int    MODE_STAND     = 1;
static constexpr int    MODE_MOVE      = 4;

// ============================================================
//  GRACEFUL SHUTDOWN on Ctrl+C or kill signal
// ============================================================
std::atomic<bool> g_running{true};

void signal_handler(int /*sig*/) {
    std::cout << "\n[SAFETY] Shutdown signal received. Stopping robot..." << std::endl;
    g_running = false;
}

// ============================================================
//  VELOCITY CLAMP — belt + suspenders safety on top of Python
// ============================================================
inline float clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    // ── Register signal handlers ─────────────────────────────
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::cout << "========================================" << std::endl;
    std::cout << "  Svan AI Bridge — Xterra SDK v1.0"      << std::endl;
    std::cout << "  Watchdog timeout : 500 ms"              << std::endl;
    std::cout << "  Loop rate        : 50 Hz"               << std::endl;
    std::cout << "========================================" << std::endl;

    // ── 1. Initialise Xterra SDK ─────────────────────────────
    // CommunicationManager is the central SDK entry point.
    // It sets up the DDS domain, state machine, and motor link.
    CommunicationManager comm_manager;

    if (!comm_manager.init()) {
        std::cerr << "[ERROR] Xterra CommunicationManager init failed." << std::endl;
        std::cerr << "        Check LD_LIBRARY_PATH and CAN Bus interface." << std::endl;
        return 1;
    }
    std::cout << "[OK] Xterra CommunicationManager initialised." << std::endl;

    // JoystickInterface translates velocity commands to leg trajectories
    JoystickInterface joystick(comm_manager);

    if (!joystick.init()) {
        std::cerr << "[ERROR] JoystickInterface init failed." << std::endl;
        return 1;
    }
    std::cout << "[OK] JoystickInterface initialised." << std::endl;

    // Boot robot into safe STAND mode before accepting any commands
    joystick.setMode(MODE_STAND);
    std::cout << "[OK] Robot set to STAND (safe boot state)." << std::endl;

    // ── 2. Initialise CycloneDDS subscriber ──────────────────
    try {
        dds::domain::DomainParticipant participant(domain::default_id());

        dds::topic::Topic<xterra::msg::dds_::JoyData_> topic(
            participant, "rt/experiment/joystick_data"
        );

        dds::sub::DataReader<xterra::msg::dds_::JoyData_> reader(
            participant, topic
        );

        std::cout << "[OK] DDS subscriber ready on: rt/experiment/joystick_data" << std::endl;
        std::cout << "     Waiting for Python brain..." << std::endl;

        // ── 3. State tracking ─────────────────────────────────
        auto last_msg_time  = std::chrono::steady_clock::now();
        int  last_mode      = MODE_STAND;
        bool watchdog_fired = false;

        // ── 4. Main control loop ──────────────────────────────
        while (g_running) {
            auto samples = reader.take();
            bool got_msg = false;

            for (const auto& sample : samples) {
                if (!sample.info().valid()) continue;

                got_msg = true;
                last_msg_time  = std::chrono::steady_clock::now();
                watchdog_fired = false;

                const auto& msg = sample.data();

                // ── Read raw values ──────────────────────────
                const float raw_vy  = msg.axes()[0]; // left/right
                const float raw_vx  = msg.axes()[1]; // fwd/back
                const float raw_wz  = msg.axes()[3]; // yaw/turn

                const uint8_t btn_sleep = msg.buttons()[0];
                const uint8_t btn_stand = msg.buttons()[1];
                const uint8_t btn_move  = msg.buttons()[2];

                // ── Hard clamp (safety layer 2) ───────────────
                const float vx = clamp(raw_vx, -MAX_VX, MAX_VX);
                const float vy = clamp(raw_vy, -MAX_VY, MAX_VY);
                const float wz = clamp(raw_wz, -MAX_WZ, MAX_WZ);

                // ── Determine requested mode ──────────────────
                int requested_mode = last_mode; // default: hold current

                if      (btn_sleep == 1) requested_mode = MODE_SLEEP;
                else if (btn_stand == 1) requested_mode = MODE_STAND;
                else if (btn_move  == 1) requested_mode = MODE_MOVE;

                // ── Apply mode change (only on transition) ────
                if (requested_mode != last_mode) {
                    joystick.setMode(requested_mode);
                    last_mode = requested_mode;

                    const char* mode_names[] = {"SLEEP", "STAND", "?", "?", "MOVE"};
                    std::cout << "[MODE] -> "
                              << mode_names[requested_mode]
                              << std::endl;
                }

                // ── Send velocity (only in MOVE mode) ─────────
                if (last_mode == MODE_MOVE) {
                    joystick.setVelocity(vx, vy, wz);

                    // Only print on non-zero motion to reduce spam
                    if (std::fabs(vx) > 0.01f ||
                        std::fabs(vy) > 0.01f ||
                        std::fabs(wz) > 0.01f) {
                        std::cout << "[MOVE] vx=" << vx
                                  << "  vy=" << vy
                                  << "  wz=" << wz
                                  << std::endl;
                    }
                }
            }

            // ── 5. WATCHDOG — freeze if Python brain goes silent ──
            auto elapsed = std::chrono::steady_clock::now() - last_msg_time;

            if (elapsed > WATCHDOG_MS && !watchdog_fired) {
                watchdog_fired = true;
                std::cerr << "[WATCHDOG] No message for 500ms — FREEZING ROBOT" << std::endl;
                joystick.setMode(MODE_STAND);
                joystick.setVelocity(0.0f, 0.0f, 0.0f);
                last_mode = MODE_STAND;
            }

            std::this_thread::sleep_for(
                std::chrono::milliseconds(LOOP_SLEEP_MS)
            );
        }

        // ── 6. Clean shutdown ─────────────────────────────────
        std::cout << "[SHUTDOWN] Sending SLEEP command before exit..." << std::endl;
        joystick.setVelocity(0.0f, 0.0f, 0.0f);
        joystick.setMode(MODE_SLEEP);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        comm_manager.shutdown();
        std::cout << "[SHUTDOWN] Clean exit." << std::endl;

    } catch (const dds::core::Exception& e) {
        std::cerr << "[DDS ERROR] " << e.what() << std::endl;
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] " << e.what() << std::endl;
        return 1;
    }

    return 0;
}