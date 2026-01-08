// RecuperHhorrairesTV.cpp
// Utilisation API TVMaze gratuite
// https://api.tvmaze.com/schedule?country=FR

#define WM_TVPROGRAMS_LOADED (WM_USER + 1)

#include <windows.h>
#include <commctrl.h>
#include <winhttp.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <utility>
#include <cstdio>
/// Cette entete ne vient pas de moi
/// JSON for Modern C++  version 3.12.0  https://github.com/nlohmann/json
#include "json.hpp"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")

using nlohmann::json;

HINSTANCE g_hInst = nullptr;
HWND g_hListView = nullptr;
HWND g_hStatus = nullptr;
HWND g_hBtnPrev = nullptr;
HWND g_hBtnNext = nullptr;
HWND g_hEditFilter = nullptr;
HWND g_hBtnFilter = nullptr;

SYSTEMTIME g_currentDate;
int g_sortColumn = 0;
bool g_sortAscending = true;

struct TvProgram
{
    std::wstring channel;
    std::wstring title;
    std::wstring startTime;
    std::wstring endTime;
    std::wstring url;
};

std::vector<TvProgram> g_allPrograms;   // tous les résultats
std::vector<TvProgram> g_viewPrograms;  // filtrés

#define IDC_LISTVIEW     1001
#define IDC_BTN_PREV     2001
#define IDC_BTN_NEXT     2002
#define IDC_STATUS       3001
#define IDC_EDIT_FILTER  4001
#define IDC_BTN_FILTER   4002

#define WM_TVPROGRAMS_LOADED (WM_USER + 1)

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

// ---------- Utilitaires date ----------

std::string FormatDate(const SYSTEMTIME& st)
{
    char buf[16];
    sprintf_s(buf, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
    return buf;
}

void AddDays(SYSTEMTIME& st, int delta)
{
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);

    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;

    const ULONGLONG oneDay = 24ULL * 60 * 60 * 10000000ULL;
    u.QuadPart += (LONGLONG)delta * oneDay;

    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;

    FileTimeToSystemTime(&ft, &st);
}

// ---------- HTTP ----------

std::string HttpGetA(const std::wstring& host, INTERNET_PORT port, const std::wstring& path)
{
    std::string result;

    HINTERNET hSession = WinHttpOpen(L"TvGuideWin32/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect)
    {
        WinHttpCloseHandle(hSession);
        return result;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        0);
    if (!hRequest)
    {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }

    BOOL bResults = WinHttpSendRequest(hRequest,
        WINHTTP_NO_ADDITIONAL_HEADERS,
        0,
        WINHTTP_NO_REQUEST_DATA,
        0,
        0,
        0);

    if (bResults)
        bResults = WinHttpReceiveResponse(hRequest, nullptr);

    if (bResults)
    {
        DWORD dwSize = 0;
        do
        {
            DWORD dwDownloaded = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &dwSize))
                break;

            if (dwSize == 0)
                break;

            std::string buffer;
            buffer.resize(dwSize);

            if (!WinHttpReadData(hRequest, &buffer[0], dwSize, &dwDownloaded))
                break;

            buffer.resize(dwDownloaded);
            result += buffer;

        } while (dwSize > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

std::string FetchSchedule(const std::string& country, const SYSTEMTIME& date)
{
    std::wstring host = L"api.tvmaze.com";
    INTERNET_PORT port = 80;

    std::string d = FormatDate(date);
   std::wstring path = L"/schedule/?date=" +
       std::wstring(d.begin(), d.end());
     //std::wstring path = L"/schedule/full";
    return HttpGetA(host, port, path);
}

// ---------- ListView ----------

void InitListViewColumns(HWND hListView)
{
    LVCOLUMN col = { 0 };
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    col.pszText = (LPWSTR)L"Chaîne";
    col.cx = 150;
    col.iSubItem = 0;
    ListView_InsertColumn(hListView, 0, &col);

    col.pszText = (LPWSTR)L"Titre";
    col.cx = 680;
    col.iSubItem = 1;
    ListView_InsertColumn(hListView, 1, &col);

    col.pszText = (LPWSTR)L"Début";
    col.cx = 130;
    col.iSubItem = 2;
    ListView_InsertColumn(hListView, 2, &col);
}

HWND CreateListView(HWND hParent)
{
    RECT rc;
    GetClientRect(hParent, &rc);

    HWND hList = CreateWindowEx(
        WS_EX_CLIENTEDGE,
        WC_LISTVIEW,
        nullptr,
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 35,
        rc.right - rc.left,
        rc.bottom - rc.top - 35 - 20,
        hParent,
        (HMENU)IDC_LISTVIEW,
        g_hInst,
        nullptr);

    ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    InitListViewColumns(hList);

    return hList;
}

void FillListView(HWND hListView, const std::vector<TvProgram>& programs)
{
    ListView_DeleteAllItems(hListView);

    int index = 0;
    for (const auto& p : programs)
    {
        LVITEM item = { 0 };
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = index;
        item.iSubItem = 0;
        item.pszText = const_cast<LPWSTR>(p.channel.c_str());
        item.lParam = index;
        int row = ListView_InsertItem(hListView, &item);

        ListView_SetItemText(hListView, row, 1, const_cast<LPWSTR>(p.title.c_str()));
        ListView_SetItemText(hListView, row, 2, const_cast<LPWSTR>(p.startTime.c_str()));

        ++index;
    }
}

// ---------- Tri ----------

int CALLBACK ListViewCompareProc(LPARAM lParam1, LPARAM lParam2, LPARAM lParamSort)
{
    int column = LOWORD(lParamSort);
    bool ascending = HIWORD(lParamSort) != 0;

    const TvProgram& a = g_viewPrograms[(int)lParam1];
    const TvProgram& b = g_viewPrograms[(int)lParam2];

    const std::wstring* sa = nullptr;
    const std::wstring* sb = nullptr;

    switch (column)
    {
    case 0: sa = &a.channel;   sb = &b.channel;   break;
    case 1: sa = &a.title;     sb = &b.title;     break;
    case 2: sa = &a.startTime; sb = &b.startTime; break;
    case 3: sa = &a.endTime;   sb = &b.endTime;   break;
    default: sa = &a.title;    sb = &b.title;     break;
    }

    int cmp = _wcsicmp(sa->c_str(), sb->c_str());
    return ascending ? cmp : -cmp;
}

// ---------- Parsing JSON ----------

void ParseTvMazeJson(const std::string& response)
{
    g_allPrograms.clear();

    json j;
    try { j = json::parse(response); }
    catch (...) { return; }

    for (const auto& item : j)
    {
        try
        {
            TvProgram p;

            if (item.contains("show") &&
                item["show"].contains("network") &&
                !item["show"]["network"].is_null())
            {
                std::string ch = item["show"]["network"]["name"];
                p.channel.assign(ch.begin(), ch.end());
            }
            else if (item.contains("show") &&
                item["show"].contains("webChannel") &&
                !item["show"]["webChannel"].is_null())
            {
                std::string ch = item["show"]["webChannel"]["name"];
                p.channel.assign(ch.begin(), ch.end());
            }
            /**/
            if (item.contains("name"))
            {
                std::string t = item["name"];
                p.title.assign(t.begin(), t.end());
            }

            if (item.contains("airdate") && item.contains("airtime"))
            {
                std::string st = item["airdate"].get<std::string>() +
                    " " +
                    item["airtime"].get<std::string>();
                p.startTime.assign(st.begin(), st.end());
            }

            if (item.contains("show") &&
                item["show"].contains("url") &&
                !item["show"]["url"].is_null())
            {
                std::string u = item["show"]["url"];
                p.url.assign(u.begin(), u.end());
            }

            p.endTime = L"";

            if (!p.title.empty())
                g_allPrograms.push_back(std::move(p));
        }
        catch (...) {}
    }
}

// ---------- Filtre chaîne / titre ----------

void ApplyFilter()
{
    wchar_t buf[256];
    GetWindowTextW(g_hEditFilter, buf, 256);
    std::wstring filter = buf;

    g_viewPrograms.clear();

    if (filter.empty())
    {
        g_viewPrograms = g_allPrograms;
        return;
    }

    std::wstring filterLower = filter;
    for (auto& c : filterLower) c = towlower(c);

    for (const auto& p : g_allPrograms)
    {
        std::wstring ch = p.channel;
        std::wstring ti = p.title;
        for (auto& c : ch) c = towlower(c);
        for (auto& c : ti) c = towlower(c);

        if (ch.find(filterLower) != std::wstring::npos ||
            ti.find(filterLower) != std::wstring::npos)
        {
            g_viewPrograms.push_back(p);
        }
    }
}

// ---------- Async load ----------

void StartAsyncLoad(HWND hWnd)
{
    SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)L"Chargement...");

    std::thread([hWnd]()
        {
            DWORD start = GetTickCount();
            std::string response = FetchSchedule("CA", g_currentDate);
            DWORD elapsed = GetTickCount() - start;

            auto* result = new std::pair<std::string, DWORD>(response, elapsed);
            PostMessage(hWnd, WM_TVPROGRAMS_LOADED, (WPARAM)result, 0);
        }).detach();
}

// ---------- WinMain ----------

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow)
{
    g_hInst = hInstance;

    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icex);

    WNDCLASSEX wc = { 0 };
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"TvGuideClass";
    wc.hIcon = LoadIcon(wc.hInstance, (LPCWSTR)IDI_ICON1);

    if (!RegisterClassEx(&wc))
        return 0;

    HWND hWnd = CreateWindowEx(
        0,
        wc.lpszClassName,
        L"Guide TV - TVMaze - Win32",
        WS_OVERLAPPED | WS_CAPTION |WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        1000, 700,
        nullptr,
        nullptr,
        hInstance,
        nullptr);

    if (!hWnd)
        return 0;

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

// ---------- WndProc ----------

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
    {
        GetLocalTime(&g_currentDate);

        g_hBtnPrev = CreateWindow(L"BUTTON", L"< Précédent",
            WS_CHILD | WS_VISIBLE,
            10, 5, 120, 25,
            hWnd, (HMENU)IDC_BTN_PREV, g_hInst, nullptr);

        g_hBtnNext = CreateWindow(L"BUTTON", L"Suivant >",
            WS_CHILD | WS_VISIBLE,
            140, 5, 120, 25,
            hWnd, (HMENU)IDC_BTN_NEXT, g_hInst, nullptr);

        g_hEditFilter = CreateWindowEx(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            280, 5, 250, 25,
            hWnd, (HMENU)IDC_EDIT_FILTER, g_hInst, nullptr);

        g_hBtnFilter = CreateWindow(L"BUTTON", L"Filtrer",
            WS_CHILD | WS_VISIBLE,
            540, 5, 80, 25,
            hWnd, (HMENU)IDC_BTN_FILTER, g_hInst, nullptr);

        g_hListView = CreateListView(hWnd);

        g_hStatus = CreateWindowEx(0, STATUSCLASSNAME, nullptr,
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0,
            hWnd, (HMENU)IDC_STATUS, g_hInst, nullptr);

        StartAsyncLoad(hWnd);
        return 0;
    }

    case WM_SIZE:
    {
        int cx = LOWORD(lParam);
        int cy = HIWORD(lParam);

        MoveWindow(g_hBtnPrev, 10, 5, 120, 25, TRUE);
        MoveWindow(g_hBtnNext, 140, 5, 120, 25, TRUE);
        MoveWindow(g_hEditFilter, 280, 5, 250, 25, TRUE);
        MoveWindow(g_hBtnFilter, 540, 5, 80, 25, TRUE);

        SendMessage(g_hStatus, WM_SIZE, 0, 0);

        RECT rcStatus;
        GetWindowRect(g_hStatus, &rcStatus);
        int statusHeight = rcStatus.bottom - rcStatus.top;

        MoveWindow(g_hListView, 0, 35, cx, cy - 35 - statusHeight, TRUE);
        return 0;
    }

    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case IDC_BTN_PREV:
            AddDays(g_currentDate, -1);
            StartAsyncLoad(hWnd);
            break;

        case IDC_BTN_NEXT:
            AddDays(g_currentDate, +1);
            StartAsyncLoad(hWnd);
            break;

        case IDC_BTN_FILTER:
            ApplyFilter();
            FillListView(g_hListView, g_viewPrograms);
            break;
        }
        return 0;
    }

    case WM_NOTIFY:
    {
        LPNMHDR hdr = (LPNMHDR)lParam;

        if (hdr->hwndFrom == g_hListView)
        {
            if (hdr->code == LVN_COLUMNCLICK)
            {
                NMLISTVIEW* pnm = (NMLISTVIEW*)lParam;
                int col = pnm->iSubItem;

                if (g_sortColumn == col)
                    g_sortAscending = !g_sortAscending;
                else
                {
                    g_sortColumn = col;
                    g_sortAscending = true;
                }

                LPARAM sortParam = MAKELPARAM(g_sortColumn, g_sortAscending ? 1 : 0);
                ListView_SortItems(g_hListView, ListViewCompareProc, sortParam);
            }
            else if (hdr->code == NM_DBLCLK)
            {
                LPNMITEMACTIVATE pia = (LPNMITEMACTIVATE)lParam;
                int iItem = pia->iItem;
                if (iItem >= 0)
                {
                    LVITEM item = { 0 };
                    item.mask = LVIF_PARAM;
                    item.iItem = iItem;
                    if (ListView_GetItem(g_hListView, &item))
                    {
                        int index = (int)item.lParam;
                        if (index >= 0 && index < (int)g_viewPrograms.size())
                        {
                            const TvProgram& p = g_viewPrograms[index];
                            if (!p.url.empty())
                            {
                                ShellExecuteW(nullptr, L"open", p.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                            }
                        }
                    }
                }
            }
        }
        return 0;
    }

    case WM_TVPROGRAMS_LOADED:
    {
        auto* result = (std::pair<std::string, DWORD>*)wParam;
        std::string response = result->first;
        DWORD elapsed = result->second;
        delete result;

        if (response.empty())
        {
            SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)L"Erreur réseau");
            return 0;
        }

        ParseTvMazeJson(response);
        g_viewPrograms = g_allPrograms;

        FillListView(g_hListView, g_viewPrograms);

        wchar_t buf[64];
        swprintf(buf, 64, L"Terminé en %lu ms", elapsed);
        SendMessage(g_hStatus, SB_SETTEXT, 0, (LPARAM)buf);

        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProc(hWnd, msg, wParam, lParam);
}
