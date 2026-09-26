/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <atomic>
#include <filesystem>
#include <string>

namespace launcher {
struct DiscInfo {
    bool supported = false;
    std::string message;
};
enum class VerifyState { Verified, Mismatch, Canceled, Error };
struct Verification {
    VerifyState state = VerifyState::Error;
    std::string message;
};
struct Preferences {
    std::string disc;
    std::string net_name = "PLAYER";
    std::string net_target;  // empty hosts our own connect code
    int net_delay = -1;      // auto; otherwise 0..4 frames
    int net_port = 0;        // operating-system allocated
    bool net_upnp = true;    // ask the router to forward net_port (net_upnp.c)

    bool vsync = true;
    bool fullscreen = false;
    float scale = 1.0f;
#if defined(__ANDROID__)
    float render_scale = 1.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 1;
#else
    float render_scale = 0.0f, volume = 1.0f;
    int msaa = 1, anisotropy = 16;
#endif
    int widescreen = 0;
    int filter_mode = 0;
    int backend = 0;
    bool mute = false, fps = false, reverb = true;
    bool check_updates = true;
    bool custom_textures = true;
    bool unlock_all = false;
    int hud_mode = 0;
    bool frozen_stadium = false;
    bool free_camera = false;
    bool ucf = false;
    // Per-controller-port index into the F1 menu's "MeleeVS: Tag Bind"
    // cycle (0 = Off, D-Pad Down only), one per physical GameCube port
    // 0-3; see kTagBindNames in launcher.cpp for the ordered list. The
    // pre-game launcher window (which has no port selector) edits port 0
    // only; the F1 PortMenu overlay edits whichever port it's currently
    // showing.
    int tag_bind[4] = {0, 0, 0, 0};
    float music_volume = 1.0f;
    float sfx_volume = 1.0f;
    uint64_t install_id = 0;  // random once per install (LAN host election); 0 = not yet
    // MeleeVS Matchmaking team from TEAM SELECT: the 9-byte PcNetTeam
    // (net.h) as 18 lowercase hex digits; empty until a team is saved.
    std::string meleevs_team;
};
DiscInfo inspect_disc(const std::string& path);
Verification verify_disc(
    const std::string& path, std::atomic_bool& cancel, std::atomic_uint& progress);
Preferences load_preferences(const std::filesystem::path& path);
bool save_preferences(
    const std::filesystem::path& path, const Preferences& prefs, std::string& error);
}  // namespace launcher
