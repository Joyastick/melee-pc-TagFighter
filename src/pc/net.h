/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PC_NET_H
#define PC_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Netplay prototype: rollback lockstep between two instances over UDP
 * (src/pc/net.c, docs/netcode-plan.md §4). Enabled at boot by
 * MELEE_NET=<peer host:port> or at runtime by pc_net_connect()
 * (src/pc/net_lan.h); see net.c for the other knobs. */

/* Wire protocol version; a peer with another one is refused (both sides
 * report PEER_INCOMPATIBLE). Bump on any change to the packet layouts,
 * Rules or the handshake. */
/* Version 6 requires sequenced scene exits and acknowledged LAN election.
 * Version 7 adds the sender's frame advantage to every input packet; the
 * phase controller acts on the difference of the two, so a peer that does
 * not send one cannot be synchronised against.
 * Version 8 appends a truncated keyed-BLAKE2b tag to every datagram, so a
 * peer that does not authenticate what it sends cannot be talked to at all
 * once the session key exists (src/pc/net_wire.c).
 * Version 9 adds Rules.game_mode (Tag Battle vs plain VS) and
 * Rules.tag_bind / Ready.tag_bind (each peer's own MeleeVS: Tag Bind).
 * Version 10 sends two pads per frame (each machine's player and its couch
 * partner, WireFrame) and adds Rules/Ready.partner_bind, so a MeleeVS duo
 * can share one machine (ports 3 and 4 follow ports 1 and 2's machines).
 * Version 11 adds Rules.layout and each machine's team setup (Rules.team,
 * Ready.team) for MeleeVS Matchmaking, which skips the online CSS/SSS. */
#define PC_NET_PROTO_VERSION 11
void pc_net_init(void);
void pc_net_set_input_delay(int frames);
bool pc_net_active(void);
/* Presentation-only quick chat is available in connected noncombat scenes. */
bool pc_net_chat_available(void);
/* True when the simulation must be reproducible elsewhere: netplay,
 * record, replay or sync test. Guards machine-seeded retail behaviour. */
bool pc_net_deterministic(void);

/* Netplay scene hand-off: true while the scene that asked to end must keep
 * ticking, so both peers leave it on the same frame however long their loads
 * took (src/melee/gm/gmscene.c, docs/netcode-plan.md section 5.2). */
bool pc_net_scene_hold(void);
/* Controller port the local player drives (0 = P1/host, 1 = P2/guest). */
int pc_net_local_player(void);

/* Frame of the tick being simulated (-1 before the first); identical on both
 * peers, so a scene change scheduled for a given frame lands in sync. */
int32_t pc_net_frame(void);
/* Scheduled lobby exit, including a still-pending host handshake. */
int32_t pc_net_start_frame(void);
/* Service transport without advancing simulation (lobby start fence). */
void pc_net_poll(void);
/* Matchmaking's local pause (net.c): true while this machine shows it.
 * leave_hold counts frames L+R+A+START has been held, out of leave_needed.
 * Either pointer may be NULL. */
bool pc_net_local_pause(int* leave_hold, int* leave_needed);
/* Internet rendezvous transfers its already-bound IPv4 socket. Ownership
 * transfers on success only; no new NAT mapping is created. */
bool pc_net_connect_socket(
    intptr_t socket, const char* ip, uint16_t port, int player, uint32_t seed);
/* The 32-byte secret pairing agreed for the next session (NULL clears it).
 * Set just before pc_net_connect_socket(); the handshake's datagram key is
 * then derived under it, and it is wiped when that session ends. */
void pc_net_set_session_secret(const uint8_t* secret);
/* Offered every IPv4 datagram on the session socket before the session sees
 * it; true consumes it. Runs on net.c's receive thread, and
 * pc_net_set_datagram_handler() returns only once no call into the previous
 * handler is still running, so what one reads must stay fixed while it is
 * installed. */
typedef bool (*PcNetDatagramHandler)(const void*, size_t, uint32_t, uint16_t);
void pc_net_set_datagram_handler(PcNetDatagramHandler handler);
bool pc_net_send_datagram(const void* data, size_t size, uint32_t address, uint16_t port);

/* RNG seed agreed for the session (pc_net_connect / match handshake). */
uint32_t pc_net_seed(void);

/* Match handshake progress: 0 idle, 1 pending, 2 done, 3 failed (15 s). */
int pc_net_handshake_state(void);

/* Match rules in force from the RULES handshake until disconnect: unlock-all
 * is on for both peers, frozen stadium is the host's setting. False when no
 * rules are in force (use the local prefs).
 *
 * While a session is in force the unlock masks in the save data are actually
 * written all-unlocked (net_handshake.c), so this flag now agrees with them
 * instead of overriding them; the direct mask readers agree too. */
bool pc_net_rules(bool* unlock_all, bool* frozen_stadium);

/* The remote peer's own MeleeVS: Tag Bind index (their pc_get_tag_bind(0)),
 * exchanged once at handshake time (Rules.tag_bind if they're the host,
 * Ready.tag_bind if they're the guest) - not agreed to a shared value like
 * pc_net_rules's fields, each peer keeps their own. 0 (Off) before the
 * handshake completes or when netplay is inactive. */
int pc_net_remote_tag_bind(void);

/* This machine's own MeleeVS: Tag Bind index, pinned at the moment it went
 * on the wire (Rules.tag_bind if we're the host, Ready.tag_bind if we're
 * the guest) rather than read live - see TagAssist_ExtraBindMask's caller
 * in tag_assist.c for why a live pc_get_tag_bind(0) read during an active
 * session is a desync risk (changing it mid-session takes effect on this
 * machine at once, while the peer keeps simulating this port with whatever
 * it cached at handshake time). Falls back to a live read before a session
 * exists. */
int pc_net_local_tag_bind(void);

/* MeleeVS couch duo: the Tag Bind of this machine's / the peer's couch
 * partner (a second controller on the same machine, driving game port
 * net.local + 2 / net.remote + 2), or -1 when that machine has none. Pinned
 * at handshake time like the player's own bind, only ever set in a Tag
 * Battle session, and -1 while no session is in force. */
int pc_net_local_partner_bind(void);
int pc_net_remote_partner_bind(void);
/* MeleeVS Matchmaking team setup: one per machine, fixed before searching
 * and carried by the handshake (Rules.team for the host, Ready.team for the
 * guest). fighter[0] is the machine's own player, fighter[1] its couch
 * partner (human) or CPU assist. point is the fighter that starts on point:
 * a human+CPU team must start with the human. Exact wire image. */
typedef struct PcNetTeamFighter {
    int8_t ckind;      /* CharacterKind, 0..CKind_Playable_Count-1 */
    uint8_t color;     /* costume, < gm_GetNumCostumesForCKind(ckind) */
    uint8_t human;     /* 1: a player; 0: CPU assist */
    uint8_t cpu_level; /* 1..9, CPU fighters only */
} __attribute__((packed)) PcNetTeamFighter;
typedef struct PcNetTeam {
    PcNetTeamFighter fighter[2];
    uint8_t point; /* 0 or 1 */
} __attribute__((packed)) PcNetTeam;

/* Set before connecting: whether this side is joining a Matchmaking game,
 * and its team. The host's choice decides the session (Rules.layout); a
 * NULL team clears it (Direct, LAN, plain VS). */
void pc_net_set_matchmade(bool matchmade, const PcNetTeam* team);
/* After the handshake: whether this session is a Matchmaking game, and each
 * machine's team (machine 0 the host, 1 the guest; NULL outside one). */
bool pc_net_matchmade(void);
const PcNetTeam* pc_net_team(int machine);
/* Game port (0-3) of a machine's fighter: slot 0 its player, slot 1 its
 * partner or assist. Direct/LAN keep MeleeVS's CSS layout (host 1+3, guest
 * 2+4); Matchmaking puts each team together (host 1+2, guest 3+4). */
int pc_net_game_port(int machine, int slot);

/* Whether game port 0-3 is a human in this session: ports 0/1 always, 2/3
 * only for a machine that announced a couch partner. The CSS uses it to
 * open those doors as Human instead of CPU. */
bool pc_net_port_human(int port);

/* Called once per simulation tick before the pad queue head is consumed.
 * Replaces the head sample's four ports with the synced inputs for this
 * frame, predicting the remote one when it has not arrived (stalling only
 * when the remote is more than the rollback window behind). */
void pc_net_sync(void);

/* Called after each tick. Returns true when the tick must be run again
 * (rollback re-simulation or the MELEE_NET_SYNCTEST self-check). */
/* Finish rollback/bookkeeping, but defer fresh advances during scene exit. */
bool pc_net_after_tick(bool scene_ending);

/* Called by the frame boundary (src/pc/vi.c) after the pad alarm ran; the
 * returned ns are added to the next pacing wait. Time-sync corrections are
 * paid here rather than by sleeping inside a tick, and only ever lengthen a
 * frame: the peer that is behind is caught by the one ahead slowing down. */
uint64_t pc_net_pace_adjust_ns(void);

/* Called by the frame boundary before its pacing sleep with how late the
 * boundary is against its schedule; returns how much of that to run off by
 * skipping the sleep. The rest is dropped. */
uint64_t pc_net_catch_up_ns(uint64_t late_ns);

/* True while re-simulating: sound/music/rumble starts must be suppressed. */
bool pc_net_resim(void);
/* Reconcile physical motors after rollback; hardware state is not snapshotted. */
void pc_net_rumble_command(unsigned port, unsigned command);

/* The simulation asks the audio engine two questions whose answers live
 * outside every snapshot and move in real time: "did this sound start, and
 * with what voice id" (AXDriver_8038CFF4) and "is that voice still playing"
 * (AXDriver_8038D9D8 -> HSD_SynthSFXCheck, whose nodes sit in the excluded
 * HSD_Synth heap and whose voice state the audio engine clears when the
 * sample ends). A rollback re-runs a frame one round trip later, so on any
 * link with real latency the second answer differs from the first, the
 * simulation branches differently (src/melee/sfx/crowdsfx.c,
 * src/melee/gr/ground.c) and the frame's checksum diverges from the peer
 * that never re-ran it. So the answers are journalled per frame: the first
 * simulation of a frame asks the engine and records, every later
 * simulation of that frame replays the same answers and touches nothing.
 * MELEE_NET_AUDIO_JOURNAL=off restores the old behaviour.
 *
 * replay: true when this frame already has an answer at this point in its
 * call order, and *out is it. record: stores and returns v. Both are no-ops
 * outside a netplay tick on the game thread. */
bool pc_net_audio_replay(int32_t* out);
int32_t pc_net_audio_record(int32_t v);

/* The second question -- "is that voice still playing" (AXDriver_8038D9D8 ->
 * HSD_SynthSFXCheck) -- cannot be journalled into agreement, because each
 * peer's audio engine runs on its own wall clock and a peer that stalled for
 * a round trip has aged the voice further than the peer that did not. During
 * a session the simulation is therefore told "no, it finished" (true return,
 * *answer = false); outside one the real answer stands. That is a behaviour
 * change: crowd cheers no longer wait for the previous cheer to end and the
 * ground.c:3125 gate reads as "the stage sound is over".
 * MELEE_NET_AUDIO_DEAF=off restores the real answer.
 * pc_net_audio_deaf_note() records what the engine would have said, so the
 * periodic report can show how often that differs. */
bool pc_net_audio_deaf(bool* answer);
void pc_net_audio_deaf_note(bool live);

/* Called whenever the game issues a disc request: a tick that did I/O can
 * never be re-simulated (completions land on worker threads). */
void pc_net_note_io(void);

/* Live netplay numbers for the HUD. False when netplay is not active. */
bool pc_net_stats(int* ping_ms, int* delay_frames, unsigned* rollbacks);

/* Link quality for the HUD/lobby: 0 stable, 1 warning (loss, jitter or
 * deep rollbacks in the last second), 2 stalling (a stall over 500 ms in the
 * last two seconds, or the peer announced it is leaving), 3 reconnecting
 * (the peer has been silent past the stall timeout and the session is being
 * resumed, net.c's MELEE_NET_RECONNECT_MS). A reader that only knows 0-2
 * must treat anything above 2 as at least as bad as 2. */
int pc_net_quality(void);

/* True once this session's checksums have disagreed with the peer's. The
 * simulations have parted and no rollback will bring them back; the match is
 * no longer a match. Reported so the player can see it -- it used to be a
 * log line and nothing else, which left two players finishing a game that
 * only one of them was playing. */
bool pc_net_desync(void);

/* Why the last session ended (kept until the next connect): 0 still up /
 * never broke, 1 the peer left (BYE), 2 timeout, 3 desync, 4 incompatible
 * protocol version, 5 an interruption could not be resumed (the input gap
 * outran the rings, or the peer answered for another session or seed). */
enum {
    PC_NET_PEER_OK,
    PC_NET_PEER_LEFT,
    PC_NET_PEER_TIMEOUT,
    PC_NET_PEER_DESYNC,
    PC_NET_PEER_INCOMPATIBLE,
    PC_NET_PEER_RESUME
};
int pc_net_peer_status(void);
void pc_net_peer_status_clear(void);

#ifdef __cplusplus
}
#endif

#endif
