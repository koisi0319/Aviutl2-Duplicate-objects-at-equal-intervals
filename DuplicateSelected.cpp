// DuplicateSelected.cpp
// AviUtl2 Generic Plugin (.aux2) - オブジェクト行複製 objects
// Spec: same layer, gap(B)=end->nextStart, toggle by count / limit frame
// Build: x64 DLL, rename to .aux2

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <string>
#include <algorithm>
#include <cstdint>

#include "plugin2.h"

// -------------------------
// UI control IDs
// -------------------------
static constexpr int IDC_EDIT_GAP        = 1001;
static constexpr int IDC_RADIO_COUNT     = 1002;
static constexpr int IDC_RADIO_LIMIT     = 1003;
static constexpr int IDC_EDIT_COUNT      = 1004;
static constexpr int IDC_EDIT_LIMIT      = 1005;
static constexpr int IDC_CHECK_STOP      = 1006;
static constexpr int IDC_BTN_APPLY       = 1007;
static constexpr int IDC_STATIC_STATUS   = 1008;

// -------------------------
// Globals
// -------------------------
static HOST_APP_TABLE* g_host = nullptr;
static EDIT_HANDLE*    g_editHandle = nullptr;

static HWND g_hwnd = nullptr;
static HWND g_status = nullptr;

// -------------------------
// Helpers
// -------------------------
static int GetIntFromEdit(HWND hEdit, int defVal)
{
    wchar_t buf[64]{};
    GetWindowTextW(hEdit, buf, 63);
    if (buf[0] == 0) return defVal;
    return _wtoi(buf);
}

static void SetStatus(const std::wstring& s)
{
    if (g_status) SetWindowTextW(g_status, s.c_str());
}

static void EnableModeControls(HWND hwnd)
{
    const bool byCount = (IsDlgButtonChecked(hwnd, IDC_RADIO_COUNT) == BST_CHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_COUNT), byCount);
    EnableWindow(GetDlgItem(hwnd, IDC_EDIT_LIMIT), !byCount);
}

// -------------------------
// Core duplication (runs inside call_edit_section)
// -------------------------
// -------------------------
// Duplication parameters & result (passed via call_edit_section_param)
// -------------------------
struct DuplicateContext {
    // Input params
    int interval = 0;        // start-to-start interval (F)
    bool byCount = true;
    int copies = 1;          // number of duplicates
    int limitFrame = 0;      // duplicate while newStart <= limitFrame
    bool stopOnFail = false; // overlap etc
    // Output results
    int totalCreated = 0;
    int totalFailed  = 0;
    int objCount = 0;        // number of objects processed
    int noAlias = 0;         // objects with no alias data
    std::wstring diagMsg;    // diagnostic message
};

static DuplicateContext g_ctx;

// 1つのオブジェクトを複製する内部関数
static void DuplicateOneObject(EDIT_SECTION* es, OBJECT_HANDLE obj, DuplicateContext& ctx)
{
    // オブジェクトのレイヤー・フレーム情報を取得
    OBJECT_LAYER_FRAME lf = es->get_object_layer_frame(obj);

    const int len = lf.end - lf.start;
    if (len <= 0) {
        ctx.diagMsg += L"[len=0] ";
        return;
    }

    const int step = std::max(1, ctx.interval);  // 先頭→先頭の間隔

    // オブジェクトのエイリアスデータを取得
    LPCSTR aliasData = es->get_object_alias(obj);
    if (!aliasData || aliasData[0] == '\0') {
        ctx.noAlias++;
        ctx.diagMsg += L"[alias=null] ";
        return;
    }

    ctx.objCount++;

    if (ctx.byCount)
    {
        for (int i = 1; i <= std::max(0, ctx.copies); i++)
        {
            const int newStart = lf.start + step * i;
            OBJECT_HANDLE created = es->create_object_from_alias(aliasData, lf.layer, newStart, len);
            if (created) {
                ctx.totalCreated++;
            } else {
                ctx.totalFailed++;
                if (ctx.stopOnFail) break;
            }
        }
    }
    else
    {
        for (int i = 1;; i++)
        {
            const int newStart = lf.start + step * i;
            if (newStart > ctx.limitFrame) break;

            OBJECT_HANDLE created = es->create_object_from_alias(aliasData, lf.layer, newStart, len);
            if (created) {
                ctx.totalCreated++;
            } else {
                ctx.totalFailed++;
                if (ctx.stopOnFail) break;
            }
        }
    }
}

// call_edit_section_param コールバック
static void CALLBACK DoDuplicateCallback(void* param, EDIT_SECTION* es)
{
    auto& ctx = *static_cast<DuplicateContext*>(param);

    if (!es || !es->info) {
        ctx.diagMsg = L"EDIT_SECTION invalid";
        return;
    }

    // まず複数選択オブジェクトを試す
    const int selCount = es->get_selected_object_num();
    if (selCount > 0)
    {
        ctx.diagMsg = L"selected=" + std::to_wstring(selCount) + L" ";
        for (int si = 0; si < selCount; si++)
        {
            OBJECT_HANDLE obj = es->get_selected_object(si);
            if (!obj) continue;
            DuplicateOneObject(es, obj, ctx);
        }
    }
    else
    {
        // フォーカス中のオブジェクト（設定ウィンドウで選択中）をフォールバック
        OBJECT_HANDLE focusObj = es->get_focus_object();
        if (focusObj)
        {
            ctx.diagMsg = L"focus=1 ";
            DuplicateOneObject(es, focusObj, ctx);
        }
        else
        {
            ctx.diagMsg = L"selected=0, focus=none";
            return;
        }
    }
}

// -------------------------
// Window proc
// -------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        CreateWindowExW(0, L"STATIC", L"\u9593\u9694(\u5148\u982d\u2192\u5148\u982d) [F]:", WS_CHILD|WS_VISIBLE,
                        10, 12, 170, 20, hwnd, 0, 0, 0);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"40", WS_CHILD|WS_VISIBLE|ES_NUMBER,
                        190, 10, 70, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_GAP, 0, 0);

        CreateWindowExW(0, L"BUTTON", L"\u56de\u6570", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                        10, 45, 80, 20, hwnd, (HMENU)(INT_PTR)IDC_RADIO_COUNT, 0, 0);
        CreateWindowExW(0, L"BUTTON", L"\u6307\u5b9a\u30d5\u30ec\u30fc\u30e0\u307e\u3067", WS_CHILD|WS_VISIBLE|BS_AUTORADIOBUTTON,
                        100, 45, 160, 20, hwnd, (HMENU)(INT_PTR)IDC_RADIO_LIMIT, 0, 0);

        CreateWindowExW(0, L"STATIC", L"\u56de\u6570:", WS_CHILD|WS_VISIBLE,
                        10, 72, 50, 20, hwnd, 0, 0, 0);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"5", WS_CHILD|WS_VISIBLE|ES_NUMBER,
                        60, 70, 60, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_COUNT, 0, 0);

        CreateWindowExW(0, L"STATIC", L"\u672b\u5c3eF:", WS_CHILD|WS_VISIBLE,
                        140, 72, 50, 20, hwnd, 0, 0, 0);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"1000", WS_CHILD|WS_VISIBLE|ES_NUMBER,
                        190, 70, 70, 22, hwnd, (HMENU)(INT_PTR)IDC_EDIT_LIMIT, 0, 0);

        CreateWindowExW(0, L"BUTTON", L"\u5931\u6557(\u91cd\u306a\u308a\u7b49)\u3067\u505c\u6b62", WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,
                        10, 100, 220, 22, hwnd, (HMENU)(INT_PTR)IDC_CHECK_STOP, 0, 0);

        CreateWindowExW(0, L"BUTTON", L"\u5b9f\u884c\uff08\u9078\u629e\u30aa\u30d6\u30b8\u30a7\u30af\u30c8\u8907\u88fd\uff09", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON,
                        10, 130, 250, 28, hwnd, (HMENU)(INT_PTR)IDC_BTN_APPLY, 0, 0);

        g_status = CreateWindowExW(0, L"STATIC", L"\u5f85\u6a5f\u4e2d", WS_CHILD|WS_VISIBLE,
                        10, 165, 260, 20, hwnd, (HMENU)(INT_PTR)IDC_STATIC_STATUS, 0, 0);

        CheckDlgButton(hwnd, IDC_RADIO_COUNT, BST_CHECKED);
        EnableModeControls(hwnd);
        return 0;
    }
    case WM_COMMAND:
    {
        const int id = LOWORD(wp);
        if (id == IDC_RADIO_COUNT || id == IDC_RADIO_LIMIT)
        {
            EnableModeControls(hwnd);
            return 0;
        }
        if (id == IDC_BTN_APPLY)
        {
            if (!g_editHandle)
            {
                SetStatus(L"EditHandle \u304c\u7121\u52b9\u3067\u3059");
                return 0;
            }

            // パラメータ設定
            g_ctx = {};  // reset
            g_ctx.interval  = std::max(1, GetIntFromEdit(GetDlgItem(hwnd, IDC_EDIT_GAP), 1));
            g_ctx.byCount   = (IsDlgButtonChecked(hwnd, IDC_RADIO_COUNT) == BST_CHECKED);
            g_ctx.copies    = std::max(0, GetIntFromEdit(GetDlgItem(hwnd, IDC_EDIT_COUNT), 0));
            g_ctx.limitFrame= std::max(0, GetIntFromEdit(GetDlgItem(hwnd, IDC_EDIT_LIMIT), 0));
            g_ctx.stopOnFail= (IsDlgButtonChecked(hwnd, IDC_CHECK_STOP) == BST_CHECKED);

            SetStatus(L"\u5b9f\u884c\u4e2d\u2026");

            // call_edit_section_param 経由で編集情報へ安全にアクセス（SDKサンプル準拠）
            bool ok = g_editHandle->call_edit_section_param(&g_ctx, &DoDuplicateCallback);

            // 結果表示
            wchar_t statusBuf[512];
            if (!ok) {
                wsprintfW(statusBuf, L"\u7de8\u96c6\u30bb\u30af\u30b7\u30e7\u30f3\u53d6\u5f97\u5931\u6557 (\u51fa\u529b\u4e2d?)");
            } else {
                wsprintfW(statusBuf, L"\u5b8c\u4e86: \u4f5c\u6210%d / \u5931\u6557%d [%s]",
                    g_ctx.totalCreated, g_ctx.totalFailed, g_ctx.diagMsg.c_str());
            }
            SetStatus(statusBuf);
            return 0;
        }
        break;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// -------------------------
// RegisterPlugin (export)
// -------------------------
EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host)
{
    g_host = host;
    if (!g_host) return;

    // プラグイン情報設定
    g_host->set_plugin_information(L"\u30aa\u30d6\u30b8\u30a7\u30af\u30c8\u884c\u8907\u88fd (aux2)");

    // ウィンドウクラス登録
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DuplicateSelectedWindow";
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    RegisterClassExW(&wc);

    // ウィンドウ作成 (WS_POPUP で作成後、register_window_client で AviUtl2 側に組み込まれる)
    g_hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"\u30aa\u30d6\u30b8\u30a7\u30af\u30c8\u884c\u8907\u88fd",
        WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, 290, 220,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    // AviUtl2へ登録
    g_host->register_window_client(L"\u30aa\u30d6\u30b8\u30a7\u30af\u30c8\u884c\u8907\u88fd", g_hwnd);

    // 編集ハンドル取得
    g_editHandle = g_host->create_edit_handle();
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }