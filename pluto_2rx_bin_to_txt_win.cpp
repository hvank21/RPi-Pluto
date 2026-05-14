/*
    pluto_2rx_bin_to_txt_win.cpp

    Windows GUI utility for converting ADALM-Pluto 2RX binary IQ recordings
    to TXT format.

    Input binary format:
        int16 little-endian interleaved samples:
        I0 Q0 I1 Q1 I0 Q0 I1 Q1 ...

    Output TXT format, without header:
        I0<TAB>Q0<TAB>I1<TAB>Q1

    Build with Visual Studio Developer Command Prompt:
        cl /EHsc /O2 /W4 /DUNICODE /D_UNICODE pluto_2rx_bin_to_txt_win.cpp user32.lib comdlg32.lib shell32.lib

    Build with MinGW-w64:
        g++ -O2 -Wall -Wextra -municode pluto_2rx_bin_to_txt_win.cpp -o pluto_2rx_bin_to_txt_win.exe -lcomdlg32 -lshell32
*/

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <algorithm>

#define IDC_EDIT_INPUT       1001
#define IDC_BTN_INPUT        1002
#define IDC_EDIT_OUTPUT      1003
#define IDC_BTN_OUTPUT       1004
#define IDC_EDIT_NSAMPLES    1005
#define IDC_BTN_CONVERT      1006
#define IDC_BTN_EXIT         1007
#define IDC_STATIC_STATUS    1008
#define IDC_STATIC_INFO      1009
#define IDC_PROGRESS         1010

static HWND g_hInput = nullptr;
static HWND g_hOutput = nullptr;
static HWND g_hSamples = nullptr;
static HWND g_hStatus = nullptr;
static HWND g_hProgress = nullptr;

static std::wstring get_window_text(HWND h)
{
    int len = GetWindowTextLengthW(h);
    std::wstring s;
    s.resize(len);
    if (len > 0) {
        GetWindowTextW(h, &s[0], len + 1);
    }
    return s;
}

static void set_window_text(HWND h, const std::wstring& s)
{
    SetWindowTextW(h, s.c_str());
}

static std::wstring get_file_name_only(const std::wstring& path)
{
    size_t p = path.find_last_of(L"\\/");
    if (p == std::wstring::npos) return path;
    return path.substr(p + 1);
}

static std::wstring default_output_path(const std::wstring& input)
{
    if (input.empty()) return L"";

    std::wstring out = input;
    size_t p = out.find_last_of(L'.');
    if (p != std::wstring::npos) {
        out = out.substr(0, p);
    }
    out += L"_samples.txt";
    return out;
}

static bool open_file_dialog(HWND owner, std::wstring& path)
{
    wchar_t fileName[MAX_PATH] = L"";

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Binary IQ files (*.bin)\0*.bin\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileNameW(&ofn)) {
        path = fileName;
        return true;
    }
    return false;
}

static bool save_file_dialog(HWND owner, std::wstring& path)
{
    wchar_t fileName[MAX_PATH] = L"";
    if (!path.empty()) {
        wcsncpy_s(fileName, path.c_str(), MAX_PATH - 1);
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Text files (*.txt)\0*.txt\0All files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;

    if (GetSaveFileNameW(&ofn)) {
        path = fileName;
        return true;
    }
    return false;
}

static uint64_t parse_uint64(const std::wstring& s)
{
    wchar_t* end = nullptr;
    unsigned long long v = wcstoull(s.c_str(), &end, 10);
    if (end == s.c_str()) return 0;
    return static_cast<uint64_t>(v);
}

static uint64_t file_size_bytes(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }
    ULARGE_INTEGER sz{};
    sz.HighPart = data.nFileSizeHigh;
    sz.LowPart = data.nFileSizeLow;
    return sz.QuadPart;
}

static void show_error(HWND owner, const std::wstring& msg)
{
    MessageBoxW(owner, msg.c_str(), L"Error", MB_ICONERROR | MB_OK);
}

static void show_info(HWND owner, const std::wstring& msg)
{
    MessageBoxW(owner, msg.c_str(), L"Done", MB_ICONINFORMATION | MB_OK);
}

static bool convert_file(HWND owner)
{
    std::wstring inputPath = get_window_text(g_hInput);
    std::wstring outputPath = get_window_text(g_hOutput);
    uint64_t requestedSamples = parse_uint64(get_window_text(g_hSamples));

    if (inputPath.empty()) {
        show_error(owner, L"Choose input .bin file.");
        return false;
    }
    if (outputPath.empty()) {
        show_error(owner, L"Choose output .txt file.");
        return false;
    }
    if (requestedSamples == 0) {
        show_error(owner, L"Number of samples must be > 0.");
        return false;
    }

    const uint64_t bytesPerSample2rx = 4ULL * sizeof(int16_t); // I0 Q0 I1 Q1
    uint64_t fsize = file_size_bytes(inputPath);
    if (fsize < bytesPerSample2rx) {
        show_error(owner, L"Input file is empty or too small.");
        return false;
    }

    uint64_t availableSamples = fsize / bytesPerSample2rx;
    uint64_t samplesToConvert = std::min(requestedSamples, availableSamples);

    if (requestedSamples > availableSamples) {
        std::wstringstream ss;
        ss << L"Requested samples: " << requestedSamples
           << L"\nAvailable samples: " << availableSamples
           << L"\nOnly available samples will be converted.";
        MessageBoxW(owner, ss.str().c_str(), L"Warning", MB_ICONWARNING | MB_OK);
    }

    std::ifstream fin(inputPath, std::ios::binary);
    if (!fin) {
        show_error(owner, L"Cannot open input file.");
        return false;
    }

    std::ofstream fout(outputPath);
    if (!fout) {
        show_error(owner, L"Cannot create output file.");
        return false;
    }

    set_window_text(g_hStatus, L"Converting...");
    SendMessageW(g_hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
    SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);

    constexpr size_t blockSamples = 65536;
    std::vector<int16_t> buffer(blockSamples * 4);

    uint64_t done = 0;
    int lastPercent = -1;

    while (done < samplesToConvert) {
        uint64_t remaining = samplesToConvert - done;
        size_t nowSamples = static_cast<size_t>(std::min<uint64_t>(remaining, blockSamples));
        size_t wordsToRead = nowSamples * 4;
        size_t bytesToRead = wordsToRead * sizeof(int16_t);

        fin.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(bytesToRead));
        std::streamsize gotBytes = fin.gcount();
        if (gotBytes <= 0) break;

        size_t gotWords = static_cast<size_t>(gotBytes) / sizeof(int16_t);
        size_t gotSamples = gotWords / 4;

        for (size_t i = 0; i < gotSamples; ++i) {
            int16_t I0 = buffer[4 * i + 0];
            int16_t Q0 = buffer[4 * i + 1];
            int16_t I1 = buffer[4 * i + 2];
            int16_t Q1 = buffer[4 * i + 3];

            fout << I0 << '\t' << Q0 << '\t' << I1 << '\t' << Q1 << '\n';
        }

        done += gotSamples;

        int percent = static_cast<int>((done * 100ULL) / samplesToConvert);
        if (percent != lastPercent) {
            SendMessageW(g_hProgress, PBM_SETPOS, percent, 0);
            std::wstringstream ss;
            ss << L"Converting... " << percent << L"%  " << done << L" / " << samplesToConvert << L" samples";
            set_window_text(g_hStatus, ss.str());
            lastPercent = percent;
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    fout.close();
    fin.close();

    std::wstringstream ss;
    ss << L"Done. Converted samples: " << done << L"\nOutput:\n" << outputPath;
    set_window_text(g_hStatus, L"Ready.");
    SendMessageW(g_hProgress, PBM_SETPOS, 100, 0);
    show_info(owner, ss.str());

    return true;
}

static void create_controls(HWND hwnd)
{
    HFONT hFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    CreateWindowW(L"STATIC", L"Pluto 2RX BIN → TXT converter for Windows", WS_CHILD | WS_VISIBLE,
                  20, 15, 520, 24, hwnd, nullptr, nullptr, nullptr);

    CreateWindowW(L"STATIC", L"Input .bin:", WS_CHILD | WS_VISIBLE,
                  20, 55, 100, 22, hwnd, nullptr, nullptr, nullptr);
    g_hInput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             120, 52, 460, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_INPUT), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Browse...", WS_CHILD | WS_VISIBLE,
                  590, 51, 95, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_INPUT), nullptr, nullptr);

    CreateWindowW(L"STATIC", L"Output .txt:", WS_CHILD | WS_VISIBLE,
                  20, 95, 100, 22, hwnd, nullptr, nullptr, nullptr);
    g_hOutput = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                              120, 92, 460, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_OUTPUT), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Save as...", WS_CHILD | WS_VISIBLE,
                  590, 91, 95, 26, hwnd, reinterpret_cast<HMENU>(IDC_BTN_OUTPUT), nullptr, nullptr);

    CreateWindowW(L"STATIC", L"Samples:", WS_CHILD | WS_VISIBLE,
                  20, 135, 100, 22, hwnd, nullptr, nullptr, nullptr);
    g_hSamples = CreateWindowW(L"EDIT", L"100000", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               120, 132, 160, 24, hwnd, reinterpret_cast<HMENU>(IDC_EDIT_NSAMPLES), nullptr, nullptr);

    CreateWindowW(L"STATIC", L"Input format: int16 I0 Q0 I1 Q1.  TXT output: I0 Q0 I1 Q1, no header.",
                  WS_CHILD | WS_VISIBLE,
                  20, 172, 650, 22, hwnd, reinterpret_cast<HMENU>(IDC_STATIC_INFO), nullptr, nullptr);

    g_hProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER,
                                20, 205, 665, 22, hwnd, reinterpret_cast<HMENU>(IDC_PROGRESS), nullptr, nullptr);

    g_hStatus = CreateWindowW(L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE,
                              20, 237, 665, 22, hwnd, reinterpret_cast<HMENU>(IDC_STATIC_STATUS), nullptr, nullptr);

    CreateWindowW(L"BUTTON", L"Convert", WS_CHILD | WS_VISIBLE,
                  460, 275, 105, 32, hwnd, reinterpret_cast<HMENU>(IDC_BTN_CONVERT), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Exit", WS_CHILD | WS_VISIBLE,
                  580, 275, 105, 32, hwnd, reinterpret_cast<HMENU>(IDC_BTN_EXIT), nullptr, nullptr);

    HWND controls[] = {g_hInput, g_hOutput, g_hSamples, g_hStatus, g_hProgress};
    for (HWND h : controls) {
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
    }

    // Apply font to all direct child controls.
    HWND child = GetWindow(hwnd, GW_CHILD);
    while (child) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);
        child = GetWindow(child, GW_HWNDNEXT);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_CREATE:
        create_controls(hwnd);
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_BTN_INPUT: {
            std::wstring path;
            if (open_file_dialog(hwnd, path)) {
                set_window_text(g_hInput, path);
                std::wstring out = default_output_path(path);
                set_window_text(g_hOutput, out);

                uint64_t fsize = file_size_bytes(path);
                uint64_t samples = fsize / 8ULL;
                std::wstringstream ss;
                ss << L"File: " << get_file_name_only(path) << L" | Size: " << fsize
                   << L" bytes | Available 2RX samples: " << samples;
                set_window_text(g_hStatus, ss.str());
            }
            return 0;
        }
        case IDC_BTN_OUTPUT: {
            std::wstring path = get_window_text(g_hOutput);
            if (save_file_dialog(hwnd, path)) {
                set_window_text(g_hOutput, path);
            }
            return 0;
        }
        case IDC_BTN_CONVERT:
            convert_file(hwnd);
            return 0;

        case IDC_BTN_EXIT:
            PostQuitMessage(0);
            return 0;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow)
{
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    const wchar_t CLASS_NAME[] = L"Pluto2RxBinToTxtWinClass";

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);

    if (!RegisterClassW(&wc)) {
        MessageBoxW(nullptr, L"RegisterClass failed.", L"Error", MB_ICONERROR);
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Pluto 2RX BIN to TXT Converter",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        725, 365,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        MessageBoxW(nullptr, L"CreateWindow failed.", L"Error", MB_ICONERROR);
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return static_cast<int>(msg.wParam);
}
