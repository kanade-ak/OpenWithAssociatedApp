#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <vector>

#include "plugin2.h"
#include "logger2.h"
#include "config2.h"

namespace {

constexpr wchar_t PluginName[] = L"OpenWithAssociatedApp";
constexpr wchar_t IniSection[] = L"Applications";
constexpr wchar_t RunAppsSection[] = L"RunApps";
constexpr int MaxRunAppMenus = 32;
constexpr int IDC_EXT_EDIT = 1001;
constexpr int IDC_APP_EDIT = 1002;
constexpr int IDC_BROWSE = 1003;
constexpr int IDC_LIST = 1004;
constexpr int IDC_ADD = 1005;
constexpr int IDC_DELETE = 1006;
constexpr int IDC_APP_COMBO = 1007;
constexpr int IDC_NAME_EDIT = 1008;
constexpr int IDC_UP = 1009;
constexpr int IDC_DOWN = 1010;
constexpr int IDC_OK = IDOK;
constexpr int IDC_CANCEL = IDCANCEL;

struct Association {
    std::wstring extension;
    std::wstring application;
};

struct RunApp {
    std::wstring name;
    std::wstring command;
};

struct DialogState {
    HWND owner = nullptr;
    HFONT font = nullptr;
    std::vector<Association> associations;
    std::vector<std::wstring> candidate_paths;
};

struct RunAppsState {
    HWND owner = nullptr;
    HFONT font = nullptr;
    std::vector<RunApp> apps;
};

HINSTANCE g_instance = nullptr;
EDIT_HANDLE* g_edit_handle = nullptr;
LOG_HANDLE* g_logger = nullptr;
CONFIG_HANDLE* g_config = nullptr;
std::wstring g_ini_path;
std::vector<Association> g_associations;
std::vector<RunApp> g_run_apps;
std::vector<RunApp> g_registered_run_apps;
bool g_restart_requested = false;

COMMON_PLUGIN_TABLE g_common_plugin_table = {
    L"OpenWithAssociatedApp",
    L"OpenWithAssociatedApp version 1.00",
};

std::wstring trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring to_lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return value;
}

std::wstring normalize_extension(std::wstring extension) {
    extension = to_lower(trim(extension));
    if (extension.empty()) return {};
    if (extension.front() != L'.') extension.insert(extension.begin(), L'.');
    return extension;
}

std::wstring ini_key_from_extension(const std::wstring& extension) {
    if (!extension.empty() && extension.front() == L'.') return extension.substr(1);
    return extension;
}

std::wstring file_base_name(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    auto name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
    return name;
}

std::wstring first_command_token(const std::wstring& command) {
    if (command.empty()) return {};
    if (command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        return end == std::wstring::npos ? command.substr(1) : command.substr(1, end - 1);
    }
    const auto space = command.find(L' ');
    return space == std::wstring::npos ? command : command.substr(0, space);
}

// メニュー名の階層区切り('\')やINIの区切り('|')に使う文字を除去する
std::wstring sanitize_run_app_name(const std::wstring& name) {
    std::wstring result;
    for (const wchar_t ch : name) {
        if (ch == L'|' || ch == L'\\' || ch == L'/') continue;
        result += ch;
    }
    return trim(result);
}

std::wstring get_parent_directory(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return {};
    return path.substr(0, pos);
}

std::wstring get_module_path() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(g_instance, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) return {};
        if (length < path.size() - 1) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring get_ini_path() {
    if (!g_ini_path.empty()) return g_ini_path;

    const auto module_path = get_module_path();
    const auto module_dir = get_parent_directory(module_path);
    g_ini_path = module_dir.empty()
        ? std::wstring(L"OpenWithAssociatedApp.ini")
        : module_dir + L"\\OpenWithAssociatedApp.ini";
    return g_ini_path;
}

void log_warning(const std::wstring& message) {
    if (g_logger) g_logger->warn(g_logger, message.c_str());
}

void log_error(const std::wstring& message) {
    if (g_logger) g_logger->error(g_logger, message.c_str());
}

void show_message(HWND owner, const std::wstring& message, UINT icon = MB_ICONINFORMATION) {
    MessageBoxW(owner, message.c_str(), L"対応ソフト", MB_OK | icon);
}

void load_associations() {
    g_associations.clear();

    std::vector<wchar_t> buffer(32768, L'\0');
    const auto path = get_ini_path();
    const DWORD read = GetPrivateProfileSectionW(
        IniSection,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        path.c_str());
    if (read == 0) return;

    for (const wchar_t* entry = buffer.data(); *entry != L'\0'; entry += wcslen(entry) + 1) {
        const std::wstring line(entry);
        const auto split = line.find(L'=');
        if (split == std::wstring::npos) continue;

        auto extension = normalize_extension(line.substr(0, split));
        auto application = trim(line.substr(split + 1));
        if (extension.empty() || application.empty()) continue;

        g_associations.push_back({ std::move(extension), std::move(application) });
    }
}

bool save_associations(const std::vector<Association>& associations) {
    const auto path = get_ini_path();
    WritePrivateProfileStringW(IniSection, nullptr, nullptr, path.c_str());

    for (const auto& association : associations) {
        const auto key = ini_key_from_extension(association.extension);
        if (!WritePrivateProfileStringW(IniSection, key.c_str(), association.application.c_str(), path.c_str())) {
            return false;
        }
    }

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    g_associations = associations;
    return true;
}

void load_run_apps() {
    g_run_apps.clear();

    const auto path = get_ini_path();
    for (int i = 1; i <= 999; ++i) {
        wchar_t key[16] = {};
        swprintf_s(key, L"%d", i);

        std::vector<wchar_t> buffer(4096, L'\0');
        GetPrivateProfileStringW(RunAppsSection, key, L"", buffer.data(), static_cast<DWORD>(buffer.size()), path.c_str());
        const std::wstring line = trim(buffer.data());
        if (line.empty()) break;

        std::wstring name;
        std::wstring command;
        const auto split = line.find(L'|');
        if (split == std::wstring::npos) {
            command = trim(line);
        } else {
            name = sanitize_run_app_name(line.substr(0, split));
            command = trim(line.substr(split + 1));
        }
        if (command.empty()) continue;
        if (name.empty()) name = file_base_name(first_command_token(command));
        if (name.empty()) continue;

        g_run_apps.push_back({ std::move(name), std::move(command) });
    }
}

bool save_run_apps(const std::vector<RunApp>& apps) {
    const auto path = get_ini_path();
    WritePrivateProfileStringW(RunAppsSection, nullptr, nullptr, path.c_str());

    for (size_t i = 0; i < apps.size(); ++i) {
        wchar_t key[16] = {};
        swprintf_s(key, L"%d", static_cast<int>(i) + 1);
        const auto value = apps[i].name + L"|" + apps[i].command;
        if (!WritePrivateProfileStringW(RunAppsSection, key, value.c_str(), path.c_str())) {
            return false;
        }
    }

    WritePrivateProfileStringW(nullptr, nullptr, nullptr, path.c_str());
    g_run_apps = apps;
    return true;
}

std::wstring get_window_text_string(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring value(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, value.data(), length + 1);
    }
    value.resize(static_cast<size_t>(length));
    return value;
}

void set_control_font(HWND hwnd, HFONT font) {
    if (hwnd && font) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND create_control(
    HWND parent,
    const wchar_t* class_name,
    const wchar_t* text,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    int id,
    HFONT font) {
    HWND control = CreateWindowExW(
        0,
        class_name,
        text,
        WS_CHILD | WS_VISIBLE | style,
        x,
        y,
        width,
        height,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        g_instance,
        nullptr);
    set_control_font(control, font);
    return control;
}

bool file_exists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

struct AppCandidate {
    std::wstring name;
    std::wstring path;
};

std::vector<AppCandidate> enum_app_candidates(const std::wstring& extension) {
    std::vector<AppCandidate> result;

    for (const ASSOC_FILTER filter : { ASSOC_FILTER_RECOMMENDED, ASSOC_FILTER_NONE }) {
        IEnumAssocHandlers* enumerator = nullptr;
        if (FAILED(SHAssocEnumHandlers(extension.c_str(), filter, &enumerator)) || !enumerator) continue;

        IAssocHandler* handler = nullptr;
        while (enumerator->Next(1, &handler, nullptr) == S_OK && handler) {
            std::wstring path;
            std::wstring name;
            LPWSTR value = nullptr;
            if (SUCCEEDED(handler->GetName(&value)) && value) {
                path = value;
                CoTaskMemFree(value);
            }
            value = nullptr;
            if (SUCCEEDED(handler->GetUIName(&value)) && value) {
                name = value;
                CoTaskMemFree(value);
            }
            handler->Release();
            handler = nullptr;

            if (path.empty() || !file_exists(path)) continue;

            const auto lower_path = to_lower(path);
            const bool duplicate = std::any_of(result.begin(), result.end(), [&](const AppCandidate& item) {
                return to_lower(item.path) == lower_path;
            });
            if (duplicate) continue;

            result.push_back({ name.empty() ? path : name, path });
        }
        enumerator->Release();

        if (!result.empty()) break;
    }

    return result;
}

void populate_app_candidates(HWND hwnd, DialogState* state) {
    HWND combo = GetDlgItem(hwnd, IDC_APP_COMBO);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    state->candidate_paths.clear();
    state->candidate_paths.push_back({});

    const auto extension = normalize_extension(get_window_text_string(GetDlgItem(hwnd, IDC_EXT_EDIT)));
    if (extension.empty()) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(拡張子を入力すると候補を表示します)"));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return;
    }

    const auto candidates = enum_app_candidates(extension);
    if (candidates.empty()) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(候補なし: [参照...]から選択してください)"));
        SendMessageW(combo, CB_SETCURSEL, 0, 0);
        return;
    }

    const auto placeholder = extension + L" を開けるアプリから選択...";
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(placeholder.c_str()));
    for (const auto& candidate : candidates) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(candidate.name.c_str()));
        state->candidate_paths.push_back(candidate.path);
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void apply_candidate_selection(HWND hwnd, DialogState* state) {
    HWND combo = GetDlgItem(hwnd, IDC_APP_COMBO);
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index <= 0 || index >= static_cast<int>(state->candidate_paths.size())) return;
    if (state->candidate_paths[index].empty()) return;

    SetWindowTextW(GetDlgItem(hwnd, IDC_APP_EDIT), state->candidate_paths[index].c_str());
}

int selected_list_index(HWND hwnd) {
    return static_cast<int>(SendMessageW(GetDlgItem(hwnd, IDC_LIST), LVM_GETNEXTITEM, static_cast<WPARAM>(-1), LVNI_SELECTED));
}

void refresh_list(HWND hwnd, DialogState* state, int select_index = -1) {
    HWND list = GetDlgItem(hwnd, IDC_LIST);
    ListView_DeleteAllItems(list);

    for (size_t i = 0; i < state->associations.size(); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(state->associations[i].extension.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, static_cast<int>(i), 1, const_cast<wchar_t*>(state->associations[i].application.c_str()));
    }

    if (select_index >= 0 && select_index < static_cast<int>(state->associations.size())) {
        ListView_SetItemState(list, select_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, select_index, FALSE);
    }
}

void fill_edits_from_selection(HWND hwnd, DialogState* state) {
    const int index = selected_list_index(hwnd);
    if (index < 0 || index >= static_cast<int>(state->associations.size())) return;

    SetWindowTextW(GetDlgItem(hwnd, IDC_EXT_EDIT), state->associations[index].extension.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_APP_EDIT), state->associations[index].application.c_str());
}

void add_or_update_association(HWND hwnd, DialogState* state) {
    const auto extension = normalize_extension(get_window_text_string(GetDlgItem(hwnd, IDC_EXT_EDIT)));
    const auto application = trim(get_window_text_string(GetDlgItem(hwnd, IDC_APP_EDIT)));

    if (extension.empty()) {
        show_message(hwnd, L"拡張子を入力してください。", MB_ICONWARNING);
        return;
    }
    if (application.empty()) {
        show_message(hwnd, L"アプリを候補から選択するか、[参照...]で指定してください。", MB_ICONWARNING);
        return;
    }

    auto found = std::find_if(state->associations.begin(), state->associations.end(), [&](const Association& item) {
        return item.extension == extension;
    });
    if (found == state->associations.end()) {
        state->associations.push_back({ extension, application });
    } else {
        found->application = application;
    }

    std::sort(state->associations.begin(), state->associations.end(), [](const Association& left, const Association& right) {
        return left.extension < right.extension;
    });

    const auto added = std::find_if(state->associations.begin(), state->associations.end(), [&](const Association& item) {
        return item.extension == extension;
    });
    refresh_list(hwnd, state, static_cast<int>(added - state->associations.begin()));
}

void delete_selected_association(HWND hwnd, DialogState* state) {
    const int index = selected_list_index(hwnd);
    if (index < 0 || index >= static_cast<int>(state->associations.size())) return;

    state->associations.erase(state->associations.begin() + index);
    const int next = index < static_cast<int>(state->associations.size())
        ? index
        : static_cast<int>(state->associations.size()) - 1;
    refresh_list(hwnd, state, next);
}

void browse_application(HWND hwnd) {
    wchar_t file[MAX_PATH] = {};
    const auto current = trim(get_window_text_string(GetDlgItem(hwnd, IDC_APP_EDIT)));
    if (!current.empty() && current.find(L"{file}") == std::wstring::npos) {
        wcsncpy_s(file, current.c_str(), _TRUNCATE);
    }

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"Executable Files (*.exe)\0*.exe\0All Files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrTitle = L"外部アプリケーションを選択";

    if (GetOpenFileNameW(&ofn)) {
        SetWindowTextW(GetDlgItem(hwnd, IDC_APP_EDIT), file);
    }
}

LRESULT CALLBACK config_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE:
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = reinterpret_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        state->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        create_control(hwnd, L"STATIC", L"拡張子", 0, 12, 16, 72, 18, -1, state->font);
        create_control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 90, 12, 110, 24, IDC_EXT_EDIT, state->font);
        create_control(hwnd, L"STATIC", L"(入力すると下の候補が更新されます)", 0, 210, 16, 322, 18, -1, state->font);
        create_control(hwnd, L"STATIC", L"アプリ候補", 0, 12, 48, 72, 18, -1, state->font);
        create_control(hwnd, L"COMBOBOX", L"", WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST, 90, 44, 442, 320, IDC_APP_COMBO, state->font);
        create_control(hwnd, L"STATIC", L"実行アプリ", 0, 12, 80, 72, 18, -1, state->font);
        create_control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 90, 76, 350, 24, IDC_APP_EDIT, state->font);
        create_control(hwnd, L"BUTTON", L"参照...", WS_TABSTOP | BS_PUSHBUTTON, 448, 75, 84, 26, IDC_BROWSE, state->font);
        create_control(hwnd, L"STATIC", L"{file} を含めるとその位置に素材ファイルパスを挿入して起動します", 0, 90, 106, 442, 18, -1, state->font);
        create_control(hwnd, L"BUTTON", L"追加/更新", WS_TABSTOP | BS_PUSHBUTTON, 12, 130, 112, 26, IDC_ADD, state->font);
        create_control(hwnd, L"BUTTON", L"削除", WS_TABSTOP | BS_PUSHBUTTON, 132, 130, 84, 26, IDC_DELETE, state->font);

        HWND list = create_control(
            hwnd, WC_LISTVIEWW, L"",
            WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 164, 520, 148, IDC_LIST, state->font);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"拡張子");
        column.cx = 90;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"アプリケーション");
        column.cx = 406;
        ListView_InsertColumn(list, 1, &column);

        create_control(hwnd, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 356, 322, 84, 28, IDC_OK, state->font);
        create_control(hwnd, L"BUTTON", L"キャンセル", WS_TABSTOP | BS_PUSHBUTTON, 448, 322, 84, 28, IDC_CANCEL, state->font);

        refresh_list(hwnd, state);
        populate_app_candidates(hwnd, state);
        return 0;
    }

    case WM_NOTIFY:
    {
        if (!state) break;
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == IDC_LIST) {
            if (header->code == LVN_ITEMCHANGED) {
                const auto* info = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if ((info->uNewState & LVIS_SELECTED) && !(info->uOldState & LVIS_SELECTED)) {
                    fill_edits_from_selection(hwnd, state);
                }
            } else if (header->code == LVN_KEYDOWN) {
                if (reinterpret_cast<const NMLVKEYDOWN*>(lparam)->wVKey == VK_DELETE) {
                    delete_selected_association(hwnd, state);
                }
            }
        }
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wparam)) {
        case IDC_EXT_EDIT:
            if (HIWORD(wparam) == EN_CHANGE) populate_app_candidates(hwnd, state);
            return 0;
        case IDC_APP_COMBO:
            if (HIWORD(wparam) == CBN_SELCHANGE) apply_candidate_selection(hwnd, state);
            return 0;
        case IDC_BROWSE:
            browse_application(hwnd);
            return 0;
        case IDC_ADD:
            add_or_update_association(hwnd, state);
            return 0;
        case IDC_DELETE:
            delete_selected_association(hwnd, state);
            return 0;
        case IDC_OK:
            if (!save_associations(state->associations)) {
                show_message(hwnd, L"設定ファイルの保存に失敗しました。", MB_ICONERROR);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

struct ComScope {
    bool initialized = false;
    ComScope() {
        initialized = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
    }
    ~ComScope() {
        if (initialized) CoUninitialize();
    }
};

HWND create_dialog_window(
    const wchar_t* class_name,
    WNDPROC proc,
    const wchar_t* title,
    int client_width,
    int client_height,
    HWND owner,
    void* state) {
    WNDCLASSEXW existing = {};
    if (!GetClassInfoExW(g_instance, class_name, &existing)) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = proc;
        wc.hInstance = g_instance;
        wc.lpszClassName = class_name;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        if (!RegisterClassExW(&wc)) return nullptr;
    }

    RECT frame = { 0, 0, client_width, client_height };
    AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_DLGMODALFRAME);

    return CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT,
        class_name,
        title,
        WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        frame.right - frame.left,
        frame.bottom - frame.top,
        owner,
        nullptr,
        g_instance,
        state);
}

void run_modal_dialog(HWND owner, HWND hwnd) {
    if (owner) EnableWindow(owner, FALSE);

    RECT owner_rect = {};
    RECT window_rect = {};
    if (owner && GetWindowRect(owner, &owner_rect) && GetWindowRect(hwnd, &window_rect)) {
        const int width = window_rect.right - window_rect.left;
        const int height = window_rect.bottom - window_rect.top;
        const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
        const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
        SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOZORDER | SWP_NOSIZE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (IsWindow(hwnd) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (owner) {
        EnableWindow(owner, TRUE);
        SetForegroundWindow(owner);
    }
}

void show_config_dialog(HWND owner, HINSTANCE dll_hinst) {
    if (dll_hinst) g_instance = dll_hinst;
    load_associations();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    ComScope com_scope;

    auto* state = new DialogState();
    state->owner = owner;
    state->associations = g_associations;

    HWND hwnd = create_dialog_window(
        L"OpenWithAssociatedAppConfigWindow",
        config_window_proc,
        L"拡張子設定",
        544, 362,
        owner,
        state);
    if (!hwnd) {
        delete state;
        show_message(owner, L"設定ウィンドウを作成できませんでした。", MB_ICONERROR);
        return;
    }

    run_modal_dialog(owner, hwnd);
}

//----------------------------------------------------------------------------------
// アプリ実行設定ダイアログ

void refresh_run_app_list(HWND hwnd, RunAppsState* state, int select_index = -1) {
    HWND list = GetDlgItem(hwnd, IDC_LIST);
    ListView_DeleteAllItems(list);

    for (size_t i = 0; i < state->apps.size(); ++i) {
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(state->apps[i].name.c_str());
        ListView_InsertItem(list, &item);
        ListView_SetItemText(list, static_cast<int>(i), 1, const_cast<wchar_t*>(state->apps[i].command.c_str()));
    }

    if (select_index >= 0 && select_index < static_cast<int>(state->apps.size())) {
        ListView_SetItemState(list, select_index, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list, select_index, FALSE);
    }
}

void fill_run_app_edits_from_selection(HWND hwnd, RunAppsState* state) {
    const int index = selected_list_index(hwnd);
    if (index < 0 || index >= static_cast<int>(state->apps.size())) return;

    SetWindowTextW(GetDlgItem(hwnd, IDC_NAME_EDIT), state->apps[index].name.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_APP_EDIT), state->apps[index].command.c_str());
}

void add_or_update_run_app(HWND hwnd, RunAppsState* state) {
    auto name = sanitize_run_app_name(get_window_text_string(GetDlgItem(hwnd, IDC_NAME_EDIT)));
    const auto command = trim(get_window_text_string(GetDlgItem(hwnd, IDC_APP_EDIT)));

    if (command.empty()) {
        show_message(hwnd, L"実行アプリを[参照...]で選択するか、直接入力してください。", MB_ICONWARNING);
        return;
    }
    if (name.empty()) name = file_base_name(first_command_token(command));
    if (name.empty()) {
        show_message(hwnd, L"メニューに表示する名前を入力してください。", MB_ICONWARNING);
        return;
    }

    auto found = std::find_if(state->apps.begin(), state->apps.end(), [&](const RunApp& item) {
        return item.name == name;
    });
    int index = 0;
    if (found == state->apps.end()) {
        state->apps.push_back({ name, command });
        index = static_cast<int>(state->apps.size()) - 1;
    } else {
        found->command = command;
        index = static_cast<int>(found - state->apps.begin());
    }

    refresh_run_app_list(hwnd, state, index);
}

void delete_selected_run_app(HWND hwnd, RunAppsState* state) {
    const int index = selected_list_index(hwnd);
    if (index < 0 || index >= static_cast<int>(state->apps.size())) return;

    state->apps.erase(state->apps.begin() + index);
    const int next = index < static_cast<int>(state->apps.size())
        ? index
        : static_cast<int>(state->apps.size()) - 1;
    refresh_run_app_list(hwnd, state, next);
}

void move_selected_run_app(HWND hwnd, RunAppsState* state, int delta) {
    const int index = selected_list_index(hwnd);
    if (index < 0 || index >= static_cast<int>(state->apps.size())) return;

    const int target = index + delta;
    if (target < 0 || target >= static_cast<int>(state->apps.size())) return;

    std::swap(state->apps[index], state->apps[target]);
    refresh_run_app_list(hwnd, state, target);
}

bool run_app_menus_changed(const std::vector<RunApp>& apps) {
    if (apps.size() != g_registered_run_apps.size()) return true;
    for (size_t i = 0; i < apps.size(); ++i) {
        if (apps[i].name != g_registered_run_apps[i].name) return true;
    }
    return false;
}

LRESULT CALLBACK run_apps_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<RunAppsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (message) {
    case WM_CREATE:
    {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = reinterpret_cast<RunAppsState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));

        state->font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

        create_control(hwnd, L"STATIC", L"名前", 0, 12, 16, 72, 18, -1, state->font);
        create_control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 90, 12, 200, 24, IDC_NAME_EDIT, state->font);
        create_control(hwnd, L"STATIC", L"(空欄ならexe名が入ります)", 0, 300, 16, 232, 18, -1, state->font);
        create_control(hwnd, L"STATIC", L"実行アプリ", 0, 12, 48, 72, 18, -1, state->font);
        create_control(hwnd, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 90, 44, 350, 24, IDC_APP_EDIT, state->font);
        create_control(hwnd, L"BUTTON", L"参照...", WS_TABSTOP | BS_PUSHBUTTON, 448, 43, 84, 26, IDC_BROWSE, state->font);
        create_control(hwnd, L"STATIC", L"{file} を含めるとその位置に素材ファイルパスを挿入して起動します", 0, 90, 74, 442, 18, -1, state->font);
        create_control(hwnd, L"BUTTON", L"追加/更新", WS_TABSTOP | BS_PUSHBUTTON, 12, 100, 112, 26, IDC_ADD, state->font);
        create_control(hwnd, L"BUTTON", L"削除", WS_TABSTOP | BS_PUSHBUTTON, 132, 100, 84, 26, IDC_DELETE, state->font);
        create_control(hwnd, L"BUTTON", L"上へ", WS_TABSTOP | BS_PUSHBUTTON, 224, 100, 70, 26, IDC_UP, state->font);
        create_control(hwnd, L"BUTTON", L"下へ", WS_TABSTOP | BS_PUSHBUTTON, 302, 100, 70, 26, IDC_DOWN, state->font);

        HWND list = create_control(
            hwnd, WC_LISTVIEWW, L"",
            WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            12, 134, 520, 150, IDC_LIST, state->font);
        ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
        LVCOLUMNW column = {};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(L"名前");
        column.cx = 140;
        ListView_InsertColumn(list, 0, &column);
        column.pszText = const_cast<wchar_t*>(L"アプリケーション");
        column.cx = 356;
        ListView_InsertColumn(list, 1, &column);

        create_control(hwnd, L"STATIC", L"右クリックメニューの[アプリ実行]にリストの順番で表示されます(名前・順番の変更は再起動後に反映)", 0, 12, 290, 520, 18, -1, state->font);

        create_control(hwnd, L"BUTTON", L"OK", WS_TABSTOP | BS_DEFPUSHBUTTON, 356, 316, 84, 28, IDC_OK, state->font);
        create_control(hwnd, L"BUTTON", L"キャンセル", WS_TABSTOP | BS_PUSHBUTTON, 448, 316, 84, 28, IDC_CANCEL, state->font);

        refresh_run_app_list(hwnd, state);
        return 0;
    }

    case WM_NOTIFY:
    {
        if (!state) break;
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == IDC_LIST) {
            if (header->code == LVN_ITEMCHANGED) {
                const auto* info = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if ((info->uNewState & LVIS_SELECTED) && !(info->uOldState & LVIS_SELECTED)) {
                    fill_run_app_edits_from_selection(hwnd, state);
                }
            } else if (header->code == LVN_KEYDOWN) {
                if (reinterpret_cast<const NMLVKEYDOWN*>(lparam)->wVKey == VK_DELETE) {
                    delete_selected_run_app(hwnd, state);
                }
            }
        }
        break;
    }

    case WM_COMMAND:
        if (!state) break;
        switch (LOWORD(wparam)) {
        case IDC_BROWSE:
        {
            browse_application(hwnd);
            const auto name = trim(get_window_text_string(GetDlgItem(hwnd, IDC_NAME_EDIT)));
            if (name.empty()) {
                const auto command = trim(get_window_text_string(GetDlgItem(hwnd, IDC_APP_EDIT)));
                const auto base = sanitize_run_app_name(file_base_name(first_command_token(command)));
                if (!base.empty()) SetWindowTextW(GetDlgItem(hwnd, IDC_NAME_EDIT), base.c_str());
            }
            return 0;
        }
        case IDC_ADD:
            add_or_update_run_app(hwnd, state);
            return 0;
        case IDC_DELETE:
            delete_selected_run_app(hwnd, state);
            return 0;
        case IDC_UP:
            move_selected_run_app(hwnd, state, -1);
            return 0;
        case IDC_DOWN:
            move_selected_run_app(hwnd, state, +1);
            return 0;
        case IDC_OK:
        {
            if (!save_run_apps(state->apps)) {
                show_message(hwnd, L"設定ファイルの保存に失敗しました。", MB_ICONERROR);
                return 0;
            }
            if (run_app_menus_changed(state->apps)) {
                const int answer = MessageBoxW(
                    hwnd,
                    L"メニューの表示名・順番・数の変更はAviUtl2の再起動後に反映されます。\n今すぐ再起動しますか?",
                    L"アプリ実行設定",
                    MB_YESNO | MB_ICONQUESTION);
                if (answer == IDYES) g_restart_requested = true;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case IDC_CANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete state;
        return 0;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void show_run_apps_dialog(HWND owner, HINSTANCE dll_hinst) {
    if (dll_hinst) g_instance = dll_hinst;
    load_run_apps();

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    auto* state = new RunAppsState();
    state->owner = owner;
    state->apps = g_run_apps;

    HWND hwnd = create_dialog_window(
        L"OpenWithAssociatedAppRunAppsWindow",
        run_apps_window_proc,
        L"アプリ実行設定",
        544, 356,
        owner,
        state);
    if (!hwnd) {
        delete state;
        show_message(owner, L"設定ウィンドウを作成できませんでした。", MB_ICONERROR);
        return;
    }

    g_restart_requested = false;
    run_modal_dialog(owner, hwnd);

    if (g_restart_requested && g_edit_handle) {
        g_restart_requested = false;
        g_edit_handle->restart_host_app();
    }
}

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (length <= 0) {
        codepage = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(codepage, flags, value.data(), static_cast<int>(value.size()), nullptr, 0);
    }
    if (length <= 0) return {};

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(codepage, flags, value.data(), static_cast<int>(value.size()), result.data(), length);
    return result;
}

std::wstring unquote_value(std::wstring value) {
    value = trim(std::move(value));
    if (value.size() >= 2) {
        const wchar_t first = value.front();
        const wchar_t last = value.back();
        if ((first == L'"' && last == L'"') || (first == L'\'' && last == L'\'')) {
            value = value.substr(1, value.size() - 2);
        }
    }
    return trim(std::move(value));
}

std::wstring strip_resource_prefix(std::wstring value) {
    constexpr wchar_t ImagePrefix[] = L"image:";
    if (_wcsnicmp(value.c_str(), ImagePrefix, wcslen(ImagePrefix)) == 0) {
        return value.substr(wcslen(ImagePrefix));
    }
    return value;
}

std::wstring expand_environment_strings(const std::wstring& value) {
    DWORD length = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (length == 0) return value;
    std::wstring result(length, L'\0');
    DWORD written = ExpandEnvironmentStringsW(value.c_str(), result.data(), length);
    if (written == 0 || written > length) return value;
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

bool is_absolute_path(const std::wstring& path) {
    if (path.size() >= 3 &&
        std::iswalpha(path[0]) &&
        path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) {
        return true;
    }
    return path.rfind(L"\\\\", 0) == 0 || path.rfind(L"//", 0) == 0;
}

std::wstring get_full_path(const std::wstring& path) {
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (length == 0) return path;
    if (length >= buffer.size()) {
        buffer.resize(length + 1);
        length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0) return path;
    }
    buffer.resize(length);
    return buffer;
}

std::wstring combine_path(const std::wstring& directory, const std::wstring& relative) {
    if (directory.empty()) return get_full_path(relative);
    if (directory.back() == L'\\' || directory.back() == L'/') return get_full_path(directory + relative);
    return get_full_path(directory + L"\\" + relative);
}

std::wstring get_extension_from_path(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return {};
    if (slash != std::wstring::npos && dot < slash) return {};
    if (dot + 1 >= path.size()) return {};
    return normalize_extension(path.substr(dot));
}

const Association* find_association(const std::wstring& extension) {
    const auto normalized = normalize_extension(extension);
    const auto found = std::find_if(g_associations.begin(), g_associations.end(), [&](const Association& item) {
        return item.extension == normalized;
    });
    return found == g_associations.end() ? nullptr : &*found;
}

bool has_association_for_path(const std::wstring& path) {
    return find_association(get_extension_from_path(path)) != nullptr;
}

bool looks_like_path(const std::wstring& value) {
    if (value.find(L'\\') != std::wstring::npos || value.find(L'/') != std::wstring::npos) return true;
    if (value.size() >= 2 && value[1] == L':') return true;
    return !get_extension_from_path(value).empty();
}

std::wstring resolve_candidate_path(const std::wstring& raw_value, const std::wstring& project_dir) {
    auto value = strip_resource_prefix(unquote_value(raw_value));
    value = expand_environment_strings(value);
    if (value.empty()) return {};

    if (is_absolute_path(value)) return get_full_path(value);

    if (!project_dir.empty()) {
        auto project_path = combine_path(project_dir, value);
        if (file_exists(project_path)) return project_path;
    }

    wchar_t current_dir[MAX_PATH] = {};
    if (GetCurrentDirectoryW(MAX_PATH, current_dir) > 0) {
        auto current_path = combine_path(current_dir, value);
        if (file_exists(current_path)) return current_path;
    }

    return project_dir.empty() ? get_full_path(value) : combine_path(project_dir, value);
}

std::wstring get_project_directory(EDIT_SECTION* edit) {
    if (!edit || !g_edit_handle) return {};

    PROJECT_FILE* project = edit->get_project_file(g_edit_handle);
    if (!project) return {};

    LPCWSTR project_path = project->get_project_file_path();
    if (!project_path || !*project_path) return {};

    return get_parent_directory(project_path);
}

bool is_object_section(const std::wstring& section) {
    return section.rfind(L"Object.", 0) == 0;
}

int score_candidate(const std::wstring& key, const std::wstring& path) {
    const auto lower_key = to_lower(key);
    int score = 0;

    if (key == L"ファイル" || lower_key == L"file") score += 100;
    if (key.find(L"ファイル") != std::wstring::npos ||
        lower_key.find(L"file") != std::wstring::npos ||
        lower_key.find(L"path") != std::wstring::npos) {
        score += 50;
    }
    if (has_association_for_path(path)) score += 40;
    if (file_exists(path)) score += 20;
    if (looks_like_path(path)) score += 10;

    return score;
}

std::wstring extract_material_path_from_alias(LPCSTR alias_utf8, const std::wstring& project_dir) {
    if (!alias_utf8 || !*alias_utf8) return {};

    const std::string alias(alias_utf8);
    size_t start = 0;
    std::wstring current_section;
    std::wstring best_path;
    int best_score = 0;

    while (start <= alias.size()) {
        const size_t end = alias.find_first_of("\r\n", start);
        const auto line_view = end == std::string::npos
            ? std::string_view(alias).substr(start)
            : std::string_view(alias).substr(start, end - start);
        start = end == std::string::npos ? alias.size() + 1 : end + 1;

        auto line = trim(utf8_to_wide(line_view));
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;

        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            current_section = line.substr(1, line.size() - 2);
            continue;
        }

        if (!is_object_section(current_section)) continue;

        const auto split = line.find(L'=');
        if (split == std::wstring::npos) continue;

        const auto key = trim(line.substr(0, split));
        const auto value = unquote_value(line.substr(split + 1));
        if (value.empty()) continue;

        const auto path = resolve_candidate_path(value, project_dir);
        if (path.empty()) continue;

        const int score = score_candidate(key, path);
        if (score > best_score && score >= 60) {
            best_score = score;
            best_path = path;
        }
    }

    return best_path;
}

OBJECT_HANDLE find_target_object(EDIT_SECTION* edit) {
    if (!edit) return nullptr;

    if (auto object = edit->get_focus_object()) return object;

    const int selected_num = edit->get_selected_object_num();
    if (selected_num > 0) {
        if (auto object = edit->get_selected_object(0)) return object;
    }

    int layer = 0;
    int frame = 0;
    if (edit->get_mouse_layer_frame(&layer, &frame)) {
        return edit->find_object(layer, frame);
    }

    return nullptr;
}

std::wstring quote_argument(const std::wstring& value) {
    std::wstring result = L"\"";
    for (const auto ch : value) {
        if (ch == L'"') result += L'\\';
        result += ch;
    }
    result += L'"';
    return result;
}

std::wstring replace_all(std::wstring value, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return value;
    size_t pos = 0;
    while ((pos = value.find(from, pos)) != std::wstring::npos) {
        value.replace(pos, from.size(), to);
        pos += to.size();
    }
    return value;
}

bool launch_application(const std::wstring& application, const std::wstring& material_path, HWND owner) {
    if (application.find(L"{file}") != std::wstring::npos) {
        auto command = replace_all(application, L"{file}", quote_argument(material_path));
        STARTUPINFOW startup = {};
        PROCESS_INFORMATION process = {};
        startup.cb = sizeof(startup);

        std::vector<wchar_t> command_line(command.begin(), command.end());
        command_line.push_back(L'\0');

        if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process)) {
            return false;
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    const auto parameters = quote_argument(material_path);
    const auto result = ShellExecuteW(owner, L"open", application.c_str(), parameters.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

bool launch_os_associated_app(const std::wstring& material_path, HWND owner) {
    const auto result = ShellExecuteW(owner, L"open", material_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

std::wstring get_material_path(EDIT_SECTION* edit, HWND owner) {
    OBJECT_HANDLE object = find_target_object(edit);
    if (!object) {
        show_message(owner, L"対象オブジェクトを取得できませんでした。", MB_ICONWARNING);
        return {};
    }

    const auto project_dir = get_project_directory(edit);
    const auto material_path = extract_material_path_from_alias(edit->get_object_alias(object), project_dir);
    if (material_path.empty()) {
        show_message(owner, L"対象オブジェクトから素材ファイルパスを取得できませんでした。", MB_ICONWARNING);
    }
    return material_path;
}

void open_with_associated_app(EDIT_SECTION* edit) {
    HWND owner = g_edit_handle ? g_edit_handle->get_host_app_window() : nullptr;
    load_associations();

    const auto material_path = get_material_path(edit, owner);
    if (material_path.empty()) return;

    const auto extension = get_extension_from_path(material_path);
    const auto* association = find_association(extension);
    if (!association) {
        if (!launch_os_associated_app(material_path, owner)) {
            log_error(L"Failed to launch OS associated application: " + material_path);
            show_message(owner, L"OSで紐づいているアプリケーションの起動に失敗しました。\n\n" + material_path, MB_ICONERROR);
        }
        return;
    }

    if (!launch_application(association->application, material_path, owner)) {
        log_error(L"Failed to launch application: " + association->application);
        show_message(owner, L"外部アプリケーションの起動に失敗しました。\n\n" + association->application, MB_ICONERROR);
        return;
    }
}

void run_app_with_edit(int index, EDIT_SECTION* edit) {
    HWND owner = g_edit_handle ? g_edit_handle->get_host_app_window() : nullptr;
    if (index < 0 || index >= static_cast<int>(g_registered_run_apps.size())) return;

    const auto material_path = get_material_path(edit, owner);
    if (material_path.empty()) return;

    // コマンドの変更は再起動なしで反映されるよう、登録時の名前でINIの最新値を引き直す
    const auto& registered = g_registered_run_apps[index];
    load_run_apps();
    std::wstring command = registered.command;
    for (const auto& app : g_run_apps) {
        if (app.name == registered.name) {
            command = app.command;
            break;
        }
    }

    if (!launch_application(command, material_path, owner)) {
        log_error(L"Failed to launch application: " + command);
        show_message(owner, L"アプリの起動に失敗しました。\n\n" + command, MB_ICONERROR);
    }
}

void run_app_menu_proc(void* param) {
    if (!g_edit_handle) return;
    g_edit_handle->call_edit_section_param(param, [](void* p, EDIT_SECTION* edit) {
        run_app_with_edit(static_cast<int>(reinterpret_cast<INT_PTR>(p)), edit);
    });
}

} // namespace

BOOL APIENTRY DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_instance = hinst;
        DisableThreadLibraryCalls(hinst);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) DWORD RequiredVersion() {
    return 2004900;
}

extern "C" __declspec(dllexport) void InitializeLogger(LOG_HANDLE* logger) {
    g_logger = logger;
}

extern "C" __declspec(dllexport) void InitializeConfig(CONFIG_HANDLE* config) {
    g_config = config;
    (void)g_config;
}

extern "C" __declspec(dllexport) bool InitializePlugin(DWORD) {
    load_associations();
    load_run_apps();
    return true;
}

extern "C" __declspec(dllexport) void UninitializePlugin() {
}

extern "C" __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable() {
    return &g_common_plugin_table;
}

extern "C" __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
    if (!host) return;

    host->register_object_menu(L"対応ソフト", open_with_associated_app);

    load_run_apps();
    g_registered_run_apps = g_run_apps;
    if (g_registered_run_apps.size() > static_cast<size_t>(MaxRunAppMenus)) {
        g_registered_run_apps.resize(MaxRunAppMenus);
    }
    static std::vector<std::wstring> menu_labels;
    menu_labels.clear();
    for (const auto& app : g_registered_run_apps) {
        menu_labels.push_back(L"アプリ実行\\" + app.name);
    }
    for (size_t i = 0; i < menu_labels.size(); ++i) {
        host->register_object_menu_param(
            menu_labels[i].c_str(),
            reinterpret_cast<void*>(static_cast<INT_PTR>(i)),
            run_app_menu_proc);
    }

    host->register_config_menu(L"拡張子設定", show_config_dialog);
    host->register_config_menu(L"アプリ実行設定", show_run_apps_dialog);

    g_edit_handle = host->create_edit_handle();
    if (!g_edit_handle) {
        log_warning(L"create_edit_handle failed");
    }
}
