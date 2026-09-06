// mgmp_loader_dialog.cpp -- see mgmp_loader_dialog.h for the design.
#include "mgmp_loader_dialog.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

#include "json.hpp"
#include "resource.h"

namespace mgmp_loader {
namespace {

struct DialogState {
    std::wstring path;          // full path to mgmp.json
    nlohmann::json doc;         // the whole file, so nothing else is lost on write
    bool           is_client = false;
    wchar_t        addr[64]   = L"127.0.0.1";
    wchar_t        port[8]    = L"27600";
    bool           dont_show  = false;
    bool           launched   = false;   // true only if IDOK was actually pressed
};

void set_addr_controls_enabled(HWND hwnd, bool enabled) {
    EnableWindow(GetDlgItem(hwnd, IDC_LABEL_ADDR), enabled);
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_ADDR),  enabled);
}

INT_PTR CALLBACK dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_INITDIALOG: {
            auto* st = (DialogState*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)st);

            CheckRadioButton(hwnd, IDC_RADIO_HOST, IDC_RADIO_CLIENT,
                             st->is_client ? IDC_RADIO_CLIENT : IDC_RADIO_HOST);
            SetDlgItemTextW(hwnd, IDC_EDIT_ADDR, st->addr);
            SetDlgItemTextW(hwnd, IDC_EDIT_PORT, st->port);
            CheckDlgButton(hwnd, IDC_CHECK_NOSHOW, st->dont_show ? BST_CHECKED : BST_UNCHECKED);
            set_addr_controls_enabled(hwnd, st->is_client);

            SetForegroundWindow(hwnd);
            return TRUE;
        }

        case WM_COMMAND: {
            auto* st = (DialogState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            const WORD id = LOWORD(wp);

            if (id == IDC_RADIO_HOST || id == IDC_RADIO_CLIENT) {
                set_addr_controls_enabled(hwnd, id == IDC_RADIO_CLIENT);
                return TRUE;
            }

            if (id == IDOK) {
                wchar_t addr[64] = {}, port[8] = {};
                GetDlgItemTextW(hwnd, IDC_EDIT_ADDR, addr, 64);
                GetDlgItemTextW(hwnd, IDC_EDIT_PORT, port, 8);
                const bool is_client = IsDlgButtonChecked(hwnd, IDC_RADIO_CLIENT) == BST_CHECKED;

                const long port_n = wcstol(port, nullptr, 10);
                if (port_n < 1 || port_n > 65535) {
                    MessageBoxW(hwnd, L"Port must be between 1 and 65535.",
                               L"mgmp", MB_OK | MB_ICONWARNING);
                    return TRUE;   // keep the dialog open
                }
                if (is_client && addr[0] == L'\0') {
                    MessageBoxW(hwnd, L"Enter the host's IP address.",
                               L"mgmp", MB_OK | MB_ICONWARNING);
                    return TRUE;
                }

                st->is_client = is_client;
                wcsncpy_s(st->addr, addr, _TRUNCATE);
                wcsncpy_s(st->port, port, _TRUNCATE);
                st->dont_show = IsDlgButtonChecked(hwnd, IDC_CHECK_NOSHOW) == BST_CHECKED;
                st->launched  = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }
            return FALSE;
        }

        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;

        default:
            return FALSE;
    }
}

// UTF-8 std::string <-> UTF-16 std::wstring. mgmp.json is UTF-8 on disk (same
// as mgmp_config.cpp reads); the dialog's own controls are wide, since that
// is what the Win32 Edit control speaks natively.
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string narrow(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

} // namespace

bool run_launcher_dialog(const wchar_t* dir) {
    wchar_t env[8];
    if (GetEnvironmentVariableW(L"MGMP_NO_LAUNCHER", env, 8) > 0 && env[0] == L'1')
        return true;   // diagnostic escape hatch -- see the header note

    DialogState st;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\mgmp.json", dir);
    st.path = path;

    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            st.doc = nlohmann::json::parse(in, nullptr, false, true);
            if (st.doc.is_discarded()) st.doc = nlohmann::json::object();
        } else {
            st.doc = nlohmann::json::object();
        }
    }
    if (!st.doc.is_object()) st.doc = nlohmann::json::object();

    // "launcher.enabled" defaults to true (show it) when the key is missing
    // entirely -- a fresh install with no config yet is exactly the case
    // this exists for.
    auto& launcher = st.doc["launcher"];
    if (!launcher.is_object()) launcher = nlohmann::json::object();
    bool enabled = true;
    if (launcher.contains("enabled") && launcher["enabled"].is_boolean())
        enabled = launcher["enabled"].get<bool>();
    if (!enabled) return true;   // net_test.ps1's per-peer configs land here

    // Pre-fill from whatever net.* already says, same "seeded from disk"
    // convention mgmp_ui.cpp's own connect panel uses -- a player who ran
    // this before sees their own last choice, not a blank form.
    auto& net = st.doc["net"];
    if (!net.is_object()) net = nlohmann::json::object();
    std::string role = net.value("role", std::string("host"));
    st.is_client = _wcsicmp(widen(role).c_str(), L"client") == 0;
    wcsncpy_s(st.addr, widen(net.value("addr", std::string("127.0.0.1"))).c_str(), _TRUNCATE);
    wcsncpy_s(st.port, std::to_wstring(net.value("port", 27600)).c_str(), _TRUNCATE);

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    INT_PTR result = DialogBoxParamW(hinst, MAKEINTRESOURCEW(IDD_LAUNCHER), nullptr,
                                     dlg_proc, (LPARAM)&st);
    if (result != IDOK || !st.launched) {
        wprintf(L"[*] launcher cancelled -- not starting the game\n");
        return false;
    }

    net["role"] = narrow(st.is_client ? L"client" : L"host");
    net["addr"] = narrow(st.addr);
    net["port"] = (int)wcstol(st.port, nullptr, 10);
    // Written explicitly either way -- not just when unchecked -- so the
    // file never ends up with a bare `"launcher": {}` (true, but only by the
    // C++ reader's OWN "missing key defaults to enabled" convention; a
    // different consumer of this file, e.g. tools/net_test.ps1's plain
    // property-mutation, has no reason to know that and broke on exactly
    // this shape live).
    launcher["enabled"] = !st.dont_show;

    std::ofstream out(st.path, std::ios::binary | std::ios::trunc);
    if (out) {
        out << st.doc.dump(2);
        wprintf(L"[*] wrote net.role=%s net.addr=%s net.port=%s to %s\n",
                st.is_client ? L"client" : L"host", st.addr, st.port, st.path.c_str());
    } else {
        fwprintf(stderr, L"[!] could not write %s -- launching with whatever was already "
                        L"on disk\n", st.path.c_str());
    }
    return true;
}

} // namespace mgmp_loader
