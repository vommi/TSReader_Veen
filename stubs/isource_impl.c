/* _ISource replacement implementation using stb_image + GDI.
   Replaces the commercial ImgSource/Pegasus SDK.
   All image data is 24-bit BGR (bottom-up), matching Windows DIB format. */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_BMP
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include "stb_image_resize2.h"

/* ======================================================================
   Source/Dest handle types (simple file path wrappers)
   ====================================================================== */
typedef struct { char szPath[MAX_PATH]; } ISOURCE_FILE;
typedef struct { char szPath[MAX_PATH]; } IDEST_FILE;

typedef ISOURCE_FILE * HISSRC;
typedef IDEST_FILE * HISDEST;

/* ======================================================================
   Initialization (no-op)
   ====================================================================== */
void _ISInitialize(const char * key) { (void)key; }

/* ======================================================================
   Source file operations
   ====================================================================== */
HISSRC _ISOpenFileSource(const char * filename)
{
    ISOURCE_FILE * src;
    FILE * f;
    if (!filename) return NULL;
    f = fopen(filename, "rb");
    if (!f) return NULL;
    fclose(f);
    src = (ISOURCE_FILE *)malloc(sizeof(ISOURCE_FILE));
    if (!src) return NULL;
    lstrcpynA(src->szPath, filename, MAX_PATH);
    return src;
}

void _ISCloseSource(HISSRC h)
{
    if (h) free(h);
}

/* ======================================================================
   Dest file operations
   ====================================================================== */
HISDEST _ISOpenFileDest(const char * filename)
{
    IDEST_FILE * dst;
    if (!filename) return NULL;
    dst = (IDEST_FILE *)malloc(sizeof(IDEST_FILE));
    if (!dst) return NULL;
    lstrcpynA(dst->szPath, filename, MAX_PATH);
    return dst;
}

void _ISCloseDest(HISDEST h)
{
    if (h) free(h);
}

/* ======================================================================
   Helper: convert stb RGB (top-down) to Windows BGR (bottom-up)
   ====================================================================== */
static BYTE * rgb_topdown_to_bgr_bottomup(unsigned char * stb_data, int w, int h)
{
    BYTE * out = (BYTE *)GlobalAlloc(GPTR, w * h * 3);
    int row, col;
    if (!out) return NULL;
    for (row = 0; row < h; row++)
    {
        BYTE * dst_row = out + (h - 1 - row) * w * 3;
        unsigned char * src_row = stb_data + row * w * 3;
        for (col = 0; col < w; col++)
        {
            dst_row[col * 3 + 0] = src_row[col * 3 + 2]; /* B */
            dst_row[col * 3 + 1] = src_row[col * 3 + 1]; /* G */
            dst_row[col * 3 + 2] = src_row[col * 3 + 0]; /* R */
        }
    }
    return out;
}

/* ======================================================================
   Helper: convert Windows BGR (bottom-up) to stb RGB (top-down)
   ====================================================================== */
static unsigned char * bgr_bottomup_to_rgb_topdown(BYTE * bgr, int w, int h)
{
    unsigned char * out = (unsigned char *)malloc(w * h * 3);
    int row, col;
    if (!out) return NULL;
    for (row = 0; row < h; row++)
    {
        unsigned char * dst_row = out + row * w * 3;
        BYTE * src_row = bgr + (h - 1 - row) * w * 3;
        for (col = 0; col < w; col++)
        {
            dst_row[col * 3 + 0] = src_row[col * 3 + 2]; /* R */
            dst_row[col * 3 + 1] = src_row[col * 3 + 1]; /* G */
            dst_row[col * 3 + 2] = src_row[col * 3 + 0]; /* B */
        }
    }
    return out;
}

/* ======================================================================
   Read BMP/JPG to RGB buffer
   ====================================================================== */
void * _ISReadBMPToRGB(HISSRC h, DWORD * pw, DWORD * ph)
{
    int w, h2, channels;
    unsigned char * data;
    BYTE * result;
    if (!h || !pw || !ph) return NULL;
    data = stbi_load(h->szPath, &w, &h2, &channels, 3);
    if (!data) return NULL;
    result = rgb_topdown_to_bgr_bottomup(data, w, h2);
    stbi_image_free(data);
    *pw = (DWORD)w;
    *ph = (DWORD)h2;
    return result;
}

void * _ISReadJPGToRGB(HISSRC h, DWORD * pw, DWORD * ph)
{
    return _ISReadBMPToRGB(h, pw, ph); /* stb handles both */
}

/* ======================================================================
   Write RGB buffer to JPG
   ====================================================================== */
int _ISWriteRGBToJPG(HISDEST hd, void * pData, int nWidth, int nHeight, int nQuality, int nFlags)
{
    unsigned char * rgb;
    int result;
    (void)nFlags;
    if (!hd || !pData || nWidth <= 0 || nHeight <= 0) return 0;
    rgb = bgr_bottomup_to_rgb_topdown((BYTE *)pData, nWidth, nHeight);
    if (!rgb) return 0;
    result = stbi_write_jpg(hd->szPath, nWidth, nHeight, 3, rgb, nQuality);
    free(rgb);
    return result;
}

/* ======================================================================
   Draw RGB buffer to a device context (using GDI StretchDIBits)
   ====================================================================== */
void _ISDrawRGB(HDC hDC, void * pData, int srcW, int srcH,
                int dstX, int dstY, int dstW, int dstH, void * reserved)
{
    BITMAPINFO bmi;
    (void)reserved;
    if (!hDC || !pData || srcW <= 0 || srcH <= 0) return;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = srcW;
    bmi.bmiHeader.biHeight = srcH; /* positive = bottom-up */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(hDC, dstX, dstY, dstW, dstH,
                  0, 0, srcW, srcH,
                  pData, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

/* ======================================================================
   Overlay one RGB image onto another with transparency color key
   ====================================================================== */
void _ISOverlayRGBTrans(void * pDst, int dstW, int dstH,
                        void * pSrc, int srcW, int srcH,
                        int x, int y, double alpha, DWORD transColor)
{
    int row, col;
    BYTE transB, transG, transR;
    if (!pDst || !pSrc) return;

    /* transColor is 0x00RRGGBB but in BGR buffer it's stored as B,G,R */
    transR = (BYTE)((transColor >> 16) & 0xFF);
    transG = (BYTE)((transColor >> 8) & 0xFF);
    transB = (BYTE)(transColor & 0xFF);

    for (row = 0; row < srcH; row++)
    {
        int dstRow = y + row;
        if (dstRow < 0 || dstRow >= dstH) continue;
        for (col = 0; col < srcW; col++)
        {
            int dstCol = x + col;
            BYTE * sp, *dp;
            if (dstCol < 0 || dstCol >= dstW) continue;
            sp = (BYTE *)pSrc + (srcH - 1 - row) * srcW * 3 + col * 3;
            dp = (BYTE *)pDst + (dstH - 1 - dstRow) * dstW * 3 + dstCol * 3;
            /* Check transparency (BGR order in buffer) */
            if (sp[0] == transB && sp[1] == transG && sp[2] == transR)
                continue;
            if (alpha >= 1.0)
            {
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
            else
            {
                dp[0] = (BYTE)(sp[0] * alpha + dp[0] * (1.0 - alpha));
                dp[1] = (BYTE)(sp[1] * alpha + dp[1] * (1.0 - alpha));
                dp[2] = (BYTE)(sp[2] * alpha + dp[2] * (1.0 - alpha));
            }
        }
    }
}

/* ======================================================================
   Resize RGB image (upscale/general)
   ====================================================================== */
void _ISResizeRGB(void * pSrc, int srcW, int srcH,
                  void * pDst, int dstW, int dstH)
{
    /* Convert BGR bottom-up to RGB top-down, resize, convert back */
    unsigned char * rgb_src, * rgb_dst;
    int row, col;
    if (!pSrc || !pDst || srcW <= 0 || srcH <= 0 || dstW <= 0 || dstH <= 0) return;

    rgb_src = bgr_bottomup_to_rgb_topdown((BYTE *)pSrc, srcW, srcH);
    if (!rgb_src) return;
    rgb_dst = (unsigned char *)malloc(dstW * dstH * 3);
    if (!rgb_dst) { free(rgb_src); return; }

    stbir_resize_uint8_linear(rgb_src, srcW, srcH, 0,
                              rgb_dst, dstW, dstH, 0, STBIR_RGB);

    /* Convert back to BGR bottom-up in the destination buffer */
    for (row = 0; row < dstH; row++)
    {
        unsigned char * src_row = rgb_dst + row * dstW * 3;
        BYTE * dst_row = (BYTE *)pDst + (dstH - 1 - row) * dstW * 3;
        for (col = 0; col < dstW; col++)
        {
            dst_row[col * 3 + 0] = src_row[col * 3 + 2]; /* B */
            dst_row[col * 3 + 1] = src_row[col * 3 + 1]; /* G */
            dst_row[col * 3 + 2] = src_row[col * 3 + 0]; /* R */
        }
    }
    free(rgb_src);
    free(rgb_dst);
}

/* ======================================================================
   Decimate RGB image (downscale — same as resize)
   ====================================================================== */
void _ISDecimateRGB(void * pSrc, int srcW, int srcH,
                    void * pDst, int dstW, int dstH)
{
    _ISResizeRGB(pSrc, srcW, srcH, pDst, dstW, dstH);
}

/* ======================================================================
   Draw text onto an RGB buffer using GDI
   ====================================================================== */
void _ISDrawTextOnRGB2(void * pImage, int nWidth, int nHeight,
                       const char * szText, LOGFONTA * pLogFont,
                       int nX, int nY, DWORD dwColor)
{
    HDC hDC, hMemDC;
    HBITMAP hBmp, hOldBmp;
    HFONT hFont, hOldFont;
    BITMAPINFO bmi;
    RECT rc;
    int row, col;
    BYTE * pBits;

    if (!pImage || !szText || nWidth <= 0 || nHeight <= 0) return;
    if (!szText[0]) return;

    hDC = GetDC(NULL);
    hMemDC = CreateCompatibleDC(hDC);

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = nWidth;
    bmi.bmiHeader.biHeight = nHeight; /* bottom-up */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    hBmp = CreateDIBSection(hMemDC, &bmi, DIB_RGB_COLORS, (void **)&pBits, NULL, 0);
    if (!hBmp) { DeleteDC(hMemDC); ReleaseDC(NULL, hDC); return; }

    hOldBmp = (HBITMAP)SelectObject(hMemDC, hBmp);

    /* Copy existing image into the DIB so text overlays on it */
    memcpy(pBits, pImage, nWidth * nHeight * 3);

    hFont = pLogFont ? CreateFontIndirectA(pLogFont) : (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    hOldFont = (HFONT)SelectObject(hMemDC, hFont);

    SetBkMode(hMemDC, TRANSPARENT);
    SetTextColor(hMemDC, (COLORREF)dwColor);

    /* GDI coordinates in a DIB section are top-down whatever the row order in memory */
    rc.left = nX; rc.top = nY;
    rc.right = nWidth - 2; rc.bottom = nHeight;
    DrawTextA(hMemDC, szText, -1, &rc, DT_LEFT | DT_TOP | DT_NOPREFIX);

    SelectObject(hMemDC, hOldFont);
    if (pLogFont && hFont) DeleteObject(hFont);

    GdiFlush();

    /* Copy back */
    memcpy(pImage, pBits, nWidth * nHeight * 3);

    SelectObject(hMemDC, hOldBmp);
    DeleteObject(hBmp);
    DeleteDC(hMemDC);
    ReleaseDC(NULL, hDC);
}

/* ======================================================================
   Convert HBITMAP to RGB buffer
   ====================================================================== */
BYTE * _ISHBITMAPToRGB(HBITMAP hBitmap, int * pWidth, int * pHeight, HDC hDC, void * reserved)
{
    BITMAP bm;
    BITMAPINFO bmi;
    BYTE * pBits;
    HDC hMemDC;
    (void)reserved;

    if (!hBitmap || !pWidth || !pHeight) return NULL;
    GetObject(hBitmap, sizeof(bm), &bm);

    *pWidth = bm.bmWidth;
    *pHeight = bm.bmHeight;

    pBits = (BYTE *)GlobalAlloc(GPTR, bm.bmWidth * bm.bmHeight * 3);
    if (!pBits) return NULL;

    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = bm.bmWidth;
    bmi.bmiHeader.biHeight = bm.bmHeight; /* bottom-up */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    hMemDC = hDC ? hDC : GetDC(NULL);
    GetDIBits(hMemDC, hBitmap, 0, bm.bmHeight, pBits, &bmi, DIB_RGB_COLORS);
    if (!hDC) ReleaseDC(NULL, hMemDC);

    return pBits;
}

/* ======================================================================
   Load bitmap from resources
   ====================================================================== */
HBITMAP _ISLoadResourceBitmap(HINSTANCE hInst, const char * szName, void * reserved)
{
    (void)reserved;
    if (!hInst || !szName) return NULL;
    return LoadBitmapA(hInst, szName);
}
