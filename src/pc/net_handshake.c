/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Netplay match handshake: RULES host -> guest, READY back. RULES carries
 * the seed, start_frame (the frame the lobby leaves for the CSS on both
 * peers; each side applies the seed when it learns it and again entering
 * that frame, and frame checksums are only compared from it on, since the
 * two lobbies run different states until then) and everything
 * match-affecting from plan §5 item 7: the memcard GameRules, the
 * item/stage switches from GamePrefs and the frozen-stadium toggle. The
 * guest overwrites its copies (restored at disconnect) so CSS/SSS/match
 * read the same values on both peers. Every session runs it: the lobby
 * drives it from pc_lan_poll()/net_match.c, and a direct MELEE_NET session
 * drives it itself from handshake_direct() at the bottom of this file.
 *
 * Unlock state gets the same treatment, but written for real: both sides
 * pin the whole unlock surface to pc_unlock_state_all() before RULES is
 * built (unlock_force below) and put the player's own back at disconnect,
 * and RULES/READY each carry an FNV hash of what the masks actually came
 * out as, so two builds that disagree refuse the handshake. Before this,
 * unlock-all was a read-time override on three of the four predicates and
 * the direct mask readers bypassed it, so the two peers could differ by a
 * single HSD_Randi draw and desync on the first frame of the match.
 *
 * Freshness: each side draws a 64-bit nonce from the platform CSPRNG at
 * handshake time. RULES carries the host's; READY carries the guest's plus
 * the host's echoed back, and the session id is folded into both payload
 * hashes (rules_hash/ready_hash, net_wire.c). So a RULES/READY captured off
 * one session cannot be replayed into another, and a stale process at the
 * peer's address cannot drive the handshake with an old payload. After the
 * handshake is done a further RULES or READY is logged once and dropped,
 * never applied.
 *
 * What this is NOT: authentication. The nonces travel in the clear, so an
 * on-path attacker who can read them can still forge either side and
 * impersonate a peer; and the session id (net.c:879) is a perf-counter/pid
 * mix, not a secret. This raises the bar to "must see the traffic" and no
 * higher. Real peer identity is the M5 ed25519 work in docs/netcode-plan.md
 * §9 (signed RULES/READY with a long-term key); Monocypher is not vendored
 * yet, so none of it is implemented here. */
#include "compat.h"
#include "pc/net_internal.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wscalar-storage-order" /* disc-struct unions in lb/types.h */
#include <melee/gm/gm_1601.h>
#include <melee/gm/gmmain_lib.h>
#pragma GCC diagnostic pop
#include <melee/mod/tag_assist.h>
#include <sysdolphin/baselib/random.h>

#include <SDL3/SDL_timer.h>
#include <string.h>

/* ---- nonce randomness -------------------------------------------------
 * The nonces are the only value in the session that must be unguessable,
 * so they come from the platform CSPRNG and nowhere else: pc_install_id()
 * (pc.h:65) is persistent and public, and HSD_Rand is the game's
 * deterministic RNG whose seed is on the wire. There is deliberately no
 * fallback - a machine that cannot produce 8 random bytes fails the
 * handshake instead of producing a predictable nonce. */
#if defined(MELEE_USE_BCRYPT)
#include <bcrypt.h>
static bool csprng(void* out, size_t n) {
    return BCryptGenRandom(NULL, out, (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0;
}
#else
#include <errno.h>
/* Bionic declares getrandom() only from API 28 and this ships against 26, so
 * on older Android the /dev/urandom path below is the one that runs. */
#if defined(__ANDROID__)
#if __ANDROID_API__ >= 28
#define MELEE_HAVE_GETRANDOM 1
#endif
#elif defined(__linux__)
#define MELEE_HAVE_GETRANDOM 1
#endif
#if defined(MELEE_HAVE_GETRANDOM)
#include <sys/random.h>
#endif
static bool csprng(void* out, size_t n) {
    size_t got = 0;
#if defined(MELEE_HAVE_GETRANDOM)
    while (got < n) {
        ssize_t r = getrandom((uint8_t*)out + got, n - got, 0);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        got += (size_t)r;
    }
    if (got == n) {
        return true;
    }
    got = 0;
#endif
    /* no getrandom (pre-3.17 kernel, a seccomp filter, or a BSD build
     * without it): the classic source, drawn from the same pool */
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        return false;
    }
    while (got < n) {
        ssize_t r = read(fd, (uint8_t*)out + got, n - got);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
        got += (size_t)r;
    }
    close(fd);
    return got == n;
}
#endif

#define HS_TIMEOUT_MS 15000
#define HS_LEAD_FRAMES 120 /* ponytail: 2 s for READY; a slower link misses the start */

static uint64_t s_hs_t0;
/* When a direct session was first seen waiting for the game's own rules to
 * fill in. Bounded, or a run that never fills them plays on unagreed. */
static uint64_t s_direct_idle_ns;
static bool s_rules_on; /* a RULES set is in force (host or guest) */
static bool s_rules_frozen;
static bool s_rules_saved; /* guest: s_rules_orig holds its own values */
static Rules s_rules_orig;
static uint64_t s_nonce_local;    /* ours this session; 0: not drawn yet */
static uint32_t s_nonce_session;  /* net.session s_nonce_local was drawn for */
static uint64_t s_nonce_peer;     /* theirs, from RULES (guest) or READY (host) */
static uint8_t s_remote_tag_bind; /* the peer's own MeleeVS: Tag Bind, see pc_net_remote_tag_bind */
/* This machine's own MeleeVS: Tag Bind, pinned at the moment it went on the
 * wire (RULES.tag_bind for the host, Ready.tag_bind for the guest) - see
 * pc_net_local_tag_bind. Without this, TagAssist_ExtraBindMask's local
 * branch read pc_get_tag_bind(0) live: changing the F1 menu's Tag Bind
 * setting mid-session took effect on THIS machine immediately, while the
 * peer kept simulating this same port with whatever s_remote_tag_bind it
 * cached at handshake time - the instant the two masks disagreed on the
 * next button press, TagAssist_TryTag/TryCallAssist fired on only one side.
 * Confirmed root cause of a real mid-session DESYNC (two machines' state
 * logs showed different fighters transitioning to different action states
 * on the exact same synced frame, right after a live Tag Bind change). */
static uint8_t s_local_tag_bind;
/* Couch partners (MeleeVS duo on one machine): NET_NO_PARTNER or that
 * player's Tag Bind, pinned when it goes on the wire like the binds above
 * (Rules/Ready.partner_bind). Whether a partner exists decides whether
 * write_head (net.c) feeds that machine's second pad to port local/remote+2,
 * so both peers must hold the same pair; the handshake is what makes it so. */
static uint8_t s_local_partner_bind = NET_NO_PARTNER;
static uint8_t s_remote_partner_bind = NET_NO_PARTNER;
/* MeleeVS Matchmaking. The game sets s_want_* before connecting; the host's
 * RULES decides the session's layout, and each machine's team (indexed by
 * player number, net.local / net.remote) is pinned from RULES and READY.
 * Matchmaking always hosts on player 0 (net_match.c), so player 0's team is
 * the host's: red, ports 1-2. */
static bool s_want_matchmade;
static PcNetTeam s_want_team;
static uint8_t s_layout = NET_LAYOUT_DIRECT;
static PcNetTeam s_team[2];
static bool s_unlock_saved; /* s_unlock_orig holds what the player had */
static uint64_t s_unlock_orig;
/* One log line per refusal class per session (the log-line rule): a peer, or
 * a stale process at its address, that keeps resending must not flood it. */
enum {
    LOG_RULES_HOST = 1 << 0,
    LOG_RULES_LEN = 1 << 1,
    LOG_RULES_DUP = 1 << 2,
    LOG_RULES_CONFLICT = 1 << 3,
    LOG_READY_GUEST = 1 << 4,
    LOG_READY_DONE = 1 << 5,
    LOG_READY_IDLE = 1 << 6,
    LOG_READY_LEN = 1 << 7,
    LOG_READY_HASH = 1 << 8,
    LOG_READY_NONCE = 1 << 9,
    LOG_RULES_INVALID = 1 << 10,
    LOG_RULES_LATE = 1 << 11,
    LOG_RULES_UNLOCK = 1 << 12,
    LOG_RULES_NONCE = 1 << 13,
};
static uint32_t s_hs_logged; /* LOG_* classes already logged this session */
/* A RULES or READY the reliable lane had no room for, kept in wire order so
 * hs_poll() can put it out on a later frame. One slot is enough: neither
 * side has two handshake messages to send, and the lane is the handshake's
 * own (types < 0x10), so only an unacked predecessor can fill it. */
static struct {
    uint32_t session; /* the net.session it was built for */
    uint8_t type;
    uint8_t len; /* 0: nothing pending */
    uint8_t wire[sizeof(Rules)];
} s_hs_tx;

uint32_t pc_net_seed(void) {
    return net.seed;
}

int pc_net_handshake_state(void) {
    return net.hs;
}

static void hs_done(void) {
    net.hs = HS_DONE;
    net.ck_from = net.start_frame;
    net.desync_reported = false; /* anything before start_frame was the lobbies differing */
    pc_log_line("net: handshake done seed=%u start_frame=%d (frame %d) partners local=%s remote=%s",
        net.seed, net.start_frame, net.tick_frame,
        s_local_partner_bind != NET_NO_PARTNER ? "yes" : "no",
        s_remote_partner_bind != NET_NO_PARTNER ? "yes" : "no");
}

/* Queue a handshake message, keeping it for a retry when the reliable lane
 * refuses it (net_reliable.c, queue full). A refused send used to be logged
 * and then treated as sent, which on the guest meant a "handshake done" the
 * host never heard about: it waited out its 15 s timeout while this side
 * believed the match was agreed. Same pattern as resume_send()/resume_poll()
 * in net.c -- the poll the caller already runs every frame is the retry. */
static bool hs_send(uint8_t type, const void* wire, int len) {
    if (pc_net_send_reliable(type, wire, len)) {
        s_hs_tx.len = 0;
        return true;
    }
    s_hs_tx.session = net.session;
    s_hs_tx.type = type;
    s_hs_tx.len = (uint8_t)len;
    memcpy(s_hs_tx.wire, wire, (size_t)len);
    pc_log_line("net: %s not queued (reliable queue full), retrying",
        type == REL_RULES ? "RULES" : "READY");
    return false;
}

/* Retry whatever hs_send() could not queue; called from hs_poll() while the
 * handshake is pending, so the handshake timeout bounds the retries. A READY
 * that goes out here is what completes the guest's handshake, which is why
 * hs_done() is reached from here rather than from on_rules(). */
static void hs_flush(void) {
    if (s_hs_tx.len == 0) {
        return;
    }
    if (s_hs_tx.session != net.session) {
        s_hs_tx.len = 0; /* built for a session that has since been torn down */
        return;
    }
    if (!pc_net_send_reliable(s_hs_tx.type, s_hs_tx.wire, s_hs_tx.len)) {
        return;
    }
    uint8_t type = s_hs_tx.type;
    s_hs_tx.len = 0;
    pc_log_line("net: %s queued on retry", type == REL_RULES ? "RULES" : "READY");
    if (type == REL_READY) {
        hs_done();
    }
}

/* The match-affecting part of RULES, from (capture) or into (apply) the
 * game's own copies. */
/* This machine's couch partner as it goes on the wire: only in Tag Battle
 * (a plain VS session stays two players), and only with a second controller
 * plugged in. Its bind is the partner's own, pc_get_tag_bind(1). */
static uint8_t local_partner_now(void) {
    if (!TagAssist_IsTagBattleOn() || !net_local_partner_present()) {
        return NET_NO_PARTNER;
    }
    return (uint8_t)pc_get_tag_bind(1);
}

/* A team as it goes on the wire: fighter[0] is always the player, fighter[1]
 * is human exactly when this machine announced a couch partner, and a
 * human+CPU team starts with the human on point (the CSS's own rule, since
 * TagAssist hands the one controller to whichever fighter is point). */
static PcNetTeam team_for_wire(uint8_t partner_bind) {
    static const int8_t fallback[2] = {CKind_Fox, CKind_Falco};
    PcNetTeam t = s_want_team;
    /* Always a valid team: our own RULES must pass rules_values_invalid
     * too (rules_ready), so a stale or empty setup falls back rather than
     * stalling the handshake. */
    for (int i = 0; i < 2; i++) {
        PcNetTeamFighter* f = &t.fighter[i];
        if (f->ckind < 0 || f->ckind >= CKind_Playable_Count) {
            f->ckind = fallback[i];
            f->color = 0;
        }
        if (f->color >= gm_GetNumCostumesForCKind((u8)f->ckind)) {
            f->color = 0;
        }
    }
    t.fighter[0].human = 1;
    t.fighter[1].human = partner_bind != NET_NO_PARTNER;
    for (int i = 0; i < 2; i++) {
        if (t.fighter[i].human) {
            t.fighter[i].cpu_level = 0;
        } else if (t.fighter[i].cpu_level < 1 || t.fighter[i].cpu_level > 9) {
            t.fighter[i].cpu_level = 9;
        }
    }
    if (!t.fighter[1].human) {
        t.point = 0;
    }
    return t;
}

/* The peer's team, checked the same way: its shape follows from its own
 * partner_bind, so a disagreement is a corrupt or forged team. */
static bool team_invalid(const PcNetTeam* t, uint8_t partner_bind) {
    if (t->point > 1 || t->fighter[0].human != 1 ||
        t->fighter[1].human != (partner_bind != NET_NO_PARTNER) ||
        (!t->fighter[1].human && t->point != 0))
    {
        return true;
    }
    for (int i = 0; i < 2; i++) {
        const PcNetTeamFighter* f = &t->fighter[i];
        if (f->ckind < 0 || f->ckind >= CKind_Playable_Count ||
            f->color >= gm_GetNumCostumesForCKind((u8)f->ckind) ||
            (f->human ? f->cpu_level != 0 : f->cpu_level < 1 || f->cpu_level > 9))
        {
            return true;
        }
    }
    return false;
}

/* MeleeVS Matchmaking always plays one ruleset, whatever either player's
 * own settings are: Stock, 3 lives, 8-minute stock timer, items off, team
 * attack off. The host sends it; the guest refuses anything else. */
#define MM_STOCKS 3
#define MM_STOCK_MINUTES 8
#define MM_ITEMS_OFF 0xFF /* mnItemSw_CommitItems' x21 - 1 for "Off" */
static void matchmade_rules(Rules* ru) {
    ru->game.mode = Mode_Stock;
    ru->game.stock_count = MM_STOCKS;
    ru->game.stock_time_limit = MM_STOCK_MINUTES;
    ru->game.friendly_fire = 0;
    ru->item_freq = MM_ITEMS_OFF;
}

static bool matchmade_rules_differ(const Rules* ru) {
    return ru->game.mode != Mode_Stock || ru->game.stock_count != MM_STOCKS ||
           ru->game.stock_time_limit != MM_STOCK_MINUTES || ru->game.friendly_fire != 0 ||
           ru->item_freq != MM_ITEMS_OFF;
}

/* The game's own settings as they are, with no session overrides: what
 * rules_restore() puts back. */
static void rules_capture_own(Rules* ru) {
    const struct GamePrefs* p = gmMainLib_GetGamePrefs();
    ru->game = *gmMainLib_GetGameRules();
    ru->item_freq = p->item_freq;
    ru->item_mask = p->item_mask;
    ru->stage_mask = p->stage_mask;
    ru->frozen_stadium = pc_is_frozen_stadium_enabled();
    ru->game_mode = TagAssist_IsTagBattleOn() ? GAME_MODE_TAG_BATTLE : GAME_MODE_VS;
    /* Port 0 always: capture_local_sample (net.c) always reads the local
     * human's real controller from physical port 0, whichever net-session
     * port (0 or 1) matchmaking assigns this peer. */
    ru->tag_bind = (uint8_t)pc_get_tag_bind(0);
    ru->partner_bind = local_partner_now();
    ru->layout = NET_LAYOUT_DIRECT;
}

/* What this side would put in RULES: its own settings, the session layout,
 * its team, and for Matchmaking the fixed ruleset. */
static void rules_capture(Rules* ru) {
    rules_capture_own(ru);
    ru->layout =
        s_want_matchmade && TagAssist_IsTagBattleOn() ? NET_LAYOUT_MATCHMADE : NET_LAYOUT_DIRECT;
    ru->team = team_for_wire(ru->partner_bind);
    if (ru->layout == NET_LAYOUT_MATCHMADE) {
        matchmade_rules(ru);
    }
}

static void rules_apply(const Rules* ru, bool from_peer) {
    struct GamePrefs* p = gmMainLib_GetGamePrefs();
    /* Keep our own settings to put back: the guest always overwrites them
     * with the host's, and a Matchmaking host overwrites its own with the
     * fixed ruleset. */
    if ((from_peer || ru->layout == NET_LAYOUT_MATCHMADE) && !s_rules_saved) {
        rules_capture_own(&s_rules_orig);
        s_rules_saved = true;
    }
    *gmMainLib_GetGameRules() = ru->game;
    p->item_freq = ru->item_freq;
    p->item_mask = ru->item_mask;
    p->stage_mask = ru->stage_mask;
    s_rules_frozen = ru->frozen_stadium != 0;
    s_rules_on = true;
    /* The host's game_mode is authoritative for both peers, same as every
     * other field here - a guest whose local menu choice somehow drifted
     * (stale state from a prior session, a race with the compatibility
     * check) ends up exactly matching the host rather than desyncing CSS. */
    if (ru->game_mode == GAME_MODE_TAG_BATTLE) {
        TagAssist_EnterForcedOn();
    } else {
        TagAssist_LeaveTagBattle();
    }
    if (from_peer) {
        /* The guest learning the host's own tag_bind; the host learns the
         * guest's symmetric way, from Ready.tag_bind in on_ready below. */
        s_remote_tag_bind = ru->tag_bind;
        s_remote_partner_bind = ru->partner_bind;
    }
    /* The host's layout and team, whichever side is applying them. */
    s_layout = ru->layout;
    s_team[net.hs_host ? net.local : net.remote] = ru->team;
    pc_log_line("net: RULES %s mode=%u time=%u stock=%u handicap=%u dmg=%u stage_sel=%u ff=%u "
                "pause=%u sd=%u items=%u/%016llx stages=%08x frozen=%u unlock_all=1 game_mode=%u",
        from_peer ? "applied" : "in force", ru->game.mode, ru->game.time_limit,
        ru->game.stock_count, ru->game.handicap, ru->game.damage_ratio, ru->game.stage_sel,
        ru->game.friendly_fire, ru->game.pause, ru->game.unk_xc, ru->item_freq,
        (unsigned long long)ru->item_mask, ru->stage_mask, ru->frozen_stadium, ru->game_mode);
}

/* ---- unlock state -----------------------------------------------------
 * Unlock progress is per-install save data (or, with no card, whatever the
 * card-absent default builder wrote), and several unlock predicates gate
 * HSD_Rand draws: gm_80164ABC gates the alternate-BGM HSD_Randi(100) roll
 * reached from ground.c case 6, and gmMainLib_8015ECBC gates an
 * HSD_Randi(4). One peer drawing where the other does not is a one-draw
 * seed divergence that shows up as a desync on the first frame that
 * advances fighter state, with every fighter field still bit-identical.
 *
 * So the session pins the whole unlock surface: pc_unlock_state_all() is
 * written into the real masks before RULES is built, which is what makes
 * every reader agree - the three predicates with a pc_is_unlock_all_enabled
 * short circuit, gm_80164600 which has none, and the direct mask readers in
 * gm_1601.c and gm_16F1.c that bypass any read-time override. The state
 * each side actually ended up with is hashed onto the wire and compared, so
 * two builds that disagree about what "all unlocked" means refuse the
 * handshake instead of desyncing mid-match.
 *
 * These masks are the RAM save image, and rules_restore() puts the player's
 * own back at disconnect. Nothing reaches the card in between: the only
 * flush is lbCardGame_SaveChanges(), whose callers are all single-player
 * mode-end paths (gm_17C0.c, gmmultiman.c, gmhomerun.c, gmscmemcard.c,
 * tyfigupon.c), so a VS session never saves and a crash mid-session loses
 * only the forced value, never the card.
 * ponytail: that is "no VS path saves today", not an enforced invariant. A
 * save trigger added to a VS path must run after rules_restore(), or the
 * flush has to be suppressed while s_unlock_saved is set. */
static void unlock_force(void) {
    if (s_unlock_saved) {
        return; /* already pinned for this session */
    }
    s_unlock_orig = pc_unlock_state_get();
    s_unlock_saved = true;
    pc_unlock_state_set(pc_unlock_state_all());
    pc_log_line("net: unlock forced %016llx (was %016llx)",
        (unsigned long long)pc_unlock_state_get(), (unsigned long long)s_unlock_orig);
}

static void unlock_restore(void) {
    if (s_unlock_saved) {
        s_unlock_saved = false;
        pc_unlock_state_set(s_unlock_orig);
        pc_log_line("net: unlock state restored (%016llx)", (unsigned long long)s_unlock_orig);
    }
}

/* Hash of the unlock state as read back out of the save data, not of the
 * constant we asked for: what goes on the wire is what the readers will
 * see. Folded into the RULES/READY hash discipline by riding inside their
 * wire images, so tampering with it fails the payload hash first. */
static uint32_t unlock_hash_now(void) {
    uint64_t s = pc_unlock_state_get();
    uint8_t be[8];
    for (int i = 0; i < 8; i++) {
        be[i] = (uint8_t)(s >> (56 - 8 * i));
    }
    return fnv1a(2166136261u, be, sizeof be);
}

void rules_restore(void) {
    unlock_restore();
    if (s_rules_saved) {
        s_rules_saved = false;
        rules_apply(&s_rules_orig, false);
        pc_log_line("net: RULES restored own settings");
    }
    s_rules_on = false;
    /* The session is over; its nonces must never be reused, anything it
     * still had to send dies with it, and the next one gets a fresh log
     * budget for each refusal class. */
    s_nonce_local = s_nonce_peer = 0;
    s_remote_tag_bind = 0;
    s_local_tag_bind = 0;
    s_remote_partner_bind = NET_NO_PARTNER;
    s_local_partner_bind = NET_NO_PARTNER;
    s_layout = NET_LAYOUT_DIRECT;
    memset(s_team, 0, sizeof s_team);
    s_hs_tx.len = 0;
    s_hs_logged = 0;
    s_direct_idle_ns = 0;
}

bool pc_net_rules(bool* unlock_all, bool* frozen_stadium) {
    if (!s_rules_on) {
        return false;
    }
    *unlock_all = true;
    *frozen_stadium = s_rules_frozen;
    return true;
}

int pc_net_remote_tag_bind(void) {
    return s_rules_on ? s_remote_tag_bind : 0;
}

/* This machine's own MeleeVS: Tag Bind, pinned for the session - see
 * s_local_tag_bind's own comment for why a live pc_get_tag_bind(0) read is
 * not safe to use in its place while a session is active. Falls back to a
 * live read before a session exists (s_local_tag_bind is only ever pinned
 * once RULES/READY actually go on the wire), matching what a live read
 * would have returned anyway with nothing to disagree with yet. */
int pc_net_local_tag_bind(void) {
    return s_rules_on ? s_local_tag_bind : pc_get_tag_bind(0);
}

int pc_net_local_partner_bind(void) {
    return s_rules_on && s_local_partner_bind != NET_NO_PARTNER ? s_local_partner_bind : -1;
}

int pc_net_remote_partner_bind(void) {
    return s_rules_on && s_remote_partner_bind != NET_NO_PARTNER ? s_remote_partner_bind : -1;
}

void pc_net_set_matchmade(bool matchmade, const PcNetTeam* team) {
    s_want_matchmade = matchmade && team != NULL;
    if (team != NULL) {
        s_want_team = *team;
    } else {
        memset(&s_want_team, 0, sizeof s_want_team);
    }
}

bool pc_net_matchmade(void) {
    return s_rules_on && s_layout == NET_LAYOUT_MATCHMADE;
}

const PcNetTeam* pc_net_team(int machine) {
    return pc_net_matchmade() && machine >= 0 && machine < 2 ? &s_team[machine] : NULL;
}

int pc_net_game_port(int machine, int slot) {
    return pc_net_matchmade() ? machine * 2 + slot : machine + 2 * slot;
}

bool pc_net_port_human(int port) {
    for (int machine = 0; machine < 2; machine++) {
        if (port == pc_net_game_port(machine, 0)) {
            return true;
        }
        if (port == pc_net_game_port(machine, 1)) {
            return (machine == net.local ? pc_net_local_partner_bind() :
                                           pc_net_remote_partner_bind()) >= 0;
        }
    }
    return false;
}

/* The value ranges a RULES set has to be inside. Split out because
 * rules_ready() below asks the same question about our own copies: the game
 * zeroes them at boot and fills them in during it, and a zeroed set fails
 * these exactly as a corrupt one does. */
static const char* rules_values_invalid(const Rules* ru) {
    /* item_freq is u8 "x21 - 1" (mnItemSw_CommitItems, mnitemsw.c): the UI's
     * first entry (x21 == 0, "Off") underflows to 0xFF, not 0 - a completely
     * ordinary Items: Off rules choice, not a corrupt or forged field. 0-4
     * cover Very Low..Very High. */
    if (ru->game.mode > 3 || ru->game.time_limit > 99 || ru->game.stock_count > 99 ||
        ru->game.damage_ratio < 5 || ru->game.damage_ratio > 20 ||
        (ru->item_freq > 4 && ru->item_freq != 0xFF) || ru->stage_mask == 0 ||
        ru->game_mode > GAME_MODE_TAG_BATTLE ||
        ru->tag_bind > 11 /* kTagBindNames/kTagBind* have 12 entries, see pc_get_tag_bind */ ||
        (ru->partner_bind > 11 && ru->partner_bind != NET_NO_PARTNER) ||
        (ru->partner_bind != NET_NO_PARTNER && ru->game_mode != GAME_MODE_TAG_BATTLE) ||
        ru->layout > NET_LAYOUT_MATCHMADE ||
        (ru->layout == NET_LAYOUT_MATCHMADE &&
            (ru->game_mode != GAME_MODE_TAG_BATTLE || team_invalid(&ru->team, ru->partner_bind) ||
                matchmade_rules_differ(ru))))
    {
        return "value out of range";
    }
    return NULL;
}

/* What a RULES set must look like before it is applied, NULL when fine. The
 * hash binds the session id (rules_hash, net_wire.c), so a RULES captured
 * off an earlier session between the same two peers fails here instead of
 * replaying into this one. */
static const char* rules_invalid(const Rules* ru) {
    if (ru->hash != rules_hash(*ru, net.session)) {
        return "hash mismatch";
    }
    if (ru->nonce == 0) {
        return "no nonce"; /* the sender's CSPRNG failed, or a forgery */
    }
    if (ru->start_frame < 0 || ru->start_frame > net.tick_frame + RING * 4) {
        return "start_frame out of range";
    }
    return rules_values_invalid(ru);
}

static void hs_drop(uint32_t cls, const char* what, const char* why) {
    if ((s_hs_logged & cls) == 0) {
        s_hs_logged |= cls;
        pc_log_line("net: %s ignored (%s)", what, why);
    }
}

/* The agreed seed, applied as soon as this side learns it. Both peers also
 * apply it entering start_frame (net.c), which is the frame that actually
 * lines their RNG streams up: the two sides learn it a leg apart, so this
 * earlier write is only there to have the seed in place for whatever a lobby
 * draws on its way to the CSS, and the lobbies run different states until
 * start_frame anyway.
 *
 * A direct session is the opposite case: its peers have been running the
 * same boot, frame for frame, since frame 0, so writing the seed on two
 * different frames is not a repair but the very divergence the handshake
 * exists to prevent -- one side's draws restart from the seed while the
 * other's carry on. So it waits for start_frame, which both reach on the
 * same frame. */
static void seed_apply(void) {
    if (!net.direct) {
        *HSD_RandSeedPtr = net.seed;
    }
}

/* Our nonce for this session, drawn on first use: RULES can land before the
 * lobby calls pc_net_guest_wait_match. Keyed on net.session so a second
 * session can never inherit the first one's nonce, whatever order connect,
 * disconnect and the lobby run in. 0 means the CSPRNG failed and the
 * handshake must not proceed.
 * ponytail: 0 doubles as "not drawn yet", so an all-zero draw (2^-64) is
 * simply drawn again on the next call. */
static uint64_t nonce_local(void) {
    if (s_nonce_session != net.session) {
        s_nonce_session = net.session;
        s_nonce_local = 0;
    }
    if (s_nonce_local == 0 && !csprng(&s_nonce_local, sizeof s_nonce_local)) {
        s_nonce_local = 0;
    }
    return s_nonce_local;
}

static void on_rules(const uint8_t* payload, int len) {
    if (net.hs_host) {
        hs_drop(LOG_RULES_HOST, "RULES", "we host");
        return;
    }
    if (len != (int)sizeof(Rules)) {
        hs_drop(LOG_RULES_LEN, "RULES", "wrong length");
        return;
    }
    Rules ru;
    memcpy(&ru, payload, sizeof ru);
    wire_rules(&ru);
    if (net.hs == HS_DONE) {
        /* A plain retransmit never reaches here (the reliable lane dedups by
         * sequence, net_reliable.c), so this is a second, distinct RULES:
         * either the host changed its mind too late or someone injected it.
         * Logged once and dropped; the rules in force do not move. */
        hs_drop(ru.nonce == s_nonce_peer ? LOG_RULES_DUP : LOG_RULES_CONFLICT, "RULES",
            ru.nonce == s_nonce_peer ? "already applied" : "conflicting nonce after done");
        return;
    }
    const char* bad = rules_invalid(&ru);
    if (bad != NULL) {
        /* Drop, never HS_FAILED: before the READY authenticates, anyone on
         * the path can shape a RULES that fails these checks, and one forged
         * datagram must not be able to kill the handshake. The genuine
         * host's reliable lane retransmits (the dropped one is never acked),
         * and a genuine persistent failure still ends in the 15 s timeout. */
        hs_drop(LOG_RULES_INVALID, "RULES", bad);
        return;
    }
    if (ru.start_frame <= net.tick_frame) {
        hs_drop(LOG_RULES_LATE, "RULES", "start_frame already reached");
        return;
    }
    /* Pin our unlock surface, then check the host's came out the same. Both
     * sides write the same constant, so a mismatch means the two builds
     * disagree about what "all unlocked" is - one peer would run with a
     * reader still saying "locked", which is a silent mid-match seed
     * divergence. Refuse instead (local://UnlockSync-doc.md).
     * ponytail: refusing sends no READY, so the host only finds out from its
     * own 15 s timeout. Sending a READY and then failing would cut that to
     * one RTT, but it would make the guest's refusal depend on the host
     * checking too; a NAK message type is the fix if this path ever stops
     * being one-in-never. */
    unlock_force();
    uint32_t unlock_mine = unlock_hash_now();
    if (unlock_mine != ru.unlock_hash) {
        hs_drop(LOG_RULES_UNLOCK, "RULES", "unlock state mismatch");
        unlock_restore();
        return;
    }
    s_local_tag_bind = (uint8_t)pc_get_tag_bind(0);
    /* The host's game_mode decides Tag Battle for both sides, so ask the
     * RULES being answered, not this machine's own menu state. */
    s_local_partner_bind = ru.game_mode == GAME_MODE_TAG_BATTLE && net_local_partner_present() ?
                               (uint8_t)pc_get_tag_bind(1) :
                               NET_NO_PARTNER;
    Ready rd = {nonce_local(), ru.nonce, unlock_mine, s_local_tag_bind, s_local_partner_bind,
        team_for_wire(s_local_partner_bind), 0};
    s_team[net.local] = rd.team;
    if (rd.nonce == 0) {
        hs_drop(LOG_RULES_NONCE, "RULES", "no random source");
        unlock_restore();
        return;
    }
    rd.hash = ready_hash(rd, net.session);
    s_nonce_peer = ru.nonce;
    net.seed = ru.seed;
    net.start_frame = ru.start_frame;
    seed_apply();
    /* The earliest moment this side holds both nonces, so the earliest one
     * at which its datagrams can be authenticated: before the READY that
     * carries our nonce has even gone out. The host cannot check a tag until
     * that READY lands, which is why the receiver treats an unverifiable
     * datagram as a peer that has not keyed yet until the first one that
     * does verify (recv_inputs). Under tx_lock because the 4 ms timer stamps
     * its resends with this key on its own thread. */
    SDL_LockMutex(net.tx_lock);
    net_key_session(ru.nonce, rd.nonce);
    SDL_UnlockMutex(net.tx_lock);
    rules_apply(&ru, true);
    wire_ready(&rd);
    if (!hs_send(REL_READY, &rd, sizeof rd)) {
        return; /* hs_poll() retries it, and finishes the handshake when it goes out */
    }
    hs_done();
}

static void on_ready(const uint8_t* payload, int len) {
    if (!net.hs_host) {
        hs_drop(LOG_READY_GUEST, "READY", "we are the guest");
        return;
    }
    if (net.hs == HS_DONE) {
        hs_drop(LOG_READY_DONE, "READY", "already done");
        return;
    }
    if (net.hs != HS_PENDING) {
        hs_drop(LOG_READY_IDLE, "READY", "no handshake pending");
        return;
    }
    if (len != (int)sizeof(Ready)) {
        hs_drop(LOG_READY_LEN, "READY", "wrong length");
        return;
    }
    Ready rd;
    memcpy(&rd, payload, sizeof rd);
    wire_ready(&rd);
    /* A READY is ours only if it hashes under this session id and echoes the
     * nonce we put in RULES; a capture from any earlier session fails both,
     * and an off-path forgery has to guess 64 bits. Refusals are dropped,
     * not failed, so an injected READY cannot end a live handshake - the
     * genuine one still arrives, or the 15 s timeout fires. */
    if (rd.hash != ready_hash(rd, net.session)) {
        hs_drop(LOG_READY_HASH, "READY", "hash mismatch");
        return;
    }
    if (s_nonce_local == 0 || rd.echo != s_nonce_local) {
        hs_drop(LOG_READY_NONCE, "READY", "echoed nonce mismatch");
        return;
    }
    /* The guest's unlock hash rides inside the image the two checks above
     * just authenticated, so a mismatch here is a real disagreement rather
     * than an injection: fail hard instead of waiting out the timeout. */
    if (rd.unlock_hash != unlock_hash_now()) {
        pc_log_line("net: READY rejected: unlock state mismatch (ours %08x/%016llx, guest %08x)",
            unlock_hash_now(), (unsigned long long)pc_unlock_state_get(), rd.unlock_hash);
        net.hs = HS_FAILED;
        return;
    }
    s_nonce_peer = rd.nonce;
    /* Authenticated like unlock_hash above, so a bad value is a real
     * disagreement: fail hard rather than wait out the timeout. */
    if (rd.partner_bind != NET_NO_PARTNER && (rd.partner_bind > 11 || !TagAssist_IsTagBattleOn())) {
        pc_log_line("net: READY rejected: partner_bind %u out of range", rd.partner_bind);
        net.hs = HS_FAILED;
        return;
    }
    if (s_layout == NET_LAYOUT_MATCHMADE && team_invalid(&rd.team, rd.partner_bind)) {
        pc_log_line("net: READY rejected: team setup invalid");
        net.hs = HS_FAILED;
        return;
    }
    s_remote_tag_bind = rd.tag_bind; /* the host learning the guest's own bind */
    s_remote_partner_bind = rd.partner_bind;
    s_team[net.remote] = rd.team;
    /* This side's turn: the READY just authenticated is what carries the
     * guest's nonce, so from here both peers hold the same three values and
     * derive the same key without another message. Under tx_lock, like the
     * guest's install above. */
    SDL_LockMutex(net.tx_lock);
    net_key_session(s_nonce_local, rd.nonce);
    SDL_UnlockMutex(net.tx_lock);
    hs_done();
}

/* Reliable types below 0x10: the match handshake. */
void handshake_msg(uint8_t type, const uint8_t* payload, int len) {
    if (type == REL_RULES) {
        on_rules(payload, len);
    } else if (type == REL_READY) {
        on_ready(payload, len);
    }
}

/* Drive the pending handshake a step; true once done, with start_frame. */
static bool hs_poll(int32_t* start_frame) {
    if (net.hs == HS_PENDING) {
        recv_inputs();
        hs_flush(); /* after recv_inputs: a 'K' just read may be what freed the slot */
        /* Re-checked: recv_inputs() or the flush above may have just finished
         * the handshake, and a done one is not timed out. */
        if (net.hs == HS_PENDING && SDL_GetTicksNS() - s_hs_t0 > HS_TIMEOUT_MS * 1000000ull) {
            net.hs = HS_FAILED;
            pc_log_line("net: handshake timed out, no %s", net.hs_host ? "READY" : "RULES");
        }
    }
    if (net.hs != HS_DONE) {
        return false;
    }
    *start_frame = net.start_frame;
    return true;
}

bool pc_net_host_match(uint32_t seed, int32_t* start_frame) {
    if (net.hs == HS_IDLE) {
        if (!net.active) {
            return false;
        }
        net.hs_host = true;
        if (nonce_local() == 0) {
            /* No CSPRNG: refuse rather than send a guessable nonce. */
            net.hs = HS_FAILED;
            pc_log_line("net: handshake failed, no random source");
            return false;
        }
        net.hs = HS_PENDING;
        s_hs_t0 = SDL_GetTicksNS();
        net.seed = seed;
        seed_apply();
        net.start_frame = net.tick_frame + HS_LEAD_FRAMES;
        Rules ru = {0};
        ru.seed = seed;
        ru.start_frame = net.start_frame;
        ru.nonce = s_nonce_local;
        rules_capture(&ru);
        s_local_tag_bind = ru.tag_bind;
        s_local_partner_bind = ru.partner_bind;
        unlock_force();
        ru.unlock_hash = unlock_hash_now();
        ru.hash = rules_hash(ru, net.session);
        rules_apply(&ru, false);
        wire_rules(&ru);
        if (hs_send(REL_RULES, &ru, sizeof ru)) {
            pc_log_line("net: RULES sent seed=%u start_frame=%d", seed, net.start_frame);
        }
    }
    return hs_poll(start_frame);
}

bool pc_net_guest_wait_match(uint32_t* seed, int32_t* start_frame) {
    if (net.hs == HS_IDLE) {
        if (!net.active) {
            return false;
        }
        net.hs = HS_PENDING; /* RULES may already have landed: then hs is DONE */
        net.hs_host = false;
        s_hs_t0 = SDL_GetTicksNS();
    }
    if (!hs_poll(start_frame)) {
        return false;
    }
    *seed = net.seed;
    return true;
}

/* ---- direct sessions ---------------------------------------------------
 * MELEE_NET=host:port has no lobby to agree the match on its behalf, and
 * until this ran it agreed nothing at all: two peers could start with
 * different rules, different unlock progress and different MELEE_SEED
 * values, and the transport would happily exchange inputs between two
 * simulations that were never going to agree. A direct session now runs
 * exactly the exchange above, so the same checks refuse the same
 * disagreements -- and, since both nonces then exist, its datagrams get a
 * derived key (net_key_session in on_rules/on_ready) instead of depending on
 * MELEE_NET_KEY.
 *
 * Who hosts costs no round trip: MELEE_NET_PLAYER already names the two
 * sides, and player 1 (net.local 0) is the host everywhere else in the
 * netcode -- it names the session id (net.c), the lobby connects its host as
 * player 0 (net_lan.c, net_match.c), and it announces the auto delay
 * (net_sync.c). The seed is the host's, whatever MELEE_SEED gave it, or its
 * own boot seed when that is unset: a direct session no longer needs
 * MELEE_SEED at all to have both peers drawing from the same stream.
 *
 * Not at connect, though. pc_net_init() runs from pc_platform_init(), before
 * the game installs gmMainLib_DefaultGameRules (gmmain.c:194) and before its
 * card data exists, so the rules captured there are still the zeroed struct
 * -- damage_ratio 0, stage_mask 0 -- which the guest's own rules_invalid()
 * refuses. The MELEE_NET_HANDSHAKE_TEST hook this replaces waited for frame
 * 300 for that reason; waiting for the values themselves is the same wait
 * without the magic number. The guest waits for its own copies too, so that
 * it does not start the 15 s timeout before the host could have sent
 * anything: menus never predict (net.c's in_fight()), so the two peers cross
 * that point within the input delay of each other.
 *
 * A refusal leaves net.hs at HS_FAILED, which pc_net_after_tick() turns into
 * a disconnect -- there is no lobby here to report it to. */
static bool rules_ready(void) {
    Rules ru = {0};
    rules_capture(&ru);
    return rules_values_invalid(&ru) == NULL;
}

void handshake_direct(void) {
    if (!net.direct || !net.active || net.hs == HS_DONE || net.hs == HS_FAILED) {
        s_direct_idle_ns = 0;
        return;
    }
    if (net.hs == HS_IDLE) {
        if (!rules_ready()) {
            /* The game has not filled its own rules in yet. Waiting for the
             * values rather than for a frame number is deliberate, but it has
             * to be bounded: a run that never fills them (a headless build, a
             * card-present prefs struct with stage_mask 0) used to sit here
             * silently while the session ran with net.start_frame == -1 and
             * each peer's own boot seed -- no agreement at all, and a desync
             * the moment the two seeds differ. A direct session has no lobby
             * to report that to, so it fails itself instead. */
            uint64_t now = SDL_GetTicksNS();
            if (s_direct_idle_ns == 0) {
                s_direct_idle_ns = now;
            } else if (now - s_direct_idle_ns > HS_TIMEOUT_MS * 1000000ull) {
                net.hs = HS_FAILED;
                pc_log_line("net: direct handshake never started; the local rules never "
                            "filled in (%d ms)",
                    HS_TIMEOUT_MS);
            }
            return;
        }
        s_direct_idle_ns = 0;
    }
    int32_t start_frame;
    if (net.local == 0) {
        pc_net_host_match(net.seed != 0 ? net.seed : *HSD_RandSeedPtr, &start_frame);
    } else {
        uint32_t seed;
        pc_net_guest_wait_match(&seed, &start_frame);
    }
}
