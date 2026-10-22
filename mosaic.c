#ifdef PRO

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <math.h>

#include "TSReader.h"
#include "bcdmux.h"
#include "util.h"
#include "resource.h"

extern PVARIABLES v;
extern char gszAppName[];
extern char gszVideoMosaicClass[];

void ValidateWindowXY(int * nX, int * nY);
void PlayWithDefaultPlayer();

int nOneToOneWidth;
int nOneToOneHeight;
BOOL fPreferRows;

#define MAX_MOSAIC_THUMBNAILS 256
typedef struct _tagMosaicThumbnails
{
	int nProgramNumber;
	RECT rc;
} MOSAICTHUMBNAILS, *PMOSAICTHUMBNAILS;

MOSAICTHUMBNAILS mt[MAX_MOSAIC_THUMBNAILS];

void GetThumbnailMaxWidthAndHeight(int * nMaxWidth, int * nMaxHeight)
{
	int nPMTIndex, nESIndex;

	*nMaxWidth = 0;
	*nMaxHeight = 0;

	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
		{
			if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
				break;
			switch(v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType)
			{
			case 0x01:	// MPEG-1
			case 0x02:	// MPEG-2
			case 0x10:	// MPEG-4
			case 0x1b:	// H264
			case 0x80:	// DCII
			case 0xea:	// VC1
				if (v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame != NULL)
				{
					if (v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth > *nMaxWidth)
						*nMaxWidth = v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth;
					if (v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight > *nMaxHeight)
						*nMaxHeight = v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight;
				}
				break;
			}
		}
	}
}

int GetVideoThumbnailCount()
{
	int nPMTIndex, nESIndex;
	int nVideoThumbnails = 0;

	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
		{
			if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
				break;
			switch(v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType)
			{
			case 0x01:	// MPEG-1
			case 0x02:	// MPEG-2
			case 0x10:	// MPEG-4
			case 0x1b:	// H264
			case 0x80:	// DCII
			case 0xea:	// VC1
				if (v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame != NULL)
					nVideoThumbnails++;
				break;
			}
		}
	}

	return nVideoThumbnails;
}

void PaintVideoMosaic(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
	int nPMTIndex, nESIndex;
	int nCurrentX, nCurrentY;
	int nVideoThumbnails = GetVideoThumbnailCount();
	int nRows, nCols;
	int nMaxWidth, nMaxHeight;
	int nTargetWidth, nTargetHeight;
	int nThumbnailIndex = 0;
	BYTE * pNewImage;
	double dWidthRatio, dHeightRatio;
	PAINTSTRUCT ps;
	RECT rc;
	HDC hRealDC, hDC;
	HBITMAP memBM;

	nCurrentX = nCurrentY = 0;
	GetClientRect(hWnd, &rc);
	hRealDC = BeginPaint(hWnd, &ps);
	hDC = CreateCompatibleDC(hRealDC);
    memBM = CreateCompatibleBitmap (hRealDC, rc.right, rc.bottom);
    SelectObject(hDC, memBM);

	nRows = nCols = (int)sqrt(nVideoThumbnails);
	if (nRows * nCols < nVideoThumbnails)
	{
		if (fPreferRows)
		{
			nRows++;
			if (nRows * nCols < nVideoThumbnails)
				nCols++;
		}
		else
		{
			nCols++;
			if (nRows * nCols < nVideoThumbnails)
				nRows++;
		}
	}
	GetThumbnailMaxWidthAndHeight(&nMaxWidth, &nMaxHeight);
	nOneToOneWidth = nCols * nMaxWidth;
	nOneToOneHeight = nRows * nMaxHeight;
	
	dWidthRatio = ((double)rc.right / (double)nCols) / (double)nMaxWidth;
	dHeightRatio = ((double)rc.bottom / (double)nRows) / (double)nMaxHeight;
	nTargetWidth = (int)(dWidthRatio * (double)nMaxWidth);
	nTargetHeight = (int)(dHeightRatio * (double)nMaxHeight);
	pNewImage = LocalAlloc(LMEM_FIXED, 3 * nTargetWidth * nTargetHeight);

	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
		{
			if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
				break;
			switch(v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType)
			{
			case 0x01:	// MPEG-1
			case 0x02:	// MPEG-2
			case 0x10:	// MPEG-4
			case 0x1b:	// H264
			case 0x80:	// DCII
			case 0xea:	// VC1
				if (v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame != NULL)
				{
					BYTE * pDrawImage = pNewImage;

					if (nCurrentY + nTargetHeight > rc.bottom)
					{
						nCurrentY = 0;
						nCurrentX += nTargetWidth;
					}
					//if (dWidthRatio > 1.0 || dHeightRatio > 1.0)
					if (   nTargetWidth > v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth
						|| nTargetHeight > v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight)
					{
						_ISResizeRGB(v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame,
									 v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth, v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight,
									 pNewImage,
									 nTargetWidth, nTargetHeight);
					}
					else if (dWidthRatio != 1.0 || dHeightRatio != 1.0)
					{
						_ISDecimateRGB(v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame,
									   v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth, v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight,
									   pNewImage,
									   nTargetWidth, nTargetHeight);
					}
					else
					{
						pDrawImage = v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame;
						/*memset(pNewImage, 0, nTargetWidth * nTargetHeight * 3);
						memcpy(pNewImage,
							   ,
							   v->pat.pmt[nPMTIndex].es[nESIndex].nVideoWidth * v->pat.pmt[nPMTIndex].es[nESIndex].nVideoHeight * 3);*/
					}
					_ISDrawRGB(hDC,
							   pDrawImage,
							   nTargetWidth, nTargetHeight,
							   nCurrentX, nCurrentY,
							   nTargetWidth, nTargetHeight,
							   NULL);
					mt[nThumbnailIndex].nProgramNumber = v->pat.pmt[nPMTIndex].nProgramNumber;
					mt[nThumbnailIndex].rc.top = nCurrentY;
					mt[nThumbnailIndex].rc.left = nCurrentX;
					mt[nThumbnailIndex].rc.bottom = nCurrentY + nTargetHeight;
					mt[nThumbnailIndex].rc.right = nCurrentX + nTargetWidth;
					nThumbnailIndex++;
					nCurrentY += nTargetHeight;
				}
				break;
			}
		}
	}
	mt[nThumbnailIndex].nProgramNumber = 0;

	LocalFree(pNewImage);
	BitBlt(hRealDC, 0, 0, rc.right, rc.bottom, hDC, 0, 0, SRCCOPY);
	DeleteObject(memBM);
	DeleteDC(hDC);
	EndPaint(hWnd, &ps);
}

LRESULT FAR PASCAL VideoMosaicWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	static BOOL fClosing;

	switch(uMsg)
	{
	case WM_CREATE:
		fClosing = FALSE;
		nOneToOneWidth = nOneToOneHeight = -1;
		fPreferRows = FALSE;
		mt[0].nProgramNumber = 0;
		break;
	case WM_DESTROY:
		if (!v->fMosaicMinimizedFlag && !v->fMosaicMaximizedFlag)
		{
			RECT rcMosaicWindow;

			GetWindowRect(hWnd, &rcMosaicWindow);
			v->nMosaicWindowX = rcMosaicWindow.left;
			v->nMosaicWindowY = rcMosaicWindow.top;
			v->nMosaicWindowW = rcMosaicWindow.right - rcMosaicWindow.left;
			v->nMosaicWindowH = rcMosaicWindow.bottom - rcMosaicWindow.top;
		}
		v->hWndVideoMosaic = NULL;
		SetForegroundWindow(v->hWndMainWindow);
		break;
	case WM_SIZE:
		if (!fClosing)
		{
			v->fMosaicMinimizedFlag = wParam == SIZE_MINIMIZED;
			v->fMosaicMaximizedFlag = wParam == SIZE_MAXIMIZED;		
			InvalidateRect(hWnd, NULL, FALSE);
		}
		break;
	case WM_CLOSE:
		fClosing = TRUE;
		DestroyWindow(hWnd);
		break;
	case WM_PAINT:
		PaintVideoMosaic(hWnd, wParam, lParam);
		break;
	case WM_KEYDOWN:
		switch(wParam)
		{
		case VK_ESCAPE:
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			break;
		case '1':
			if (nOneToOneWidth != -1 && nOneToOneHeight != -1)
			{
				RECT rc;

				rc.top = rc.left = 0;
				rc.right = nOneToOneWidth;
				rc.bottom = nOneToOneHeight;
				AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_SIZEBOX, FALSE);
				rc.right += GetSystemMetrics(SM_CXSIZEFRAME);
				rc.bottom += GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CYCAPTION);
				SetWindowPos(hWnd, HWND_TOP, 0, 0, rc.right, rc.bottom, SWP_NOMOVE);
			}
			break;
		case '2':
			fPreferRows = FALSE;
			InvalidateRect(hWnd, NULL, FALSE);
			break;
		case '3':
			fPreferRows = TRUE;
			InvalidateRect(hWnd, NULL, FALSE);
			break;
		case VK_F1:
			MessageBox(hWnd, "Keys available in the Video Mosaic:\n"
				             "\n"
							 "1\tResize window for a 1:1 zoom on thumbnails\n"
							 "2\tRearrange with preference for columns\n"
							 "3\tRearrange with preference for rows\n"
							 "\n",
							 gszAppName, MB_ICONINFORMATION);
			break;
		}
		break;
//	case WM_ACTIVATE:
//		InvalidateThumbnails();
//		break;
	case WM_LBUTTONDBLCLK:
		{
			int nThumbnailIndex;
			int x = LOWORD(lParam);
			int y = HIWORD(lParam);

			for (nThumbnailIndex = 0; nThumbnailIndex < MAX_MOSAIC_THUMBNAILS; nThumbnailIndex++)
			{
				if (mt[nThumbnailIndex].nProgramNumber == 0)
					break;
				if (x >= mt[nThumbnailIndex].rc.left && x <= mt[nThumbnailIndex].rc.right
					&& y >= mt[nThumbnailIndex].rc.top && y <= mt[nThumbnailIndex].rc.bottom)
				{
					int nPMTIndex;
					HWND hWndTV = GetDlgItem(v->hDlgSIParser, IDC_SI_TREE);

					for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
					{
						if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
							break;
						if (v->pat.pmt[nPMTIndex].nProgramNumber == mt[nThumbnailIndex].nProgramNumber)
							break;
					}
					TreeView_SelectItem(hWndTV, v->pat.pmt[nPMTIndex].hPMTTreeItem);
					TreeView_SelectSetFirstVisible(hWndTV, v->pat.pmt[nPMTIndex].hPMTTreeItem);
					if (v->nSelectedProgram != -1)
						PlayWithDefaultPlayer();
					break;
				}
			}
		}
		break;
	default:
		return(DefWindowProc(hWnd, uMsg, wParam, lParam));
	}

	return 0;
}

void ShowVideoMosaic(HWND hWnd)
{
	DWORD dwStyle;
	int nWidth, nHeight, nX, nY;
	ATOM rcreturn;
	WNDCLASS  wc;
	char szTitle[256];

	wsprintf(szTitle, "%s Video Mosaic", gszAppName);

	// Setup the window classes
	memset(&wc, 0, sizeof(wc));
	wc.style =			CS_VREDRAW | CS_HREDRAW | CS_DBLCLKS;
	wc.lpfnWndProc =	VideoMosaicWndProc;
	wc.cbClsExtra =		0;
	wc.cbWndExtra =		0;
	wc.hInstance =		v->hInstance;
	wc.hIcon =			LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_DVBSMALL_LOGO));
	wc.hCursor =		LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground =	GetStockObject(BLACK_BRUSH); 
	wc.lpszMenuName =	NULL;//MAKEINTRESOURCE(IDR_TEST);
	wc.lpszClassName =	gszVideoMosaicClass;
	rcreturn = RegisterClass(&wc);

	dwStyle = WS_OVERLAPPED | WS_CAPTION | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU | WS_SIZEBOX;
	nWidth = nHeight = nX = nY = CW_USEDEFAULT;	
	if (v->nMosaicWindowW && v->nMosaicWindowH)
	{
		nWidth = v->nMosaicWindowW;
		nHeight = v->nMosaicWindowH;
		nX = v->nMosaicWindowX;
		nY = v->nMosaicWindowY;
	}	
	ValidateWindowXY(&nX, &nY);
	v->hWndVideoMosaic = CreateWindow(gszVideoMosaicClass,
						   szTitle,
						   dwStyle,
						   nX, nY,
						   nWidth, nHeight,
						   NULL,
						   NULL,
						   v->hInstance,
						   0);
	if (v->hWndVideoMosaic == NULL)
	{
		char szTemp[256];

		wsprintf(szTemp, "CreateWindow for the Video Mosaic failed with GetLastError() = %d", GetLastError());
		MessageBox(hWnd, szTemp, gszAppName, MB_OK);
	}
	else
	{
		if (v->fMosaicMaximizedFlag)
			ShowWindow(v->hWndVideoMosaic, SW_MAXIMIZE);
		else
			ShowWindow(v->hWndVideoMosaic, SW_SHOW);
	}
	return;
}



#endif PRO
