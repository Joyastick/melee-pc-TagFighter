/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "net_firewall.h"
#include "pc.h"

#ifdef _WIN32
#define COBJMACROS
#include <SDL3/SDL_atomic.h>
#include <SDL3/SDL_thread.h>
#include <windows.h>
#include <netfw.h>
#include <shellapi.h>
#include <wchar.h>

/* HNetCfg.FwPolicy2 and its interfaces; MinGW's libuuid lacks them. */
static const CLSID clsid_fw_policy2 = {
    0xE2B3C97F, 0x6AE1, 0x41AC, {0x81, 0x7A, 0xF6, 0xF9, 0x21, 0x66, 0xD7, 0xDD}};
static const IID iid_fw_policy2 = {
    0x98325047, 0xC671, 0x4174, {0x8D, 0x81, 0xDE, 0xFC, 0xD3, 0xF0, 0x31, 0x86}};
static const IID iid_fw_rule = {
    0xAF230D27, 0xBABA, 0x4E42, {0xAC, 0xED, 0xF5, 0x24, 0xF2, 0x2C, 0xFC, 0xE2}};
static const IID iid_enum_variant = {
    0x00020404, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};

/* Whether inbound UDP reaches `exe` on every active network profile: the
 * firewall is off or lets everything in there, or an enabled inbound allow
 * rule for this very program covers it, and no block rule for it does
 * (block wins). Rules for "any program" are skipped: on Windows 11 those
 * are Store apps' package-scoped rules, which never apply to us. That is
 * also why INetFwMgr::IsPortAllowed is useless here: it counts them. */
static bool rules_allow(INetFwPolicy2* policy, const WCHAR* exe) {
    LONG active = 0;
    if (FAILED(INetFwPolicy2_get_CurrentProfileTypes(policy, &active)))
        return true;
    LONG need = 0;
    for (LONG p = 1; p <= 4; p <<= 1) {
        VARIANT_BOOL on = VARIANT_TRUE;
        NET_FW_ACTION def = NET_FW_ACTION_BLOCK;
        if (!(active & p) || FAILED(INetFwPolicy2_get_FirewallEnabled(policy, p, &on)) || !on ||
            FAILED(INetFwPolicy2_get_DefaultInboundAction(policy, p, &def)) ||
            def == NET_FW_ACTION_ALLOW)
            continue;
        need |= p;
    }
    if (!need)
        return true;
    INetFwRules* rules = NULL;
    IUnknown* unknown = NULL;
    IEnumVARIANT* it = NULL;
    LONG allowed = 0, blocked = 0;
    if (SUCCEEDED(INetFwPolicy2_get_Rules(policy, &rules)) &&
        SUCCEEDED(INetFwRules_get__NewEnum(rules, &unknown)) &&
        SUCCEEDED(IUnknown_QueryInterface(unknown, &iid_enum_variant, (void**)&it)))
    {
        VARIANT v;
        VariantInit(&v);
        while (IEnumVARIANT_Next(it, 1, &v, NULL) == S_OK) {
            INetFwRule* rule = NULL;
            if (v.vt == VT_DISPATCH && v.pdispVal &&
                SUCCEEDED(IDispatch_QueryInterface(v.pdispVal, &iid_fw_rule, (void**)&rule)))
            {
                NET_FW_RULE_DIRECTION dir = NET_FW_RULE_DIR_OUT;
                VARIANT_BOOL enabled = VARIANT_FALSE;
                NET_FW_ACTION action = NET_FW_ACTION_BLOCK;
                LONG profiles = 0, protocol = 0;
                BSTR app = NULL;
                if (SUCCEEDED(INetFwRule_get_Direction(rule, &dir)) && dir == NET_FW_RULE_DIR_IN &&
                    SUCCEEDED(INetFwRule_get_Enabled(rule, &enabled)) && enabled &&
                    SUCCEEDED(INetFwRule_get_Protocol(rule, &protocol)) &&
                    (protocol == NET_FW_IP_PROTOCOL_UDP || protocol == 256 /* any */) &&
                    SUCCEEDED(INetFwRule_get_ApplicationName(rule, &app)) && app &&
                    SUCCEEDED(INetFwRule_get_Action(rule, &action)) &&
                    SUCCEEDED(INetFwRule_get_Profiles(rule, &profiles)))
                {
                    WCHAR path[MAX_PATH];
                    DWORD n = ExpandEnvironmentStringsW(app, path, MAX_PATH);
                    if (n > 0 && n <= MAX_PATH && !_wcsicmp(path, exe)) {
                        if (action == NET_FW_ACTION_ALLOW)
                            allowed |= profiles;
                        else
                            blocked |= profiles;
                    }
                }
                SysFreeString(app);
                INetFwRule_Release(rule);
            }
            VariantClear(&v);
        }
    }
    if (it)
        IEnumVARIANT_Release(it);
    if (unknown)
        IUnknown_Release(unknown);
    if (rules)
        INetFwRules_Release(rules);
    return !(blocked & need) && (allowed & need) == need;
}

static SDL_AtomicInt state;        /* PC_FIREWALL_OPEN (0) until checked */
static SDL_AtomicInt checked_open; /* a check found it open */

static bool exe_path(WCHAR out[MAX_PATH]) {
    DWORD n = GetModuleFileNameW(NULL, out, MAX_PATH);
    return n > 0 && n < MAX_PATH;
}

/* On its own thread so COM gets an apartment of ours, whatever SDL or the
 * GPU backend set up on the main thread. Any failure counts as open: a
 * guess must never put a prompt in front of the player. */
static int check_thread(void* unused) {
    (void)unused;
    WCHAR exe[MAX_PATH];
    bool open = true;
    if (!exe_path(exe))
        return open;
    HRESULT init = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    INetFwPolicy2* policy = NULL;
    if (SUCCEEDED(CoCreateInstance(
            &clsid_fw_policy2, NULL, CLSCTX_INPROC_SERVER, &iid_fw_policy2, (void**)&policy)))
    {
        open = rules_allow(policy, exe);
        INetFwPolicy2_Release(policy);
    }
    if (SUCCEEDED(init))
        CoUninitialize();
    return open;
}

static bool check_now(void) {
    int open = 1;
    SDL_Thread* t = SDL_CreateThread(check_thread, "firewall-check", NULL);
    if (t)
        SDL_WaitThread(t, &open);
    return open != 0;
}

int pc_firewall_check(void) {
    int s = SDL_GetAtomicInt(&state);
    if (SDL_GetAtomicInt(&checked_open) || s == PC_FIREWALL_ASKING)
        return s;
    bool open = check_now();
    SDL_SetAtomicInt(&checked_open, open);
    s = open                    ? PC_FIREWALL_OPEN :
        s == PC_FIREWALL_FAILED ? PC_FIREWALL_FAILED :
                                  PC_FIREWALL_BLOCKED;
    SDL_SetAtomicInt(&state, s);
    pc_log_line("firewall: inbound play %s", open ? "allowed" : "blocked");
    return s;
}

int pc_firewall_state(void) {
    return SDL_GetAtomicInt(&state);
}

/* cmd /s /c "<a> & <b>": delete first, since a block rule (what Cancel on
 * Windows' own prompt leaves behind) wins over any allow rule. The delete
 * fails harmlessly when there is nothing to delete, so '&', not '&&'. */
static int allow_thread(void* unused) {
    (void)unused;
    WCHAR exe[MAX_PATH], args[3 * MAX_PATH + 256];
    bool ok = false;
    if (exe_path(exe)) {
        _snwprintf(args, sizeof args / sizeof *args,
            L"/s /c \"netsh advfirewall firewall delete rule name=all program=\"%ls\" & "
            L"netsh advfirewall firewall add rule name=\"MeleeVS\" dir=in action=allow "
            L"program=\"%ls\" enable=yes profile=any\"",
            exe, exe);
        args[sizeof args / sizeof *args - 1] = 0;
        HRESULT init = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei = {sizeof sei};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"runas";
        sei.lpFile = L"cmd.exe";
        sei.lpParameters = args;
        sei.nShow = SW_HIDE;
        if (ShellExecuteExW(&sei) && sei.hProcess) {
            DWORD code = 1;
            WaitForSingleObject(sei.hProcess, 30000);
            GetExitCodeProcess(sei.hProcess, &code);
            CloseHandle(sei.hProcess);
            pc_log_line("firewall: netsh exited %u", (unsigned)code);
        } else {
            pc_log_line(
                "firewall: admin prompt declined or failed (error %u)", (unsigned)GetLastError());
        }
        if (SUCCEEDED(init))
            CoUninitialize();
        ok = check_thread(NULL) != 0;
    }
    SDL_SetAtomicInt(&checked_open, ok);
    SDL_SetAtomicInt(&state, ok ? PC_FIREWALL_OPEN : PC_FIREWALL_FAILED);
    pc_log_line(
        "firewall: inbound play %s after the allow request", ok ? "allowed" : "still blocked");
    return 0;
}

void pc_firewall_allow(void) {
    if (SDL_GetAtomicInt(&state) == PC_FIREWALL_ASKING)
        return;
    SDL_SetAtomicInt(&state, PC_FIREWALL_ASKING);
    SDL_Thread* t = SDL_CreateThread(allow_thread, "firewall-allow", NULL);
    if (t)
        SDL_DetachThread(t);
    else
        SDL_SetAtomicInt(&state, PC_FIREWALL_FAILED);
}

#else

int pc_firewall_check(void) {
    return PC_FIREWALL_OPEN;
}
int pc_firewall_state(void) {
    return PC_FIREWALL_OPEN;
}
void pc_firewall_allow(void) {}

#endif
