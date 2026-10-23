// EIT guide for the embedded FFplay video window
//
// ffplay draws the video in its own SDL window, which TSReader cannot draw on, so the
// guide sits next to the video inside the host window:
//   - a strip below the video with the focused event (normally the one on now), its
//     progress and description, and the event after it
//   - an optional schedule list on the right with every EIT event of the service and
//     the full description of the selected one
// PgUp / PgDn step the focus through the events, Home goes back to now, L shows or hides
// the list and E opens the EPG grid on this service. Those keys go to ffplay's window
// (another process), so while the video window is in front a low-level keyboard hook
// takes them before ffplay does.
//
// Events come from TSReader's own EIT data (v->pEvents, under csEIT) and "now" is the
// EPG time source (GetEPGNow), so recordings show what was on when they were made.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "TSReader.h"
#include "resource.h"
#include "util.h"

extern PVARIABLES v;

void SetEPGOffsetNearChannel(int nProgram);
void FFplayEPG_ToggleList();
void FillEITLanguageMenu(HMENU hPopup, UINT uBase);
BOOL SetEITLanguageFromMenu(UINT nID, UINT uBase);

#define ID_FFPLAY_EPG_PREVIOUS		43000
#define ID_FFPLAY_EPG_NEXT			43001
#define ID_FFPLAY_EPG_NOW			43002
#define ID_FFPLAY_EPG_LIST			43003
#define ID_FFPLAY_EPG_GRID			43004
#define ID_FFPLAY_EPG_LANGUAGE		43100	// + 0..16, see FillEITLanguageMenu

#define WM_FFPLAY_EPG_KEY			(WM_USER + 20)	// wParam = virtual key taken by the hook

#define IDC_EPG_STRIP				100
#define IDC_EPG_LIST				101
#define IDC_EPG_DETAILS				102

#define EPG_TIMER_ID				0xE9C
#define EPG_MAX_EVENTS				1000
#define EPG_LIST_WIDTH				380

typedef struct
{
	DWORD64 lnStart;					// UTC FILETIME
	DWORD64 lnEnd;
	int nEventID;
	char szName[256];
	char szShort[512];
	char * szLong;						// LocalAlloc'd copy or NULL
} GUIDEEVENT;

static HWND hWndHost, hWndStrip, hWndList, hWndDetails;
static HFONT hFontNormal, hFontBold;
static HHOOK hKeyHook;
static HMENU hMenuLanguage;
static int nService;
static GUIDEEVENT * pGuide;
static int nGuideEvents;
static DWORD dwGuideSignature;			// detects EIT changes so the list is rebuilt only then
static int nFocus = -1;					// focused event index, -1 = follow now
static BOOL fFollowNow = TRUE;
static BOOL fListVisible;
static int nStripHeight;

static char gszStripClass[] = {"TSReaderFFplayEPGStrip"};

static void FreeGuide()
{
	int i;

	for (i = 0; i < nGuideEvents; i++)
		if (pGuide[i].szLong != NULL)
			LocalFree(pGuide[i].szLong);
	nGuideEvents = 0;
}

static int CompareGuideEvents(const void * p1, const void * p2)
{
	const GUIDEEVENT * e1 = (const GUIDEEVENT *)p1;
	const GUIDEEVENT * e2 = (const GUIDEEVENT *)p2;

	if (e1->lnStart < e2->lnStart)
		return -1;
	return e1->lnStart > e2->lnStart ? 1 : 0;
}

// A cheap fingerprint of the service's event list, taken under csEIT
static DWORD GuideSignature()
{
	DWORD dwSignature = 0;
	PEITEVENT pEvent;

	EnterCriticalSection(&v->csEIT);
	for (pEvent = v->pEvents[nService]; pEvent != NULL; pEvent = (PEITEVENT)pEvent->dwNextEvent)
	{
		dwSignature = dwSignature * 31 + pEvent->nEventID;
		dwSignature = dwSignature * 31 + pEvent->stStartTime.wMinute + pEvent->stStartTime.wHour * 60 + pEvent->stStartTime.wDay * 1440;
		dwSignature = dwSignature * 31 + lstrlen(pEvent->szEventName);
		if (pEvent->szLongEventDescription != NULL)
			dwSignature++;
	}
	LeaveCriticalSection(&v->csEIT);
	return dwSignature;
}

// Copies the service's events out of TSReader's EIT lists and sorts them by start time
static void LoadGuide()
{
	PEITEVENT pEvent;

	FreeGuide();
	EnterCriticalSection(&v->csEIT);
	for (pEvent = v->pEvents[nService]; pEvent != NULL && nGuideEvents < EPG_MAX_EVENTS; pEvent = (PEITEVENT)pEvent->dwNextEvent)
	{
		GUIDEEVENT * pGuideEvent = &pGuide[nGuideEvents];
		DWORD64 lnDuration = ((DWORD64)pEvent->stRunTime.wHour * 3600 + pEvent->stRunTime.wMinute * 60 + pEvent->stRunTime.wSecond) * 10000000;
		int i;

		if (!SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&pGuideEvent->lnStart))
			continue;
		// The same event can be listed twice (p/f and schedule tables)
		for (i = 0; i < nGuideEvents; i++)
			if (pGuide[i].nEventID == pEvent->nEventID && pGuide[i].lnStart == pGuideEvent->lnStart)
				break;
		if (i < nGuideEvents)
			continue;
		pGuideEvent->lnEnd = pGuideEvent->lnStart + lnDuration;
		pGuideEvent->nEventID = pEvent->nEventID;
		lstrcpyn(pGuideEvent->szName, pEvent->szEventName, sizeof(pGuideEvent->szName));
		pGuideEvent->szShort[0] = '\0';
		if (pEvent->szShortEventDescription != NULL)
			lstrcpyn(pGuideEvent->szShort, pEvent->szShortEventDescription, sizeof(pGuideEvent->szShort));
		pGuideEvent->szLong = NULL;
		if (pEvent->szLongEventDescription != NULL && lstrlen(pEvent->szLongEventDescription))
		{
			pGuideEvent->szLong = LocalAlloc(LPTR, lstrlen(pEvent->szLongEventDescription) + 1);
			if (pGuideEvent->szLong != NULL)
				lstrcpy(pGuideEvent->szLong, pEvent->szLongEventDescription);
		}
		nGuideEvents++;
	}
	LeaveCriticalSection(&v->csEIT);
	qsort(pGuide, nGuideEvents, sizeof(GUIDEEVENT), CompareGuideEvents);
}

// The event on now, else the next one to start, else the last one
static int CurrentEventIndex(DWORD64 lnNow)
{
	int i;

	for (i = 0; i < nGuideEvents; i++)
		if (lnNow < pGuide[i].lnEnd)
			return i;
	return nGuideEvents - 1;
}

static void FormatLocalTime(DWORD64 lnTime, char * szOut, BOOL fWithDate)
{
	SYSTEMTIME st, stLocal;
	static const char * szDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

	FileTimeToSystemTime((FILETIME *)&lnTime, &st);
	SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
	if (fWithDate)
		wsprintf(szOut, "%s %d.%d. %02d:%02d", szDays[stLocal.wDayOfWeek], stLocal.wDay, stLocal.wMonth, stLocal.wHour, stLocal.wMinute);
	else
		wsprintf(szOut, "%02d:%02d", stLocal.wHour, stLocal.wMinute);
}

static BOOL SameLocalDay(DWORD64 ln1, DWORD64 ln2)
{
	SYSTEMTIME st1, st2, stLocal1, stLocal2;

	FileTimeToSystemTime((FILETIME *)&ln1, &st1);
	FileTimeToSystemTime((FILETIME *)&ln2, &st2);
	SystemTimeToTzSpecificLocalTime(NULL, &st1, &stLocal1);
	SystemTimeToTzSpecificLocalTime(NULL, &st2, &stLocal2);
	return stLocal1.wYear == stLocal2.wYear && stLocal1.wMonth == stLocal2.wMonth && stLocal1.wDay == stLocal2.wDay;
}

static const char * EventLabel(int nIndex, DWORD64 lnNow)
{
	int nCurrent = CurrentEventIndex(lnNow);

	if (lnNow >= pGuide[nIndex].lnStart && lnNow < pGuide[nIndex].lnEnd)
		return "Now";
	if (pGuide[nIndex].lnEnd <= lnNow)
		return "Earlier";
	if (nIndex == nCurrent || (nIndex == nCurrent + 1 && lnNow >= pGuide[nCurrent].lnStart))
		return "Next";
	return "Later";
}

static int FocusedEvent(DWORD64 lnNow)
{
	if (nGuideEvents == 0)
		return -1;
	if (fFollowNow || nFocus < 0 || nFocus >= nGuideEvents)
		return CurrentEventIndex(lnNow);
	return nFocus;
}

// Description text for the details box: times, name, short and long description
static void UpdateDetails(int nIndex)
{
	char * szText;
	char szStart[32], szEnd[32];
	int nLength;

	if (hWndDetails == NULL)
		return;
	if (nIndex < 0 || nIndex >= nGuideEvents)
	{
		SetWindowText(hWndDetails, "");
		return;
	}
	nLength = 1024 + lstrlen(pGuide[nIndex].szShort) + (pGuide[nIndex].szLong ? lstrlen(pGuide[nIndex].szLong) * 2 : 0);
	szText = LocalAlloc(LPTR, nLength);
	if (szText == NULL)
		return;
	FormatLocalTime(pGuide[nIndex].lnStart, szStart, TRUE);
	FormatLocalTime(pGuide[nIndex].lnEnd, szEnd, FALSE);
	_snprintf(szText, nLength - 1, "%s - %s  (%d min)\r\n%s\r\n\r\n%s", szStart, szEnd,
			  (int)((pGuide[nIndex].lnEnd - pGuide[nIndex].lnStart) / 600000000), pGuide[nIndex].szName, pGuide[nIndex].szShort);
	if (pGuide[nIndex].szLong != NULL)
	{
		// EIT text uses bare LF (or CR) line breaks; the edit control wants CR LF
		char * pOut = szText + lstrlen(szText);
		const char * pIn = pGuide[nIndex].szLong;

		*pOut++ = '\r';
		*pOut++ = '\n';
		*pOut++ = '\r';
		*pOut++ = '\n';
		for (; *pIn && pOut < szText + nLength - 3; pIn++)
		{
			if (*pIn == '\n' || *pIn == '\r')
			{
				*pOut++ = '\r';
				*pOut++ = '\n';
				if (*pIn == '\r' && pIn[1] == '\n')
					pIn++;
			}
			else
				*pOut++ = *pIn;
		}
		*pOut = '\0';
	}
	SetWindowText(hWndDetails, szText);
	LocalFree(szText);
}

static void FillList()
{
	DWORD64 lnNow;
	int i;

	if (hWndList == NULL)
		return;
	GetEPGNow(&lnNow);
	SendMessage(hWndList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(hWndList);
	for (i = 0; i < nGuideEvents; i++)
	{
		LVITEM lvi;
		char szTime[32], szDuration[16];

		FormatLocalTime(pGuide[i].lnStart, szTime, TRUE);
		wsprintf(szDuration, "%d", (int)((pGuide[i].lnEnd - pGuide[i].lnStart) / 600000000));
		memset(&lvi, 0, sizeof(lvi));
		lvi.mask = LVIF_TEXT | LVIF_PARAM;
		lvi.iItem = i;
		lvi.pszText = szTime;
		lvi.lParam = i;
		ListView_InsertItem(hWndList, &lvi);
		ListView_SetItemText(hWndList, i, 1, szDuration);
		ListView_SetItemText(hWndList, i, 2, pGuide[i].szName);
	}
	SendMessage(hWndList, WM_SETREDRAW, TRUE, 0);
}

// Moves the list selection to the focused event (without making it take the focus)
static void SyncListToFocus()
{
	DWORD64 lnNow;
	int nIndex;

	GetEPGNow(&lnNow);
	nIndex = FocusedEvent(lnNow);
	if (hWndList != NULL && nIndex >= 0)
	{
		ListView_SetItemState(hWndList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_SetItemState(hWndList, nIndex, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(hWndList, nIndex, FALSE);
	}
	UpdateDetails(nIndex);
}

static void RefreshGuide(BOOL fForce)
{
	DWORD dwSignature = GuideSignature();

	if (fForce || dwSignature != dwGuideSignature)
	{
		DWORD64 lnNow;
		int nOldFocusID = -1;
		DWORD64 lnOldFocusStart = 0;
		int i;

		GetEPGNow(&lnNow);
		if (!fFollowNow && nFocus >= 0 && nFocus < nGuideEvents)
		{
			nOldFocusID = pGuide[nFocus].nEventID;
			lnOldFocusStart = pGuide[nFocus].lnStart;
		}
		dwGuideSignature = dwSignature;
		LoadGuide();
		// Keep the user's focused event across a reload
		if (nOldFocusID != -1)
		{
			fFollowNow = TRUE;
			for (i = 0; i < nGuideEvents; i++)
			{
				if (pGuide[i].nEventID == nOldFocusID && pGuide[i].lnStart == lnOldFocusStart)
				{
					nFocus = i;
					fFollowNow = FALSE;
					break;
				}
			}
		}
		FillList();
		SyncListToFocus();
	}
	InvalidateRect(hWndStrip, NULL, FALSE);
}

static void StepFocus(int nDelta)
{
	DWORD64 lnNow;
	int nIndex;

	if (nGuideEvents == 0)
		return;
	GetEPGNow(&lnNow);
	nIndex = FocusedEvent(lnNow) + nDelta;
	if (nIndex < 0)
		nIndex = 0;
	if (nIndex >= nGuideEvents)
		nIndex = nGuideEvents - 1;
	nFocus = nIndex;
	fFollowNow = (nIndex == CurrentEventIndex(lnNow));
	SyncListToFocus();
	InvalidateRect(hWndStrip, NULL, FALSE);
}

static void FocusNow()
{
	fFollowNow = TRUE;
	nFocus = -1;
	SyncListToFocus();
	InvalidateRect(hWndStrip, NULL, FALSE);
}

static void DrawTextLine(HDC hDC, HFONT hFont, COLORREF cr, const char * szText, RECT * prc, UINT uFormat)
{
	SelectObject(hDC, hFont);
	SetTextColor(hDC, cr);
	DrawText(hDC, szText, -1, prc, uFormat | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_VCENTER);
}

static void PaintStrip(HWND hWnd, HDC hDC)
{
	RECT rcClient, rc;
	HBRUSH hBackground = CreateSolidBrush(RGB(24, 28, 36));
	HFONT hOldFont;
	DWORD64 lnNow;
	int nIndex, nLine, nPad = 8;
	TEXTMETRIC tm;
	char szLine[600], szStart[32], szEnd[32];

	GetClientRect(hWnd, &rcClient);
	FillRect(hDC, &rcClient, hBackground);
	DeleteObject(hBackground);
	SetBkMode(hDC, TRANSPARENT);
	hOldFont = SelectObject(hDC, hFontBold);
	GetTextMetrics(hDC, &tm);
	nLine = tm.tmHeight + 2;

	GetEPGNow(&lnNow);
	nIndex = FocusedEvent(lnNow);

	// Line 1: the focused event, with a hint on the right
	rc.left = nPad;
	rc.right = rcClient.right - nPad;
	rc.top = 4;
	rc.bottom = rc.top + nLine;
	// The key hint only when there is room for it next to the event name
	if (rcClient.right >= 820)
	{
		DrawTextLine(hDC, hFontNormal, RGB(120, 130, 150), "PgUp/PgDn events   Home now   L list   E grid", &rc, DT_RIGHT);
		rc.right -= 330;
	}
	if (nIndex < 0)
	{
		wsprintf(szLine, "No EIT information for service %d yet", nService);
		DrawTextLine(hDC, hFontBold, RGB(200, 200, 200), szLine, &rc, DT_LEFT);
	}
	else
	{
		const GUIDEEVENT * pEvent = &pGuide[nIndex];
		const char * szLabel = EventLabel(nIndex, lnNow);
		BOOL fOnNow = lnNow >= pEvent->lnStart && lnNow < pEvent->lnEnd;

		FormatLocalTime(pEvent->lnStart, szStart, !SameLocalDay(pEvent->lnStart, lnNow));
		FormatLocalTime(pEvent->lnEnd, szEnd, FALSE);
		_snprintf(szLine, sizeof(szLine) - 1, "%s   %s - %s   %s", szLabel, szStart, szEnd, pEvent->szName);
		szLine[sizeof(szLine) - 1] = '\0';
		DrawTextLine(hDC, hFontBold, fOnNow ? RGB(255, 210, 90) : RGB(140, 200, 255), szLine, &rc, DT_LEFT);

		// Progress bar of the event on now (or how long until a later one starts)
		rc.left = nPad;
		rc.right = rcClient.right - nPad;
		rc.top = rc.bottom + 1;
		rc.bottom = rc.top + 4;
		{
			HBRUSH hTrack = CreateSolidBrush(RGB(60, 66, 80));
			FillRect(hDC, &rc, hTrack);
			DeleteObject(hTrack);
		}
		if (fOnNow && pEvent->lnEnd > pEvent->lnStart)
		{
			RECT rcDone = rc;
			HBRUSH hDone = CreateSolidBrush(RGB(255, 210, 90));
			rcDone.right = rc.left + (int)((rc.right - rc.left) * (double)(lnNow - pEvent->lnStart) / (double)(pEvent->lnEnd - pEvent->lnStart));
			FillRect(hDC, &rcDone, hDone);
			DeleteObject(hDone);
		}

		// Line 2: short description
		rc.top = rc.bottom + 3;
		rc.bottom = rc.top + nLine;
		if (fOnNow)
			_snprintf(szLine, sizeof(szLine) - 1, "%d min left.  %s", (int)((pEvent->lnEnd - lnNow) / 600000000) + 1, pEvent->szShort);
		else if (pEvent->lnStart > lnNow)
			_snprintf(szLine, sizeof(szLine) - 1, "Starts in %d min.  %s", (int)((pEvent->lnStart - lnNow) / 600000000) + 1, pEvent->szShort);
		else
			_snprintf(szLine, sizeof(szLine) - 1, "%s", pEvent->szShort);
		szLine[sizeof(szLine) - 1] = '\0';
		DrawTextLine(hDC, hFontNormal, RGB(215, 215, 215), szLine, &rc, DT_LEFT);

		// Line 3: the event after it
		rc.top = rc.bottom;
		rc.bottom = rc.top + nLine;
		if (nIndex + 1 < nGuideEvents)
		{
			const GUIDEEVENT * pAfter = &pGuide[nIndex + 1];
			FormatLocalTime(pAfter->lnStart, szStart, !SameLocalDay(pAfter->lnStart, lnNow));
			_snprintf(szLine, sizeof(szLine) - 1, "%s   %s   %s", EventLabel(nIndex + 1, lnNow), szStart, pAfter->szName);
			szLine[sizeof(szLine) - 1] = '\0';
			DrawTextLine(hDC, hFontNormal, RGB(150, 160, 180), szLine, &rc, DT_LEFT);
		}
	}
	SelectObject(hDC, hOldFont);
}

static LRESULT CALLBACK StripWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_PAINT:
		{
			PAINTSTRUCT ps;
			HDC hDC = BeginPaint(hWnd, &ps);
			RECT rc;
			HDC hMemDC;
			HBITMAP hBitmap, hOldBitmap;

			// Double-buffered: it repaints every second
			GetClientRect(hWnd, &rc);
			hMemDC = CreateCompatibleDC(hDC);
			hBitmap = CreateCompatibleBitmap(hDC, rc.right, rc.bottom);
			hOldBitmap = SelectObject(hMemDC, hBitmap);
			PaintStrip(hWnd, hMemDC);
			BitBlt(hDC, 0, 0, rc.right, rc.bottom, hMemDC, 0, 0, SRCCOPY);
			SelectObject(hMemDC, hOldBitmap);
			DeleteObject(hBitmap);
			DeleteDC(hMemDC);
			EndPaint(hWnd, &ps);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_MOUSEWHEEL:
		StepFocus(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
		return 0;
	case WM_LBUTTONDBLCLK:
		FFplayEPG_ToggleList();
		return 0;
	}
	return DefWindowProc(hWnd, msg, wParam, lParam);
}

// Takes the guide keys while the video window is in front (ffplay would get them otherwise)
static LRESULT CALLBACK GuideKeyHook(int nCode, WPARAM wParam, LPARAM lParam)
{
	if (nCode == HC_ACTION && hWndHost != NULL && GetForegroundWindow() == hWndHost &&
		GetKeyState(VK_CONTROL) >= 0 && GetKeyState(VK_MENU) >= 0)
	{
		KBDLLHOOKSTRUCT * pKey = (KBDLLHOOKSTRUCT *)lParam;

		switch (pKey->vkCode)
		{
		case VK_PRIOR:
		case VK_NEXT:
		case VK_HOME:
		case 'L':
		case 'E':
			if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN)
				PostMessage(hWndHost, WM_FFPLAY_EPG_KEY, pKey->vkCode, 0);
			return 1;
		}
	}
	return CallNextHookEx(hKeyHook, nCode, wParam, lParam);
}

static void OpenEPGGrid()
{
	if (v->hWndEPGGrid == NULL || !IsWindow(v->hWndEPGGrid))
		SendMessage(v->hWndMainWindow, WM_COMMAND, ID_VIEW_SHOWEPGGRID, 0);
	if (v->hWndEPGGrid != NULL && IsWindow(v->hWndEPGGrid))
	{
		SetEPGOffsetNearChannel(nService);
		if (IsIconic(v->hWndEPGGrid))
			ShowWindow(v->hWndEPGGrid, SW_RESTORE);
		SetForegroundWindow(v->hWndEPGGrid);
		InvalidateRect(v->hWndEPGGrid, NULL, TRUE);
	}
}

// Adds the Guide menu and the EPG Grid button to the host window's menu bar
void FFplayEPG_AddMenus(HMENU hMenuBar)
{
	HMENU hMenuGuide = CreatePopupMenu();

	AppendMenu(hMenuGuide, MF_STRING, ID_FFPLAY_EPG_PREVIOUS, "&Previous event\tPgUp");
	AppendMenu(hMenuGuide, MF_STRING, ID_FFPLAY_EPG_NEXT, "&Next event\tPgDn");
	AppendMenu(hMenuGuide, MF_STRING, ID_FFPLAY_EPG_NOW, "Back to &now\tHome");
	AppendMenu(hMenuGuide, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenuGuide, MF_STRING | (fListVisible ? MF_CHECKED : 0), ID_FFPLAY_EPG_LIST, "Schedule &list\tL");
	AppendMenu(hMenuGuide, MF_STRING, ID_FFPLAY_EPG_GRID, "Open EPG &grid\tE");
	hMenuLanguage = CreatePopupMenu();		// filled when opened (FillEITLanguageMenu)
	AppendMenu(hMenuGuide, MF_POPUP, (UINT_PTR)hMenuLanguage, "EIT &language");
	AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hMenuGuide, "&Guide");
	AppendMenu(hMenuBar, MF_STRING, ID_FFPLAY_EPG_GRID, "&EPG Grid");
}

void FFplayEPG_ToggleList()
{
	HMENU hMenu = GetMenu(hWndHost);

	fListVisible = !fListVisible;
	ShowWindow(hWndList, fListVisible ? SW_SHOW : SW_HIDE);
	ShowWindow(hWndDetails, fListVisible ? SW_SHOW : SW_HIDE);
	if (hMenu != NULL)
		CheckMenuItem(hMenu, ID_FFPLAY_EPG_LIST, MF_BYCOMMAND | (fListVisible ? MF_CHECKED : MF_UNCHECKED));
	// The host lays out the video again around the panel
	SendMessage(hWndHost, WM_SIZE, 0, 0);
	if (fListVisible)
		SyncListToFocus();
}

// Video area of the host's client rectangle once the strip and the list are placed
void FFplayEPG_Layout(RECT * prcVideo)
{
	RECT rc;
	int nListWidth = 0;

	GetClientRect(hWndHost, &rc);
	if (fListVisible && rc.right > EPG_LIST_WIDTH + 160)
		nListWidth = EPG_LIST_WIDTH;
	if (hWndStrip != NULL)
		MoveWindow(hWndStrip, 0, rc.bottom - nStripHeight, rc.right - nListWidth, nStripHeight, TRUE);
	if (hWndList != NULL && nListWidth)
	{
		int nListHeight = (rc.bottom * 55) / 100;
		MoveWindow(hWndList, rc.right - nListWidth, 0, nListWidth, nListHeight, TRUE);
		MoveWindow(hWndDetails, rc.right - nListWidth, nListHeight, nListWidth, rc.bottom - nListHeight, TRUE);
	}
	prcVideo->left = 0;
	prcVideo->top = 0;
	prcVideo->right = rc.right - nListWidth;
	prcVideo->bottom = rc.bottom - nStripHeight;
	if (prcVideo->bottom < 0)
		prcVideo->bottom = 0;
}

// Handles the host's guide commands and keys; returns TRUE when it was one of ours
BOOL FFplayEPG_HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam)
{
	int nID;

	if (msg == WM_FFPLAY_EPG_KEY)
	{
		switch (wParam)
		{
		case VK_PRIOR:
			nID = ID_FFPLAY_EPG_PREVIOUS;
			break;
		case VK_NEXT:
			nID = ID_FFPLAY_EPG_NEXT;
			break;
		case VK_HOME:
			nID = ID_FFPLAY_EPG_NOW;
			break;
		case 'L':
			nID = ID_FFPLAY_EPG_LIST;
			break;
		default:
			nID = ID_FFPLAY_EPG_GRID;
			break;
		}
	}
	else if (msg == WM_COMMAND)
		nID = LOWORD(wParam);
	else if (msg == WM_INITMENUPOPUP && (HMENU)wParam == hMenuLanguage)
	{
		FillEITLanguageMenu(hMenuLanguage, ID_FFPLAY_EPG_LANGUAGE);
		return TRUE;
	}
	else if (msg == WM_TIMER && wParam == EPG_TIMER_ID)
	{
		RefreshGuide(FALSE);
		return TRUE;
	}
	else if (msg == WM_NOTIFY && ((NMHDR *)lParam)->idFrom == IDC_EPG_LIST)
	{
		NMLISTVIEW * pnm = (NMLISTVIEW *)lParam;

		// A click in the list moves the strip's focus to that event
		if (pnm->hdr.code == LVN_ITEMCHANGED && (pnm->uNewState & LVIS_SELECTED) && !(pnm->uOldState & LVIS_SELECTED) &&
			GetFocus() == hWndList && pnm->iItem >= 0 && pnm->iItem < nGuideEvents)
		{
			DWORD64 lnNow;

			GetEPGNow(&lnNow);
			nFocus = pnm->iItem;
			fFollowNow = (nFocus == CurrentEventIndex(lnNow));
			UpdateDetails(nFocus);
			InvalidateRect(hWndStrip, NULL, FALSE);
		}
		return TRUE;
	}
	else
		return FALSE;

	switch (nID)
	{
	case ID_FFPLAY_EPG_PREVIOUS:
		StepFocus(-1);
		break;
	case ID_FFPLAY_EPG_NEXT:
		StepFocus(1);
		break;
	case ID_FFPLAY_EPG_NOW:
		FocusNow();
		break;
	case ID_FFPLAY_EPG_LIST:
		FFplayEPG_ToggleList();
		break;
	case ID_FFPLAY_EPG_GRID:
		OpenEPGGrid();
		break;
	default:
		if (msg == WM_COMMAND && SetEITLanguageFromMenu(nID, ID_FFPLAY_EPG_LANGUAGE))
			break;
		return FALSE;
	}
	return TRUE;
}

// Creates the strip, list and details box in the host window for service nServiceID
void FFplayEPG_Create(HWND hWnd, int nServiceID)
{
	static BOOL fClassRegistered = FALSE;
	LVCOLUMN lvc;
	HDC hDC;
	TEXTMETRIC tm;
	NONCLIENTMETRICS ncm;

	hWndHost = hWnd;
	nService = nServiceID & 0xffff;
	nFocus = -1;
	fFollowNow = TRUE;
	dwGuideSignature = 0;
	nGuideEvents = 0;
	if (pGuide == NULL)
		pGuide = LocalAlloc(LPTR, sizeof(GUIDEEVENT) * EPG_MAX_EVENTS);

	if (!fClassRegistered)
	{
		WNDCLASS wc;

		memset(&wc, 0, sizeof(wc));
		wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
		wc.lpfnWndProc = StripWndProc;
		wc.hInstance = v->hInstance;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.lpszClassName = gszStripClass;
		RegisterClass(&wc);
		fClassRegistered = TRUE;
	}

	memset(&ncm, 0, sizeof(ncm));
	ncm.cbSize = sizeof(ncm);
	SystemParametersInfo(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	ncm.lfMessageFont.lfHeight = (ncm.lfMessageFont.lfHeight * 12) / 10;
	hFontNormal = CreateFontIndirect(&ncm.lfMessageFont);
	ncm.lfMessageFont.lfWeight = FW_BOLD;
	hFontBold = CreateFontIndirect(&ncm.lfMessageFont);

	hDC = GetDC(hWnd);
	SelectObject(hDC, hFontBold);
	GetTextMetrics(hDC, &tm);
	ReleaseDC(hWnd, hDC);
	nStripHeight = 4 + (tm.tmHeight + 2) + 8 + 2 * (tm.tmHeight + 2) + 6;

	hWndStrip = CreateWindow(gszStripClass, "", WS_CHILD | WS_VISIBLE, 0, 0, 10, nStripHeight,
							 hWnd, (HMENU)IDC_EPG_STRIP, v->hInstance, NULL);
	hWndList = CreateWindowEx(0, WC_LISTVIEW, "", WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
							  0, 0, 10, 10, hWnd, (HMENU)IDC_EPG_LIST, v->hInstance, NULL);
	ListView_SetExtendedListViewStyle(hWndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
	SendMessage(hWndList, WM_SETFONT, (WPARAM)hFontNormal, FALSE);
	memset(&lvc, 0, sizeof(lvc));
	lvc.mask = LVCF_TEXT | LVCF_WIDTH;
	lvc.pszText = "Start";
	lvc.cx = 110;
	ListView_InsertColumn(hWndList, 0, &lvc);
	lvc.pszText = "Min";
	lvc.cx = 40;
	ListView_InsertColumn(hWndList, 1, &lvc);
	lvc.pszText = "Event";
	lvc.cx = EPG_LIST_WIDTH - 110 - 40 - 24;
	ListView_InsertColumn(hWndList, 2, &lvc);
	hWndDetails = CreateWindowEx(0, "EDIT", "", WS_CHILD | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
								 0, 0, 10, 10, hWnd, (HMENU)IDC_EPG_DETAILS, v->hInstance, NULL);
	SendMessage(hWndDetails, WM_SETFONT, (WPARAM)hFontNormal, FALSE);
	if (fListVisible)
	{
		ShowWindow(hWndList, SW_SHOW);
		ShowWindow(hWndDetails, SW_SHOW);
	}

	RefreshGuide(TRUE);
	SetTimer(hWnd, EPG_TIMER_ID, 1000, NULL);
	if (hKeyHook == NULL)
		hKeyHook = SetWindowsHookEx(WH_KEYBOARD_LL, GuideKeyHook, v->hInstance, 0);
}

// Called from the host's WM_DESTROY (the child windows go with it)
void FFplayEPG_Destroy()
{
	if (hKeyHook != NULL)
	{
		UnhookWindowsHookEx(hKeyHook);
		hKeyHook = NULL;
	}
	if (hWndHost != NULL)
		KillTimer(hWndHost, EPG_TIMER_ID);
	FreeGuide();
	hWndHost = hWndStrip = hWndList = hWndDetails = NULL;
	if (hFontNormal != NULL)
		DeleteObject(hFontNormal);
	if (hFontBold != NULL)
		DeleteObject(hFontBold);
	hFontNormal = hFontBold = NULL;
}

// Height the strip adds below the video, so the host can size its window for it
int FFplayEPG_StripHeight()
{
	return nStripHeight;
}

// Name of the event on now (by the EPG time source) for a service; FALSE when the EIT has
// nothing for it. Also used by the video mosaic captions.
BOOL GetPresentEITEventName(int nServiceID, char * szName, int nSize)
{
	PEITEVENT pEvent;
	DWORD64 lnNow;
	BOOL fFound = FALSE;

	szName[0] = '\0';
	if (nServiceID < 0 || nServiceID >= MAX_EIT_CHANNEL_DATA)
		return FALSE;
	GetEPGNow(&lnNow);
	EnterCriticalSection(&v->csEIT);
	for (pEvent = v->pEvents[nServiceID]; pEvent != NULL; pEvent = (PEITEVENT)pEvent->dwNextEvent)
	{
		DWORD64 lnStart;
		DWORD64 lnDuration = ((DWORD64)pEvent->stRunTime.wHour * 3600 + pEvent->stRunTime.wMinute * 60 + pEvent->stRunTime.wSecond) * 10000000;

		if (!SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&lnStart))
			continue;
		if (lnNow >= lnStart && lnNow < lnStart + lnDuration)
		{
			lstrcpyn(szName, pEvent->szEventName, nSize);
			fFound = TRUE;
			break;
		}
	}
	LeaveCriticalSection(&v->csEIT);
	return fFound;
}

// TRUE when the EIT has any event for the service
BOOL ServiceHasEIT(int nServiceID)
{
	return nServiceID >= 0 && nServiceID < MAX_EIT_CHANNEL_DATA && v->pEvents[nServiceID] != NULL;
}

void LanguageName(const BYTE * pCode, char * szName);

// Fills an "EIT language" popup: stream order, then every language the EIT has carried
void FillEITLanguageMenu(HMENU hPopup, UINT uBase)
{
	int i;

	while (GetMenuItemCount(hPopup) > 0)
		DeleteMenu(hPopup, 0, MF_BYPOSITION);
	AppendMenu(hPopup, MF_STRING | (v->szEITLanguages[0] == '\0' ? MF_CHECKED : 0), uBase, "As the stream lists them");
	AppendMenu(hPopup, MF_SEPARATOR, 0, NULL);
	for (i = 0; i < 16 && v->szEITLanguagesSeen[i][0]; i++)
	{
		char szName[64], szItem[96];

		LanguageName((const BYTE *)v->szEITLanguagesSeen[i], szName);
		wsprintf(szItem, "%s (%s)", szName, v->szEITLanguagesSeen[i]);
		AppendMenu(hPopup, MF_STRING | (strncmp(v->szEITLanguages, v->szEITLanguagesSeen[i], 3) == 0 ? MF_CHECKED : 0),
				   uBase + 1 + i, szItem);
	}
	if (i == 0)
		AppendMenu(hPopup, MF_STRING | MF_GRAYED, 0, "(no EIT text seen yet)");
}

// Applies a choice from FillEITLanguageMenu. Stored events take the new language's text
// the next time the stream repeats them.
BOOL SetEITLanguageFromMenu(UINT nID, UINT uBase)
{
	if (nID < uBase || nID > uBase + 16)
		return FALSE;
	if (nID == uBase)
		v->szEITLanguages[0] = '\0';
	else
	{
		const char * szCode = v->szEITLanguagesSeen[nID - uBase - 1];
		int i;

		if (szCode[0] == '\0')
			return TRUE;
		// The chosen language first, then the others in the order the stream uses
		lstrcpyn(v->szEITLanguages, szCode, 4);
		for (i = 0; i < 16 && v->szEITLanguagesSeen[i][0] && lstrlen(v->szEITLanguages) + 4 < sizeof(v->szEITLanguages); i++)
		{
			if (strncmp(v->szEITLanguagesSeen[i], szCode, 3) == 0)
				continue;
			lstrcat(v->szEITLanguages, " ");
			lstrcat(v->szEITLanguages, v->szEITLanguagesSeen[i]);
		}
	}
	v->nEITLanguageGeneration++;
	return TRUE;
}
