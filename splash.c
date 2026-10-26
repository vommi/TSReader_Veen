// splash.c - Memorial splash screen for TSReader
// In memory of Rod Hewitt KG6TTD (G6TTD), 2025
//
// Shows a 5-second splash dialog at startup with Rod's photo, memorial text,
// and a clickable PayPal donate link.
//
// Uses stb_image (already in the project) to load the JPG and render it via GDI.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <stdio.h>

// stb_image implementation is in stubs/isource_impl.c - just declare what we need
#include "stubs/stb_image.h"

#define SPLASH_WIDTH         640
#define SPLASH_HEIGHT        820
#define PHOTO_HEIGHT         340
#define ID_BTN_CONTINUE      0x8001

#define PAYPAL_URL           "https://www.paypal.com/ncp/payment/DAHT2Y2HLAFMY"
#define GITHUB_URL           "https://github.com/TSReader/TSReader"

static HBITMAP g_hSplashPhoto = NULL;
static RECT    g_rcPayPal = { 0, 0, 0, 0 };
static RECT    g_rcGitHub = { 0, 0, 0, 0 };
static HCURSOR g_hHand    = NULL;

static const char *g_szMemorial =
    "Rod Hewitt KG6TTD (G6TTD) passed away March 2025 having spent a lifetime "
    "dedicated to programming and was the author of TSReader.\r\n\r\n"
    "His work will be available for download for free with just one version, "
    "TSReaderPro, being available both here and on a GitHub Page for others "
    "to continue the project.\r\n\r\n"
    "Please feel free to donate to help us maintain the website hosting etc "
    "\x97 it would be very much appreciated.";

// Load image via CreateFile/ReadFile (works around stb_image fopen quirks in
// paths with spaces/parens under UAC-elevated processes) and convert to
// top-down DIB HBITMAP
static HBITMAP LoadPhotoBitmap(const char *szPath, int maxW, int maxH)
{
    HANDLE hFile = CreateFileA(szPath, GENERIC_READ, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        char dbg[MAX_PATH + 64];
        wsprintfA(dbg, "[TSReader Splash] CreateFileA failed (err %lu): %s\n",
                  GetLastError(), szPath);
        OutputDebugStringA(dbg);
        return NULL;
    }
    DWORD dwSize = GetFileSize(hFile, NULL);
    if (dwSize == 0 || dwSize > 20*1024*1024) {
        CloseHandle(hFile);
        return NULL;
    }
    unsigned char *buf = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, dwSize);
    if (!buf) { CloseHandle(hFile); return NULL; }
    DWORD dwRead = 0;
    BOOL ok = ReadFile(hFile, buf, dwSize, &dwRead, NULL);
    CloseHandle(hFile);
    if (!ok || dwRead != dwSize) {
        HeapFree(GetProcessHeap(), 0, buf);
        return NULL;
    }

    int w, h, n;
    unsigned char *pixels = stbi_load_from_memory(buf, (int)dwSize, &w, &h, &n, 3);
    HeapFree(GetProcessHeap(), 0, buf);
    if (!pixels) return NULL;

    // Scale to fit while preserving aspect ratio
    double sx = (double)maxW / w, sy = (double)maxH / h;
    double s = sx < sy ? sx : sy;
    if (s > 1.0) s = 1.0;
    int dw = (int)(w * s), dh = (int)(h * s);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;

    // Create a DIB section at target size
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth       = dw;
    bmi.bmiHeader.biHeight      = -dh; // top-down
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    ReleaseDC(NULL, hdc);

    if (!hbm) { stbi_image_free(pixels); return NULL; }

    // Nearest-neighbour downscale, stride-aware, convert RGB->BGR
    int dstStride = ((dw * 3 + 3) & ~3);
    unsigned char *dst = (unsigned char *)bits;
    int x, y;
    for (y = 0; y < dh; y++) {
        int sy2 = (int)(y / s);
        if (sy2 >= h) sy2 = h - 1;
        unsigned char *srow = pixels + sy2 * w * 3;
        unsigned char *drow = dst + y * dstStride;
        for (x = 0; x < dw; x++) {
            int sx2 = (int)(x / s);
            if (sx2 >= w) sx2 = w - 1;
            unsigned char *p = srow + sx2 * 3;
            drow[x*3+0] = p[2]; // B
            drow[x*3+1] = p[1]; // G
            drow[x*3+2] = p[0]; // R
        }
    }

    stbi_image_free(pixels);
    return hbm;
}

static void DrawHyperlink(HDC hdc, int x, int y, const char *text, RECT *outRect)
{
    SetTextColor(hdc, RGB(0, 102, 204));
    HFONT hOld = (HFONT)SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT));
    SIZE sz;
    GetTextExtentPoint32(hdc, text, lstrlenA(text), &sz);
    TextOutA(hdc, x, y, text, lstrlenA(text));
    // Underline
    HPEN hPen = CreatePen(PS_SOLID, 1, RGB(0, 102, 204));
    HPEN hOldPen = (HPEN)SelectObject(hdc, hPen);
    MoveToEx(hdc, x, y + sz.cy, NULL);
    LineTo(hdc, x + sz.cx, y + sz.cy);
    SelectObject(hdc, hOldPen);
    DeleteObject(hPen);
    if (outRect) { outRect->left = x; outRect->top = y; outRect->right = x + sz.cx; outRect->bottom = y + sz.cy + 1; }
    SelectObject(hdc, hOld);
}

static LRESULT CALLBACK SplashWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE: {
        g_hHand = LoadCursor(NULL, IDC_HAND);
        // Create Continue button centred near the bottom
        int btnW = 120, btnH = 32;
        int btnX = (SPLASH_WIDTH - btnW) / 2;
        int btnY = SPLASH_HEIGHT - btnH - 50;
        HWND hBtn = CreateWindowA("BUTTON", "Continue",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            btnX, btnY, btnW, btnH, hWnd, (HMENU)(INT_PTR)ID_BTN_CONTINUE,
            ((LPCREATESTRUCT)lp)->hInstance, NULL);
        // Use the system GUI font on the button
        SendMessage(hBtn, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        SetFocus(hBtn);
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == ID_BTN_CONTINUE) {
            DestroyWindow(hWnd);
        }
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);

        // White background
        HBRUSH hBg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, hBg);
        DeleteObject(hBg);

        // Photo centered at top
        int photoY = 20;
        int photoH = PHOTO_HEIGHT;
        if (g_hSplashPhoto) {
            BITMAP bm;
            GetObject(g_hSplashPhoto, sizeof(bm), &bm);
            HDC hdcMem = CreateCompatibleDC(hdc);
            HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, g_hSplashPhoto);
            int px = (rc.right - bm.bmWidth) / 2;
            BitBlt(hdc, px, photoY, bm.bmWidth, bm.bmHeight, hdcMem, 0, 0, SRCCOPY);
            photoH = bm.bmHeight;
            SelectObject(hdcMem, hOld);
            DeleteDC(hdcMem);
        }

        // Title
        int textY = photoY + photoH + 18;
        SetBkMode(hdc, TRANSPARENT);
        HFONT hTitle = CreateFontA(-22, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
        HFONT hOldF = (HFONT)SelectObject(hdc, hTitle);
        SetTextColor(hdc, RGB(46, 80, 144));
        RECT rcTitle = { 0, textY, rc.right, textY + 34 };
        DrawTextA(hdc, "In memory of Rod Hewitt KG6TTD (G6TTD)", -1, &rcTitle,
                  DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(hdc, hOldF);
        DeleteObject(hTitle);

        // Memorial body
        textY += 40;
        HFONT hBody = CreateFontA(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
        hOldF = (HFONT)SelectObject(hdc, hBody);
        SetTextColor(hdc, RGB(51, 51, 51));
        RECT rcBody = { 30, textY, rc.right - 30, rc.bottom - 130 };
        DrawTextA(hdc, g_szMemorial, -1, &rcBody,
                  DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(hdc, hOldF);
        DeleteObject(hBody);

        // Bottom links: GitHub (left) and PayPal (right) - above the Continue button
        int linkY = rc.bottom - 110;
        HFONT hLink = CreateFontA(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, "Segoe UI");
        hOldF = (HFONT)SelectObject(hdc, hLink);
        DrawHyperlink(hdc, 30, linkY, "GitHub: github.com/TSReader/TSReader", &g_rcGitHub);

        // Measure PayPal text for right-alignment
        const char *payText = "Donate via PayPal";
        SIZE sz;
        GetTextExtentPoint32A(hdc, payText, lstrlenA(payText), &sz);
        DrawHyperlink(hdc, rc.right - sz.cx - 30, linkY, payText, &g_rcPayPal);
        SelectObject(hdc, hOldF);
        DeleteObject(hLink);

        EndPaint(hWnd, &ps);
        return 0;
    }

    case WM_SETCURSOR: {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hWnd, &pt);
        if (PtInRect(&g_rcPayPal, pt) || PtInRect(&g_rcGitHub, pt)) {
            SetCursor(g_hHand);
            return TRUE;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        POINT pt = { LOWORD(lp), HIWORD(lp) };
        if (PtInRect(&g_rcPayPal, pt)) {
            ShellExecuteA(NULL, "open", PAYPAL_URL, NULL, NULL, SW_SHOW);
            return 0;
        }
        if (PtInRect(&g_rcGitHub, pt)) {
            ShellExecuteA(NULL, "open", GITHUB_URL, NULL, NULL, SW_SHOW);
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        if (g_hSplashPhoto) { DeleteObject(g_hSplashPhoto); g_hSplashPhoto = NULL; }
        // Wake the local message loop so it sees the window is gone (when it is closed
        // by a sent message nothing else may arrive to end GetMessage)
        PostThreadMessage(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wp, lp);
}

// Show splash screen - blocks until closed (user click, key, or 5s timer)
void ShowMemorialSplash(HINSTANCE hInstance)
{
    // Locate the photo relative to the exe directory - try PNG then JPG
    char szBase[MAX_PATH], szPath[MAX_PATH];
    GetModuleFileNameA(NULL, szBase, MAX_PATH);
    char *p = strrchr(szBase, '\\');
    if (p) *(p + 1) = '\0';

    wsprintfA(szPath, "%srod_splash.png", szBase);
    {
        char dbg[MAX_PATH + 64];
        wsprintfA(dbg, "[TSReader Splash] Trying photo path: %s\n", szPath);
        OutputDebugStringA(dbg);
    }
    g_hSplashPhoto = LoadPhotoBitmap(szPath, SPLASH_WIDTH - 40, PHOTO_HEIGHT);
    if (!g_hSplashPhoto) {
        OutputDebugStringA("[TSReader Splash] PNG load failed, trying JPG\n");
        wsprintfA(szPath, "%srod_splash.jpg", szBase);
        g_hSplashPhoto = LoadPhotoBitmap(szPath, SPLASH_WIDTH - 40, PHOTO_HEIGHT);
    }
    if (!g_hSplashPhoto) {
        char dbg[MAX_PATH + 128];
        wsprintfA(dbg, "[TSReader Splash] Could not load %srod_splash.png/.jpg - stbi: %s\n",
                  szBase, stbi_failure_reason() ? stbi_failure_reason() : "(none)");
        OutputDebugStringA(dbg);
    } else {
        BITMAP bm;
        GetObject(g_hSplashPhoto, sizeof(bm), &bm);
        char dbg[128];
        wsprintfA(dbg, "[TSReader Splash] Photo loaded: %dx%d\n", bm.bmWidth, bm.bmHeight);
        OutputDebugStringA(dbg);
    }
    // If photo is missing, still show the text splash

    // Register window class
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc   = SplashWndProc;
    wc.hInstance     = hInstance;
    wc.hbrBackground = NULL;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "TSReaderMemorialSplash";
    RegisterClassA(&wc);

    // Centre on primary monitor
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int x = (sw - SPLASH_WIDTH) / 2;
    int y = (sh - SPLASH_HEIGHT) / 2;

    HWND hWnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        "TSReaderMemorialSplash", "TSReaderPro",
        WS_POPUP | WS_BORDER | WS_CAPTION | WS_SYSMENU,
        x, y, SPLASH_WIDTH, SPLASH_HEIGHT,
        NULL, NULL, hInstance, NULL);

    if (!hWnd) return;

    ShowWindow(hWnd, SW_SHOW);
    UpdateWindow(hWnd);

    // Local message loop until window destroyed
    MSG msg;
    while (IsWindow(hWnd) && GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnregisterClassA("TSReaderMemorialSplash", hInstance);
}
