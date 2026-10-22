#include <windows.h>
#include <commctrl.h>
#include "TSReader.h"
#include "resource.h"

#ifndef LITE
#include "TSReader_Scheduler/TSReader_Scheduler.h"
PEPGSCHEDULE pepgschedule;
int nEPGScheduleItems;
int nEPGScheduleMax;
void UpdateSkyEPGMap(int nBATID);
#endif LITE

extern PVARIABLES v;
extern char gszAppName[];
extern char szLiteWarning[];

#define EXTRA_VERTICAL_PIXELS 3
#ifndef WM_MOUSEWHEEL
	#define WM_MOUSEWHEEL                   0x020A 
#endif

int __cdecl SortEITCompare(const void *elem1, const void *elem2);
void CursorNormal();
void CursorWait(HWND hWnd);
void DecodeATSCContentAdvisoryDescriptor(char * szBuffer, BYTE * pDescriptor, BOOL fShortMode);
BOOL QuickFormatNIT(char * szBuffer, int nTransportStreamID, BOOL fLongVersion);
void GetEITSource(char * szSource, PEITEVENT pEvent);
void GetRetuneCommandLineParameters(char * szCommandLine, int nNITIndex);
BOOL CALLBACK EPGGridSettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
BOOL EventInPast(PEITEVENT pEvent, BOOL fAllowPastEITData);
BOOL GetEPGNow(DWORD64 * plnNow);
int GetLogicalChannelNumber(int nProgramNumber);
void SetupHorizontalScrollRange(HWND hWnd);

BOOL fShownScreenEventsWarning;

#define EPG_TITLE_TIMER 2			// timer 1 is used by EPGPaint when there are no events
BOOL fEPGWaitingForStreamTime;		// grid opened before the first TDT/TOT - jump to it when it arrives

// Move the grid so it starts at "now" according to the selected EPG time source
void EPGGotoNow(HWND hWnd)
{
	DWORD64 lnNow;
	SYSTEMTIME st;

	fEPGWaitingForStreamTime = !GetEPGNow(&lnNow);
	FileTimeToSystemTime((FILETIME *)&lnNow, &st);
	st.wSecond = 0;
	st.wMilliseconds = 0;
	if (st.wMinute < 30)
		st.wMinute = 0;
	else
		st.wMinute = 30;
	SystemTimeToFileTime(&st, (FILETIME *)&v->epg.nStartDisplayTime);
	v->epg.nActualStartTime = v->epg.nStartDisplayTime;
	SetupHorizontalScrollRange(hWnd);
	InvalidateRect(hWnd, NULL, FALSE);
}

// Window title shows the EPG time source and its current time (local)
void UpdateEPGTitle(HWND hWnd)
{
	static const char * szSources[] = {"Stream time (TDT/TOT)", "PC clock", "Set time"};
	DWORD64 lnNow;
	SYSTEMTIME st, stLocal;
	BOOL fKnown;
	char szTitle[256];

	fKnown = GetEPGNow(&lnNow);
	FileTimeToSystemTime((FILETIME *)&lnNow, &st);
	SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
	if (fKnown)
		wsprintf(szTitle, "EPG Grid - %s: %04d/%02d/%02d %02d:%02d:%02d",
				 szSources[v->nEPGTimeSource], stLocal.wYear, stLocal.wMonth, stLocal.wDay,
				 stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
	else
		wsprintf(szTitle, "EPG Grid - Stream time (TDT/TOT): waiting for TDT/TOT, showing PC clock %04d/%02d/%02d %02d:%02d:%02d",
				 stLocal.wYear, stLocal.wMonth, stLocal.wDay, stLocal.wHour, stLocal.wMinute, stLocal.wSecond);
	SetWindowText(hWnd, szTitle);
}

void UpdateEPGTimeSourceMenu(HWND hWnd)
{
	HMENU hMenu = GetMenu(hWnd);

	if (hMenu != NULL)
		CheckMenuRadioItem(hMenu, ID_EPG_TIME_STREAM, ID_EPG_TIME_USER,
						   ID_EPG_TIME_STREAM + v->nEPGTimeSource, MF_BYCOMMAND);
}

void CreateEPGTimeSourceMenu(HWND hWnd)
{
	HMENU hMenuBar = CreateMenu();
	HMENU hMenuTime = CreatePopupMenu();

	AppendMenu(hMenuTime, MF_STRING, ID_EPG_TIME_STREAM, "&Stream time (TDT/TOT)");
	AppendMenu(hMenuTime, MF_STRING, ID_EPG_TIME_PC, "&PC clock");
	AppendMenu(hMenuTime, MF_STRING, ID_EPG_TIME_USER, "Set &date and time...\tT");
	AppendMenu(hMenuTime, MF_SEPARATOR, 0, NULL);
	AppendMenu(hMenuTime, MF_STRING, ID_EPG_TIME_GOTO_NOW, "Go to &now\tN");
	AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hMenuTime, "&Time Source");
	SetMenu(hWnd, hMenuBar);
	UpdateEPGTimeSourceMenu(hWnd);
}

BOOL CALLBACK EPGSetTimeDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			DWORD64 lnNow;
			SYSTEMTIME st, stLocal;

			GetEPGNow(&lnNow);
			FileTimeToSystemTime((FILETIME *)&lnNow, &st);
			SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
			DateTime_SetSystemtime(GetDlgItem(hDlg, IDC_EPG_TIME_DATE), GDT_VALID, &stLocal);
			DateTime_SetSystemtime(GetDlgItem(hDlg, IDC_EPG_TIME_TIME), GDT_VALID, &stLocal);
		}
		return TRUE;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			{
				SYSTEMTIME stDate, stTime, stUTC;

				memset(&stDate, 0, sizeof(stDate));
				memset(&stTime, 0, sizeof(stTime));
				DateTime_GetSystemtime(GetDlgItem(hDlg, IDC_EPG_TIME_DATE), &stDate);
				DateTime_GetSystemtime(GetDlgItem(hDlg, IDC_EPG_TIME_TIME), &stTime);
				stDate.wHour = stTime.wHour;
				stDate.wMinute = stTime.wMinute;
				stDate.wSecond = stTime.wSecond;
				stDate.wMilliseconds = 0;
				TzSpecificLocalTimeToSystemTime(NULL, &stDate, &stUTC);
				SystemTimeToFileTime(&stUTC, (FILETIME *)&v->nEPGUserTime);
				EndDialog(hDlg, TRUE);
			}
			break;
		}
		break;
	}

	return FALSE;
}

void SetEPGTimeSource(HWND hWnd, int nSource)
{
	if (nSource == EPG_TIME_SOURCE_USER)
	{
		if (DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_SET_TIME), hWnd, EPGSetTimeDlgProc) != TRUE)
			return;
	}
	v->nEPGTimeSource = nSource;
	UpdateEPGTimeSourceMenu(hWnd);
	UpdateEPGTitle(hWnd);
	EPGGotoNow(hWnd);
}

// Scheduling a recording is about the real world, so it always uses the PC clock
BOOL EventInPastPC(PEITEVENT pEvent)
{
	SYSTEMTIME stSystemTime;
	DWORD64 lnProgramEnd, lnNow;
	DWORD64 lnRunTime = ( (pEvent->stRunTime.wHour * 60 * 60)
					    + (pEvent->stRunTime.wMinute * 60)
						+ (pEvent->stRunTime.wSecond) ) * (DWORD64)10000000;

	GetSystemTime(&stSystemTime);
	SystemTimeToFileTime(&stSystemTime, (FILETIME *)&lnNow);
	SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&lnProgramEnd);
	return lnProgramEnd + lnRunTime <= lnNow;
}

int GetEITOffset(int nProgramNumber)
{
	int nCurrent;

	for (nCurrent = 0; nCurrent < MAX_EIT_CHANNEL_DATA; nCurrent++)
	{
		if (!v->epg.fDisplayEPGByLCN)
		{
			if (v->pChannelData[nCurrent] != NULL || v->pEvents[nCurrent])
			{
				nProgramNumber--;
				if (!nProgramNumber)
					return nCurrent;
			}
		}
		else
		{
			if ((v->channel_maps[nCurrent][0] & 0xffff) != 0)
			{
				nProgramNumber--;
				if (!nProgramNumber)
					return nCurrent;
			}
		}
	}

	return 0;
}

int GetMaxEITChannels()
{
	int nMAXEIT = 0;
	int i;

	for (i = 0; i < MAX_EIT_CHANNEL_DATA; i++)
	{
		if (v->pChannelData[i] != NULL)
		{
			if (v->epg.fDisplayEPGByLCN == TRUE)
			{
				if (v->pChannelData[i]->nLogicalChannelNumber == 0)
					continue;
			}
			nMAXEIT++;
		}
		else if (v->pEvents[i] != NULL)
			nMAXEIT++;
	}
	return nMAXEIT;
}

int SetupVerticalScrollMax(HWND hWnd)
{
	SCROLLINFO si;

	v->epg.nMaxEITChannels = GetMaxEITChannels();
	si.cbSize = sizeof(SCROLLINFO);
	si.fMask = SIF_RANGE;
	si.nMax = v->epg.nMaxEITChannels;
	si.nMin = 0;
	SetScrollInfo(hWnd, SB_VERT, &si, TRUE);

	return v->epg.nMaxEITChannels;
}

#define MAX_SCREEN_EVENTS 3000
#define GRID_START_Y 70

int IsRecordingScheduled(int nProgramNumber, PEITEVENT pEvent)
{
#ifndef LITE
	int nIndex;
	int nDuration;
	int nCompareProgramNumber = nProgramNumber;
	__int64 nEventEnd;
	__int64 nEventStart;

	if (pEvent == NULL)
		return FALSE;

	if (v->pChannelData[nProgramNumber] != NULL)
	{
		if (v->pChannelData[nProgramNumber]->fATSC)
		{
			nCompareProgramNumber |= v->pChannelData[nProgramNumber]->nMajorChannelNumber << 24
				                  |  v->pChannelData[nProgramNumber]->nMinorChannelNumber << 16;
		}
	}

	SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&nEventStart);
	nDuration =  pEvent->stRunTime.wHour * 60 * 60
			   + pEvent->stRunTime.wMinute * 60
			   + pEvent->stRunTime.wSecond;
	nEventEnd = nEventStart + ((__int64)nDuration * (__int64)10000000);

	for (nIndex = 0; nIndex < nEPGScheduleItems; nIndex++)
	{
		if (pepgschedule[nIndex].nChannel == nCompareProgramNumber)
		{
			__int64 nScheduledEnd;

			nScheduledEnd = pepgschedule[nIndex].nStartTime + ((__int64)pepgschedule[nIndex].nDuration * (__int64)10000000);
			/*if (pepgschedule[nIndex].nStartTime <= nEventEnd)
			{
				if (nEventStart <= nScheduledEnd)
					return TRUE;
			}*/
			if (nEventStart == pepgschedule[nIndex].nStartTime)// && nEventEnd <= nScheduledEnd)
				return nIndex;		
		}
	}
#endif LITE	
	return -1;
}

#ifndef LITE
/*void FormatFILETIME(char * szReturn, __int64 nValue)
{
	SYSTEMTIME st;

	FileTimeToSystemTime((FILETIME *)&nValue, &st);
	wsprintf(szReturn, "%04d/%02d/%02d %02d:%02d:%02d",
		     st.wYear, st.wMonth, st.wDay,
			 st.wHour, st.wMinute, st.wSecond);
}*/

int CheckForDuplicateRecording(PEITEVENT pEvent)
{
	int nIndex;
	int nDuration;
	__int64 nEventEnd;
	__int64 nEventStart;

	if (pEvent == NULL)
		return FALSE;

	SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&nEventStart);
	nDuration =  pEvent->stRunTime.wHour * 60 * 60
			   + pEvent->stRunTime.wMinute * 60
			   + pEvent->stRunTime.wSecond;
	nDuration--;
	nEventEnd = nEventStart + ((__int64)nDuration * (__int64)10000000);

	for (nIndex = 0; nIndex < nEPGScheduleItems; nIndex++)
	{
		__int64 nScheduledEnd;

		if (pepgschedule[nIndex].nChannel == 0)
			continue;

		nScheduledEnd = pepgschedule[nIndex].nStartTime + ((__int64)pepgschedule[nIndex].nDuration * (__int64)10000000);
		/*{
			char szTemp[256];
			char szEventStart[64], szEventEnd[64];
			char szScheduleStart[64], szScheduleEnd[64];

			FormatFILETIME(szEventStart, nEventStart);
			FormatFILETIME(szEventEnd, nEventEnd);
			FormatFILETIME(szScheduleStart, pepgschedule[nIndex].nStartTime);
			FormatFILETIME(szScheduleEnd, nScheduledEnd);
			wsprintf(szTemp, "event: %s->%s schedule: %s->%s\n", 
				     szEventStart, szEventEnd,
					 szScheduleStart, szScheduleEnd);

			OutputDebugString(szTemp);
		}*/
		if (pepgschedule[nIndex].nStartTime <= nEventEnd)
		{
			if (nEventStart <= nScheduledEnd)
				return nIndex;
		}
	}

	return -1;
}
#endif LITE

void DrawEPGScheduledIcon(HDC hDC, int nCircleXRight, SIZE sizeChannel, int nCurrentY, RECT * rc, int nScheduleIndex, RECT * rcEventRectange)
{
	if (nCircleXRight > rc->right)
		nCircleXRight = rc->right - 2;

	SelectObject(hDC, v->epg.hCellEntryRecordBackground);
	Ellipse(hDC, 
			nCircleXRight - sizeChannel.cy, nCurrentY + 1,
			nCircleXRight, nCurrentY + 1 + sizeChannel.cy);
	/*Ellipse(hDC, 
			nCircleXRight - sizeChannel.cy - (sizeChannel.cy / 4), nCurrentY + 1,
			nCircleXRight - (sizeChannel.cy / 4), nCurrentY + 1 + sizeChannel.cy);*/

#ifndef LITE
	if (pepgschedule[nScheduleIndex].wPreRoll)
	{
		SelectObject(hDC, v->epg.hCellEntryPrePostRollPen);
		MoveToEx(hDC, rcEventRectange->left + 1, rcEventRectange->top + 3, NULL);
		LineTo(hDC, rcEventRectange->left + 1, rcEventRectange->bottom - 3);
	}
	if (pepgschedule[nScheduleIndex].wPostRoll)
	{
		SelectObject(hDC, v->epg.hCellEntryPrePostRollPen);
		MoveToEx(hDC, rcEventRectange->right - 2, rcEventRectange->top + 3, NULL);
		LineTo(hDC, rcEventRectange->right - 2, rcEventRectange->bottom - 3);
	}
#endif LITE
}

void EPGPaint(HWND hWnd, WPARAM wParam, LPARAM lParam)
{
	int nCurrentX;
	int nCurrentY = GRID_START_Y;
	int nProgramNumber;
	int nStartProgramOffset = GetEITOffset(v->epg.nVerticalScrollPos);
	int nEvents = 0;
	int nCountProgramNumber;
	__int64 nCurrentDisplayTime;
	HDC hDC, hRealDC;
	HBITMAP memBM;
	PAINTSTRUCT ps;
	RECT rc;
	SIZE sizeChannel;
	SYSTEMTIME st, stLocal;
	char szTemp[128];

	if (v->epg.screenevents != NULL)
		LocalFree(v->epg.screenevents);
	v->epg.screenevents = LocalAlloc(LPTR, sizeof(SCREENEVENTS) * MAX_SCREEN_EVENTS);
	if (v->epg.screenevents == NULL)
	{
		OutputDebugString("EPGGrid.c: LocalAlloc(LPTR, sizeof(SCREENEVENTS) * MAX_SCREEN_EVENTS) failed\n");
		return;
	}

	if (v->fSkyEPG)
		v->epg.fDisplayEPGByLCN = TRUE;
	else
		v->epg.fDisplayEPGByLCN = FALSE;

	GetClientRect(hWnd, &rc);
	hRealDC = BeginPaint(hWnd, &ps);
	hDC = CreateCompatibleDC(hRealDC);
    memBM = CreateCompatibleBitmap (hRealDC, rc.right, rc.bottom);
    SelectObject(hDC, memBM);
	SelectObject(hDC, v->epg.hGridTextFont);
	SetBkMode(hDC, TRANSPARENT);

	// Reset scroll range and check for no entries
	if (SetupVerticalScrollMax(hWnd) == 0)
	{
		SIZE sizeNoEvents;
		static char szNoEvents[] = {"No EPG data available at this time"};
#ifndef LITE
		SIZE sizeManualRecord;
		static char szManualRecord[] = {"Press M to schedule a manual recording"};
#endif LITE

		SetTextColor(hDC, v->dwEPGMainTextColor);
		GetTextExtentPoint(hDC, szNoEvents, lstrlen(szNoEvents), &sizeNoEvents);
		TextOut(hDC, rc.right / 2 - sizeNoEvents.cx / 2, rc.bottom / 2 - sizeNoEvents.cy / 2, szNoEvents, lstrlen(szNoEvents));
#ifndef LITE
		GetTextExtentPoint(hDC, szManualRecord, lstrlen(szManualRecord), &sizeManualRecord);
		TextOut(hDC, rc.right / 2 - sizeManualRecord.cx / 2, rc.bottom / 2 - sizeNoEvents.cy / 2 + sizeNoEvents.cy, szManualRecord, lstrlen(szManualRecord));
#endif LITE

		if (!v->epg.fTimerRunning)
		{
			SetTimer(hWnd, 1, 100, NULL);
			v->epg.fTimerRunning = TRUE;
		}
		goto WindupEPGGridPaint;
	}
	if (v->epg.fTimerRunning)
	{
		KillTimer(hWnd, 1);
		v->epg.fTimerRunning = FALSE;
	}

	// Seperate the grid from the program details
	SelectObject(hDC, GetStockObject(WHITE_PEN));
	SelectObject(hDC, GetStockObject(BLACK_BRUSH));
	RoundRect(hDC, 
			  0, 3, 
			  rc.right - 2, nCurrentY - 5, 
			  10, 10);		

	// Start date in top left
	SetTextColor(hDC, v->dwEPGMainTextColor);
	GetTextExtentPoint(hDC, "X", 1, &sizeChannel);
	nCurrentDisplayTime = v->epg.nStartDisplayTime;
	FileTimeToSystemTime((FILETIME *)&nCurrentDisplayTime, &st);
	SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
	wsprintf(szTemp, "%04d/%02d/%02d", stLocal.wYear, stLocal.wMonth, stLocal.wDay);
	{
		SIZE sizeText;
		GetTextExtentPoint(hDC, szTemp, lstrlen(szTemp), &sizeText);
		TextOut(hDC, 96 / 2 - sizeText.cx / 2, nCurrentY, szTemp, lstrlen(szTemp));
	}

	// Now time for each event in 1/2 hour steps
	v->epg.nTimeRangeShown = 0;
	nCurrentX = 100;
	if (v->nEPGHalfHourWidth == 0)
		v->nEPGHalfHourWidth = 2;
	do
	{
		FileTimeToSystemTime((FILETIME *)&nCurrentDisplayTime, &st);
		SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
		wsprintf(szTemp, "%02d:%02d", stLocal.wHour, stLocal.wMinute);
		if (!(v->nEPGHalfHourWidth == 1 && stLocal.wMinute != 00))
		{
			TextOut(hDC, nCurrentX, nCurrentY, szTemp, lstrlen(szTemp));
			if (v->fEPGTimeGrid == TRUE)
			{
				SIZE sizeText;
				GetTextExtentPoint(hDC, szTemp, lstrlen(szTemp), &sizeText);

				if (!v->fEPGTimeGridOnTop)
				{
					SelectObject(hDC, v->epg.hTimeGridPen);
					MoveToEx(hDC, nCurrentX, nCurrentY + sizeText.cy, NULL);
					LineTo(hDC, nCurrentX, rc.bottom);
				}
			}
		}
		nCurrentDisplayTime += (__int64)10000000 * (__int64)30 * (__int64)60;
		nCurrentX += 30 * v->nEPGHalfHourWidth;
		v->epg.nTimeRangeShown++;
	} while (nCurrentX < rc.right);
	nCurrentY += sizeChannel.cy + EXTRA_VERTICAL_PIXELS;

	// Now the channels and events
	if (nStartProgramOffset == 0 && v->epg.nVerticalScrollPos)
		goto EPGPaint_SelectedEvents;
	v->nMaxEPGDisplayChannel = 0;
	v->epg.nEITChannelsDisplayed = 0;
	for (nCountProgramNumber = nStartProgramOffset; nCountProgramNumber < MAX_EIT_CHANNEL_DATA; nCountProgramNumber++)		
	{
		int nPMTIndex;
		SIZE sizeText;
		char szLogicalChannelNumber[64] = {0};

		if (v->epg.fDisplayEPGByLCN == TRUE)
			nProgramNumber = v->channel_maps[nCountProgramNumber][0] & 0xffff;
		else
			nProgramNumber = nCountProgramNumber;

		if (v->pChannelData[nProgramNumber] == NULL && v->pEvents[nProgramNumber] == NULL)
			continue;
		if (v->fShowEPGChannelsOnly && v->pEvents[nProgramNumber] == NULL)
			continue;
		if (v->fShowEPGThisMuxOnly)
		{
			for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
			{
				if (v->pat.pmt[nPMTIndex].nProgramNumber == nProgramNumber)
					break;
			}
			if (nPMTIndex == MAX_PAT_ENTRIES)
				continue;
		}
		if (!v->epg.fHideChannelSelectMode)
		{
			int nByteOffset;
			int nBit;

			nByteOffset = nProgramNumber / 8;
			nBit = nProgramNumber % 8;
			if (v->epg.bHiddenChannels[nByteOffset] & (1 << nBit))
				continue;
		}

		v->epg.nEITChannelsDisplayed++;
		nCurrentX = 100;

		if (v->fEPGTimeGrid == TRUE && !v->fEPGTimeGridOnTop)
		{
			SelectObject(hDC, v->epg.hTimeGridPen);
			MoveToEx(hDC, nCurrentX, nCurrentY - 1, NULL);
			LineTo(hDC, rc.right, nCurrentY - 1);
		}
		
		if (!v->epg.fHideChannelSelectMode)
		{
			if (nProgramNumber == v->epg.nSelectedChannel)
				SelectObject(hDC, v->epg.hCellEntrySelectedBackground);
			else
				SelectObject(hDC, v->epg.hChannelEntryBackground);
		}
#ifndef LITE
		else
		{
			SelectObject(hDC, v->epg.hChannelEntryBackground);
		}
#endif LITE
		SelectObject(hDC, GetStockObject(BLACK_PEN));
		RoundRect(hDC, 
				  1, nCurrentY - 1, 
				  99, nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1, 
				  10, 10);
#ifndef LITE
		if (v->epg.fHideChannelSelectMode)
		{
			int nByteOffset;
			int nBit;

			nByteOffset = nProgramNumber / 8;
			nBit = nProgramNumber % 8;
			if (v->epg.bHiddenChannels[nByteOffset] & (1 << nBit))
			{
				DrawIcon(hDC, 3, nCurrentY, v->epg.hEPGChannelHidden);
			}
		}
#endif LITE
		v->nMaxEPGDisplayChannel = nProgramNumber;
		v->epg.screenevents[nEvents].rc.left = 1;
		v->epg.screenevents[nEvents].rc.right = 99;
		v->epg.screenevents[nEvents].rc.top = nCurrentY - 1;
		v->epg.screenevents[nEvents].rc.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1;
		v->epg.screenevents[nEvents].nChannel = nProgramNumber;
		v->epg.screenevents[nEvents].pEITEvent = NULL;
		nEvents++;
		if (nEvents >= MAX_SCREEN_EVENTS)
		{
			MessageBox(hWnd, "Not enough room in screenevents", gszAppName, MB_ICONSTOP);
			goto WindupEPGGridPaint;
		}
	
		if (v->pChannelData[nProgramNumber] != NULL)
		{
			if (v->pChannelData[nProgramNumber]->fATSC)
				wsprintf(szLogicalChannelNumber, "%d.%d - ", v->pChannelData[nProgramNumber]->nMajorChannelNumber, v->pChannelData[nProgramNumber]->nMinorChannelNumber);
			else if (v->pChannelData[nProgramNumber]->nLogicalChannelNumber)
			{
				wsprintf(szLogicalChannelNumber, "%d/", v->pChannelData[nProgramNumber]->nLogicalChannelNumber);
				if (v->pChannelData[nProgramNumber]->nLogicalChannelNumber > v->nMaxEPGDisplayChannel)
					v->nMaxEPGDisplayChannel = v->pChannelData[nProgramNumber]->nLogicalChannelNumber;
			}
		}
		wsprintf(szTemp, "%s%d", szLogicalChannelNumber, nProgramNumber);
		SetTextColor(hDC, v->dwEPGMainTextColor);
		SelectObject(hDC, v->epg.hGridTextFontBold);
		GetTextExtentPoint(hDC, szTemp, lstrlen(szTemp), &sizeText);
		TextOut(hDC, 96 / 2 - sizeText.cx / 2, nCurrentY, szTemp, lstrlen(szTemp));

		SetTextColor(hDC, v->dwEPGSubTextColor);
		SelectObject(hDC, v->epg.hGridSmallTextFont);
		if (v->pChannelData[nProgramNumber] != NULL)
		{
			GetTextExtentPoint(hDC, v->pChannelData[nProgramNumber]->szShortName, lstrlen(v->pChannelData[nProgramNumber]->szShortName), &sizeText);
			TextOut(hDC, 96 / 2 - sizeText.cx / 2, nCurrentY + sizeChannel.cy, v->pChannelData[nProgramNumber]->szShortName, lstrlen(v->pChannelData[nProgramNumber]->szShortName));
			if (v->nEPGChannelHeight > 2)
			{
				GetTextExtentPoint(hDC, v->pChannelData[nProgramNumber]->szLongName, lstrlen(v->pChannelData[nProgramNumber]->szLongName), &sizeText);
				TextOut(hDC, 96 / 2 - sizeText.cx / 2, nCurrentY + sizeChannel.cy + sizeText.cy, v->pChannelData[nProgramNumber]->szLongName, lstrlen(v->pChannelData[nProgramNumber]->szLongName));
			}
		}
	
		if (v->pEvents[nProgramNumber] != NULL)
		{
			int nFirstEPGX;
			__int64 nFirstEventTime;

			nCurrentDisplayTime = v->epg.nStartDisplayTime;
			nFirstEPGX = -1;
			nFirstEventTime = -1;
			do
			{
				PEITEVENT pCurrent = v->pEvents[nProgramNumber];
				__int64 nCurrentDisplayEndTime = nCurrentDisplayTime + ((__int64)10000000 * (__int64)((29 * 60) + 59));

				do
				{
					int nDuration;
					__int64 nEndTime;
					__int64 nEventTime;

					SystemTimeToFileTime(&pCurrent->stStartTime, (FILETIME *)&nEventTime);
					nDuration =  pCurrent->stRunTime.wHour * 60 * 60
							   + pCurrent->stRunTime.wMinute * 60
							   + pCurrent->stRunTime.wSecond;
					nEndTime = nEventTime + ((__int64)nDuration * (__int64)10000000);
					
					if (nEventTime >= nCurrentDisplayTime && nEventTime <= nCurrentDisplayEndTime)
					{
						int nPixelWidth;
						int nScheduleIndex;
						__int64 nSecondOffset = (nEventTime - nCurrentDisplayTime) / (__int64)10000000;
						int nMinuteDisplayOffset = (int)nSecondOffset / 60;	// get minutes
						char szEventName[1024];
						char szEventDescription[10 * 1024] = {0};
						
						nMinuteDisplayOffset *= v->nEPGHalfHourWidth; // get pixels							

						lstrcpy(szEventName, pCurrent->szEventName);
						nPixelWidth = ((nDuration / 60) * v->nEPGHalfHourWidth);
						SelectObject(hDC, v->epg.hGridTextFontBold);
						do
						{
							SIZE size;
							GetTextExtentPoint(hDC, szEventName, lstrlen(szEventName), &size);
							if (size.cx < nPixelWidth - 4)
								break;
							szEventName[lstrlen(szEventName) - 1] = '\0';
						} while (lstrlen(szEventName));

						if (!v->epg.fHideChannelSelectMode)
						{
							if (pCurrent == v->epg.pSelectedEvent)
								SelectObject(hDC, v->epg.hCellEntrySelectedBackground);
							else
								SelectObject(hDC, v->epg.hCellEntryBackground);
						}
#ifndef LITE
						else
							SelectObject(hDC, v->epg.hCellEntryHideModeBackground);
#endif LITE
						SelectObject(hDC, GetStockObject(BLACK_PEN));
						if (nFirstEPGX == -1)
						{
							nFirstEPGX = nCurrentX + nMinuteDisplayOffset;
							nFirstEventTime = nEventTime;
						}
						RoundRect(hDC, 
								  nCurrentX + nMinuteDisplayOffset, nCurrentY - 1, 
								  nCurrentX + nMinuteDisplayOffset + nPixelWidth, nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1, 
								  10, 10);

						// Make a note for the mouse stuff
						v->epg.screenevents[nEvents].rc.left = nCurrentX + nMinuteDisplayOffset;
						v->epg.screenevents[nEvents].rc.right = nCurrentX + nMinuteDisplayOffset + nPixelWidth;
						v->epg.screenevents[nEvents].rc.top = nCurrentY - 1;
						v->epg.screenevents[nEvents].rc.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1;
						v->epg.screenevents[nEvents].pEITEvent = pCurrent;
						v->epg.screenevents[nEvents].nChannel = nProgramNumber;
						nEvents++;
						if (nEvents >= MAX_SCREEN_EVENTS && !fShownScreenEventsWarning)
						{
							MessageBox(hWnd, "Not enough room in screenevents - tell support@tsreader.co.uk you saw this", gszAppName, MB_ICONSTOP);
							fShownScreenEventsWarning = TRUE;
							goto WindupEPGGridPaint;
						}
						nScheduleIndex = IsRecordingScheduled(nProgramNumber, pCurrent);
						if (nScheduleIndex != -1)
						{
							int nCircleXRight = nCurrentX + nMinuteDisplayOffset + nPixelWidth - 2;
							RECT rcEventRectangle;

							rcEventRectangle.left = nCurrentX + nMinuteDisplayOffset;
							rcEventRectangle.top = nCurrentY - 1;
							rcEventRectangle.right = nCurrentX + nMinuteDisplayOffset + nPixelWidth;
							rcEventRectangle.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1;
							DrawEPGScheduledIcon(hDC, nCircleXRight, sizeChannel, nCurrentY, &rc, nScheduleIndex, &rcEventRectangle);
						}

						// Output event name
						SetTextColor(hDC, v->dwEPGMainTextColor);	
						TextOut(hDC, nCurrentX + nMinuteDisplayOffset + 2, nCurrentY, szEventName, lstrlen(szEventName));
						if (pCurrent->szShortEventDescription)
							lstrcat(szEventDescription, pCurrent->szShortEventDescription);
						if (pCurrent->szLongEventDescription)
							lstrcat(szEventDescription, pCurrent->szLongEventDescription);
						if (lstrlen(szEventDescription))
						{
							RECT rcText;

							SelectObject(hDC, v->epg.hGridSmallTextFont);
							SetTextColor(hDC, v->dwEPGSubTextColor);	
							rcText.left = nCurrentX + nMinuteDisplayOffset + 2;
							rcText.right = rcText.left + nPixelWidth;
							rcText.top = nCurrentY + sizeChannel.cy;
							rcText.bottom = rcText.top + (sizeChannel.cy * (v->nEPGChannelHeight - 1) - 3) ;
							DrawText(hDC, szEventDescription, -1, &rcText, DT_TOP | DT_LEFT | DT_WORDBREAK);
						}
					}
					pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
				} while (pCurrent != NULL);
				nCurrentDisplayTime += (__int64)10000000 * (__int64)30 * (__int64)60;
				nCurrentX += 30 * v->nEPGHalfHourWidth;
			} while (nCurrentX < rc.right);
			v->nLastEPGDisplayTime = nCurrentDisplayTime;

			// See if we need to fill in an event that already started
			if (nFirstEPGX > 100 || nFirstEPGX == -1)
			{
				int nEITItems;
				PEITEVENT pCurrent;
				PEITEVENTWITHPTR pSortList = NULL;

				if (nFirstEventTime == -1)
				{
					// no items displayed at all - find the one before the start time
					nFirstEventTime = v->epg.nStartDisplayTime;
				}

				nEITItems = 0;
				EnterCriticalSection(&v->csEIT);
				pCurrent = v->pEvents[nProgramNumber];
				if (pCurrent != NULL)
				{
					do
					{
						nEITItems++;
						pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
					} while (pCurrent != NULL);
				}
				if (nEITItems)
				{
					// Got some items, let's copy them into our buffer so we can sort
					int nOutputIndex = 0;

					pSortList = LocalAlloc(LPTR, nEITItems * sizeof(EITEVENTWITHPTR));
					if (pSortList == NULL)
					{
						OutputDebugString("EPGGrid.c: LocalAlloc(LPTR, nEITItems * sizeof(EITEVENTWITHPTR)) failed\n");
						return;
					}
					pCurrent = v->pEvents[nProgramNumber];
					do
					{
						memcpy(&pSortList[nOutputIndex].eitevent, pCurrent, sizeof(EITEVENT));
						pSortList[nOutputIndex++].original = pCurrent;
						pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
					} while (pCurrent != NULL);
				}
				LeaveCriticalSection(&v->csEIT);
				if (nEITItems)
				{
					int nEITIndex;
					int nFillInTarget = -1;

					qsort(pSortList, nEITItems, sizeof(EITEVENTWITHPTR), SortEITCompare);			
					for (nEITIndex = 0; nEITIndex < nEITItems; nEITIndex++)
					{
						__int64 nThisEventStart, nThisEventEnd;
						__int64 nMultiplier = 10000000;
						__int64 nRunTime = ( (pSortList[nEITIndex].eitevent.stRunTime.wHour * 60 * 60)
											   + (pSortList[nEITIndex].eitevent.stRunTime.wMinute * 60)
											   + (pSortList[nEITIndex].eitevent.stRunTime.wSecond) ) * nMultiplier;

						SystemTimeToFileTime(&pSortList[nEITIndex].eitevent.stStartTime, (FILETIME *)&nThisEventStart);
						nThisEventEnd = nThisEventStart + nRunTime;						
						if (v->epg.nStartDisplayTime <= nThisEventEnd)
						{
							if (v->epg.nStartDisplayTime >= nThisEventStart)
								nFillInTarget = nEITIndex;
						}
					}
					if (nFillInTarget != -1)
					{
						int nScheduleIndex;
						RECT rcFill;
						char szEventName[1024];
						char szEventDescription[10 * 1024] = {0};

						if (nFirstEPGX == -1)
						{
							int nPixelWidth;
							__int64 nEventEndTime;
							__int64 nEventRemaining;
							__int64 nMultiplier = 10000000;
							__int64 nRunTime = ( (pSortList[nFillInTarget].eitevent.stRunTime.wHour * 60 * 60)
												   + (pSortList[nFillInTarget].eitevent.stRunTime.wMinute * 60)
												   + (pSortList[nFillInTarget].eitevent.stRunTime.wSecond) ) * nMultiplier;

							SystemTimeToFileTime(&pSortList[nFillInTarget].eitevent.stStartTime, (FILETIME *)&nEventEndTime);
							nEventEndTime += nRunTime;
							nEventRemaining = (nEventEndTime - v->epg.nStartDisplayTime) / nMultiplier;
							nPixelWidth = (int)((nEventRemaining / 60) * v->nEPGHalfHourWidth);
							nFirstEPGX = 100 + nPixelWidth;
							if (nFirstEPGX <= 100)
								goto xxx_here;
							//nFirstEPGX = rc.right + 10;
						}

						SelectObject(hDC, GetStockObject(BLACK_PEN));
						if (!v->epg.fHideChannelSelectMode)
						{
							if (pSortList[nFillInTarget].original == v->epg.pSelectedEvent)
								SelectObject(hDC, v->epg.hCellEntrySelectedBackground);
							else
								SelectObject(hDC, v->epg.hCellEntryBackground);
						}
#ifndef LITE
						else
							SelectObject(hDC, v->epg.hCellEntryHideModeBackground);
#endif LITE
						RoundRect(hDC, 
								  100, nCurrentY - 1, 
								  nFirstEPGX - 1, nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1, 
								  10, 10);

						nScheduleIndex = IsRecordingScheduled(nProgramNumber, pSortList[nFillInTarget].original);
						if (nScheduleIndex != -1)
						{
							int nCircleXRight = nFirstEPGX - 1 - 2;
							RECT rcEventRectangle;

							rcEventRectangle.left = 100;
							rcEventRectangle.top = nCurrentY - 1;
							rcEventRectangle.right = nFirstEPGX - 1;
							rcEventRectangle.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1;
							DrawEPGScheduledIcon(hDC, nCircleXRight, sizeChannel, nCurrentY, &rc, nScheduleIndex, &rcEventRectangle);
						}

						v->epg.screenevents[nEvents].rc.left = 100;
						v->epg.screenevents[nEvents].rc.right = nFirstEPGX - 1;
						v->epg.screenevents[nEvents].rc.top = nCurrentY - 1;
						v->epg.screenevents[nEvents].rc.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight) + 1;
						v->epg.screenevents[nEvents].pEITEvent = pSortList[nFillInTarget].original;
						v->epg.screenevents[nEvents].nChannel = nProgramNumber;
						nEvents++;
						
						// Blow away the rounded edges of the box since this is a prior event 
						rcFill.top = nCurrentY;
						rcFill.bottom = nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight);
						rcFill.right = 100;
						rcFill.left = 105;

						if (!v->epg.fHideChannelSelectMode)
						{
							if (pSortList[nFillInTarget].original == v->epg.pSelectedEvent)
								FillRect(hDC, &rcFill, v->epg.hCellEntrySelectedBackground);
							else
								FillRect(hDC, &rcFill, v->epg.hCellEntryBackground);
						}
#ifndef LITE
						else
							FillRect(hDC, &rcFill, v->epg.hCellEntryHideModeBackground);
#endif LITE

						SelectObject(hDC, GetStockObject(BLACK_PEN));
						MoveToEx(hDC, 100, nCurrentY - 1, NULL);
						LineTo(hDC, 105, nCurrentY - 1);
						MoveToEx(hDC, 100, nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight), NULL);
						LineTo(hDC, 105, nCurrentY + (sizeChannel.cy * v->nEPGChannelHeight));
						
						SetTextColor(hDC, v->dwEPGMainTextColor);	
						SelectObject(hDC, v->epg.hGridTextFontBold);
						lstrcpy(szEventName, pSortList[nFillInTarget].eitevent.szEventName);
						do
						{
							SIZE size;
							GetTextExtentPoint(hDC, szEventName, lstrlen(szEventName), &size);
							if (size.cx < (nFirstEPGX - 100) - 4)
								break;
							szEventName[lstrlen(szEventName) - 1] = '\0';
						} while (lstrlen(szEventName));
						TextOut(hDC, 100 + 2, nCurrentY, szEventName, lstrlen(szEventName));

						if (pSortList[nFillInTarget].eitevent.szShortEventDescription)
							lstrcat(szEventDescription, pSortList[nFillInTarget].eitevent.szShortEventDescription);
						if (pSortList[nFillInTarget].eitevent.szLongEventDescription)
							lstrcat(szEventDescription, pSortList[nFillInTarget].eitevent.szLongEventDescription);
						if (lstrlen(szEventDescription))
						{
							RECT rcText;

							SelectObject(hDC, v->epg.hGridSmallTextFont);
							SetTextColor(hDC, v->dwEPGSubTextColor);	
							rcText.left = 100 + 2;
							rcText.right = nFirstEPGX - 1;
							rcText.top = nCurrentY + sizeChannel.cy;
							rcText.bottom = rcText.top + (sizeChannel.cy * (v->nEPGChannelHeight - 1) - 3) ;
							DrawText(hDC, szEventDescription, -1, &rcText, DT_TOP | DT_LEFT | DT_WORDBREAK);
						}
					}
xxx_here:
					LocalFree(pSortList);
				}
			}
		}
		if (v->fEPGTimeGrid == TRUE && v->fEPGTimeGridOnTop)
		{
			SelectObject(hDC, v->epg.hTimeGridPen);
			MoveToEx(hDC, 100, nCurrentY - 1, NULL);
			LineTo(hDC, rc.right, nCurrentY - 1);
		}
		
		nCurrentY += (sizeChannel.cy * v->nEPGChannelHeight) + EXTRA_VERTICAL_PIXELS;
		if (nCurrentY > rc.bottom)
			break;
	}

	// Details for any selected event
EPGPaint_SelectedEvents:
	if (v->epg.pSelectedEvent != NULL)
	{
		int nRightLimit = 0;
		SIZE sizeEventName, sizeEventDate, sizeEventDuration;
		SYSTEMTIME stLocal;
		char szStartDate[MAX_PATH];
		char szStartTime[128];
		char szEventSource[128] = {0};
		char szEventDescription[10 * 1024] = {0};

		if (v->epg.pSelectedEvent->szShortEventDescription)
			lstrcat(szEventDescription, v->epg.pSelectedEvent->szShortEventDescription);
		if (v->epg.pSelectedEvent->szLongEventDescription)
			lstrcat(szEventDescription, v->epg.pSelectedEvent->szLongEventDescription);

		SetTextColor(hDC, v->dwEPGMainTextColor);	
		SelectObject(hDC, v->epg.hTitleEventFontBold);
		TextOut(hDC, 5, 5, v->epg.pSelectedEvent->szEventName, lstrlen(v->epg.pSelectedEvent->szEventName));
		GetTextExtentPoint(hDC, v->epg.pSelectedEvent->szEventName, lstrlen(v->epg.pSelectedEvent->szEventName), &sizeEventName);

		SystemTimeToTzSpecificLocalTime(NULL, &v->epg.pSelectedEvent->stStartTime, &stLocal);
		GetDateFormat(LOCALE_USER_DEFAULT, DATE_LONGDATE, &stLocal, NULL, szStartDate, sizeof(szStartDate));
		GetTimeFormat(LOCALE_USER_DEFAULT, 0, &stLocal, NULL, szStartTime, sizeof(szStartTime));		
		lstrcat(szStartDate, " ");
		lstrcat(szStartDate, szStartTime);
		SelectObject(hDC, v->epg.hGridSmallTextFont);
		TextOut(hDC, 5, 5 + sizeEventName.cy + 2, szStartDate, lstrlen(szStartDate));
		GetTextExtentPoint(hDC, szStartDate, lstrlen(szStartDate), &sizeEventDate);
		
		wsprintf(szTemp, "Duration %02d:%02d:%02d", v->epg.pSelectedEvent->stRunTime.wHour, v->epg.pSelectedEvent->stRunTime.wMinute, v->epg.pSelectedEvent->stRunTime.wSecond);
		TextOut(hDC, 5, 5 + sizeEventName.cy + 2 + sizeEventDate.cy, szTemp, lstrlen(szTemp));
		GetTextExtentPoint(hDC, szTemp, lstrlen(szTemp), &sizeEventDuration);

#ifndef LITE
		GetEITSource(szEventSource, v->epg.pSelectedEvent);
		wsprintf(szTemp, "EIT: %s", szEventSource);
		TextOut(hDC, 5, 5 + sizeEventName.cy + 2 + sizeEventDate.cy + sizeEventDuration.cy, szTemp, lstrlen(szTemp));
#endif LITE

		if (v->nNetworkPID == 0x1ffb)
		{
			int nDescriptor;
			int nRatingYOffset = 0;
			BOOL fCaptions = FALSE;
			HICON hRatingIcon = NULL;

			SelectObject(hDC, GetStockObject(WHITE_PEN));
			MoveToEx(hDC, rc.right - 50, 4, NULL);
			LineTo(hDC, rc.right - 50, GRID_START_Y - 5);
			nRightLimit = 50;
			for (nDescriptor = 0; nDescriptor < MAX_SDT_EXTRA_DESCRIPTORS; nDescriptor++)
			{
				if (v->epg.pSelectedEvent->pExtraDescriptors[nDescriptor] != NULL)
				{
					switch(v->epg.pSelectedEvent->pExtraDescriptors[nDescriptor][0])
					{
					case 0x86:
						fCaptions = TRUE;
						break;
					case 0x87:
						{
							char * szRatingStart;
							char szRating[256] = {0};

							DecodeATSCContentAdvisoryDescriptor(szRating, v->epg.pSelectedEvent->pExtraDescriptors[nDescriptor], TRUE);
							strupr(szRating);
							if (memcmp(szRating, "TV-Y7", 5) == 0)
								hRatingIcon = v->epg.hTV_Y7;
							else if (memcmp(szRating, "TV-Y", 4) == 0)
								hRatingIcon = v->epg.hTV_Y;
							else if (memcmp(szRating, "TV-G", 4) == 0)
								hRatingIcon = v->epg.hTV_G;
							else if (memcmp(szRating, "TV-PG", 5) == 0)
								hRatingIcon = v->epg.hTV_PG;
							else if (memcmp(szRating, "TV-14", 5) == 0)
								hRatingIcon = v->epg.hTV_14;
							else if (memcmp(szRating, "TV-MA", 5) == 0)
								hRatingIcon = v->epg.hTV_MA;							
							DecodeATSCContentAdvisoryDescriptor(szRating, v->epg.pSelectedEvent->pExtraDescriptors[nDescriptor], FALSE);
							
							// Chop off the trailing CR
							szRating[lstrlen(szRating) - 2] = '\0';
							szRatingStart = strstr(szRating, "Region");
							lstrcat(szEventDescription, " [");
							lstrcat(szEventDescription, szRatingStart);							
							lstrcat(szEventDescription, "]");
						}
						break;
					}			
				}
			}
			if (fCaptions == TRUE)
			{
				DrawIcon(hDC, rc.right - 42, 6, v->epg.hCC);
			}
				nRatingYOffset = 22;
			if (hRatingIcon != NULL)
				DrawIcon(hDC, rc.right - 43, 6 + nRatingYOffset, hRatingIcon);
		}

		{
			RECT rcText;

			SelectObject(hDC, v->epg.hEventSmallTextFont);

			rcText.left = sizeEventName.cx + 10;
			if (sizeEventDuration.cx + 5 > rcText.left)
				rcText.left = sizeEventDuration.cx + 10;
			if (sizeEventDate.cx + 5 > rcText.left)
				rcText.left = sizeEventDate.cx + 10;
			rcText.top = 5;
			rcText.bottom = GRID_START_Y - 5 - 1;
			rcText.right = rc.right - 20 - nRightLimit;

			SelectObject(hDC, GetStockObject(WHITE_PEN));
			MoveToEx(hDC, rcText.left - 3, 3, NULL);
			LineTo(hDC, rcText.left - 3, GRID_START_Y - 5);

			if (lstrlen(szEventDescription))
				DrawText(hDC, szEventDescription, -1, &rcText, DT_VCENTER | DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
		}
	}

	// Details for any selected channel
	if (v->epg.nSelectedChannel != -1)
	{
		SIZE sizeText;
		char szChannelName[MAX_PATH] = {0};

		SetTextColor(hDC, v->dwEPGMainTextColor);	
		SelectObject(hDC, v->epg.hTitleEventFontBold);
		if (v->pChannelData[v->epg.nSelectedChannel] != NULL)
		{
			lstrcpy(szChannelName, v->pChannelData[v->epg.nSelectedChannel]->szShortName);
			if (lstrlen(v->pChannelData[v->epg.nSelectedChannel]->szLongName))
			{
				lstrcat(szChannelName, " (");
				lstrcat(szChannelName, v->pChannelData[v->epg.nSelectedChannel]->szLongName);
				lstrcat(szChannelName, ")");
			}
		}
		GetTextExtentPoint(hDC, szChannelName, lstrlen(szChannelName), &sizeText);
		TextOut(hDC, 5, 5, szChannelName, lstrlen(szChannelName));
		if (v->nNetworkPID == 0x1ffb)
		{
			wsprintf(szTemp, "MPEG-2 Program %d Virtual Channel %d.%d",
				     v->epg.nSelectedChannel,
					 v->pChannelData[v->epg.nSelectedChannel]->nMajorChannelNumber,
					 v->pChannelData[v->epg.nSelectedChannel]->nMinorChannelNumber);
			SelectObject(hDC, v->epg.hGridSmallTextFont);
			TextOut(hDC, 5, 5 + sizeText.cy, szTemp, lstrlen(szTemp));
		}
		else if (v->nNetworkPID == 0x0010)
		{
			if (v->pChannelData[v->epg.nSelectedChannel] != NULL)
			{
				QuickFormatNIT(szTemp, v->pChannelData[v->epg.nSelectedChannel]->nTransportStreamID, FALSE);
				SelectObject(hDC, v->epg.hGridSmallTextFont);
				TextOut(hDC, 5, 5 + sizeText.cy, szTemp, lstrlen(szTemp));
			}
		}
	}

	// Time grid on top
	if (v->fEPGTimeGrid == TRUE && v->fEPGTimeGridOnTop)
	{
		SIZE sizeText;
		char szTemp[] = {"X"};

		nCurrentX = 100;
		nCurrentY = GRID_START_Y;
		SelectObject(hDC, v->epg.hGridTextFont);
		GetTextExtentPoint(hDC, szTemp, lstrlen(szTemp), &sizeText);
		do
		{
			FileTimeToSystemTime((FILETIME *)&nCurrentDisplayTime, &st);
			SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
			if (!(v->nEPGHalfHourWidth == 1 && stLocal.wMinute != 00))
			{
				SelectObject(hDC, v->epg.hTimeGridPen);
				MoveToEx(hDC, nCurrentX, nCurrentY + sizeText.cy, NULL);
				LineTo(hDC, nCurrentX, rc.bottom);
			}
			nCurrentDisplayTime += (__int64)10000000 * (__int64)30 * (__int64)60;
			nCurrentX += 30 * v->nEPGHalfHourWidth;
		} while (nCurrentX < rc.right);
	}

WindupEPGGridPaint:
	BitBlt(hRealDC, 0, 0, rc.right, rc.bottom, hDC, 0, 0, SRCCOPY);
	DeleteObject(memBM);
	DeleteDC(hDC);
	EndPaint(hWnd, &ps);
}

void UpdateHorizontalScroll(HWND hWnd)
{
	__int64 nTimeOffset;

	SetScrollPos(hWnd, SB_HORZ, v->epg.nHorizontalScrollPos, TRUE);
	nTimeOffset = v->epg.nHorizontalScrollPos - 32500;
	nTimeOffset *= (__int64)10000000 * (__int64)30 * (__int64)60;
	v->epg.nStartDisplayTime = v->epg.nActualStartTime + nTimeOffset;
	InvalidateRect(hWnd, NULL, FALSE);
}

void UpdateVerticalScroll(HWND hWnd)
{
	SetScrollPos(hWnd, SB_VERT, v->epg.nVerticalScrollPos, TRUE);	
	InvalidateRect(hWnd, NULL, FALSE);
}

void SetupGridFonts(HDC hDC)
{
	v->epg.hGridTextFont = CreateFont(-MulDiv(100, GetDeviceCaps(hDC, LOGPIXELSY), 720),
							   0,
							   0,
							   0,
							   400,
							   FALSE,
							   FALSE,
							   FALSE,
							   ANSI_CHARSET,
							   OUT_DEFAULT_PRECIS,
							   CLIP_DEFAULT_PRECIS,
							   ANTIALIASED_QUALITY,
							   FF_DONTCARE | VARIABLE_PITCH,
							   "Arial");	
	v->epg.hGridTextFontBold = CreateFont(-MulDiv(100, GetDeviceCaps(hDC, LOGPIXELSY), 720),
							   0,
							   0,
							   0,
							   900,
							   FALSE,
							   FALSE,
							   FALSE,
							   ANSI_CHARSET,
							   OUT_DEFAULT_PRECIS,
							   CLIP_DEFAULT_PRECIS,
							   ANTIALIASED_QUALITY,
							   FF_DONTCARE | VARIABLE_PITCH,
							   "Arial");	
	v->epg.hGridSmallTextFont = CreateFont(-MulDiv(70, GetDeviceCaps(hDC, LOGPIXELSY), 720),
							   0,
							   0,
							   0,
							   400,
							   FALSE,
							   FALSE,
							   FALSE,
							   ANSI_CHARSET,
							   OUT_DEFAULT_PRECIS,
							   CLIP_DEFAULT_PRECIS,
							   ANTIALIASED_QUALITY,
							   FF_DONTCARE | VARIABLE_PITCH,
							   "Arial");	
	v->epg.hEventSmallTextFont = CreateFont(-MulDiv(90, GetDeviceCaps(hDC, LOGPIXELSY), 720),
							   0,
							   0,
							   0,
							   400,
							   FALSE,
							   FALSE,
							   FALSE,
							   ANSI_CHARSET,
							   OUT_DEFAULT_PRECIS,
							   CLIP_DEFAULT_PRECIS,
							   ANTIALIASED_QUALITY,
							   FF_DONTCARE | VARIABLE_PITCH,
							   "Arial");	

	v->epg.hTitleEventFontBold = CreateFont(-MulDiv(140, GetDeviceCaps(hDC, LOGPIXELSY), 720),
							   0,
							   0,
							   0,
							   900,
							   FALSE,
							   FALSE,
							   FALSE,
							   ANSI_CHARSET,
							   OUT_DEFAULT_PRECIS,
							   CLIP_DEFAULT_PRECIS,
							   ANTIALIASED_QUALITY,
							   FF_DONTCARE | VARIABLE_PITCH,
							   "Arial");	

}

void SetupHorizontalScrollRange(HWND hWnd)
{
	SCROLLINFO si;

	// 65,000 horizontal and we start in the middle. each step is 30 minutes
	si.cbSize = sizeof(SCROLLINFO);
	si.fMask = SIF_POS | SIF_RANGE;
	si.nMax = 65000;
	si.nMin = 0;
	si.nPos = v->epg.nHorizontalScrollPos = 65000 / 2;
	SetScrollInfo(hWnd, SB_HORZ, &si, TRUE);
}

void SetEPGOffsetNearChannel(int nProgram)
{
	int nIndex = 0;
	int nItemCount = 0;

	if (v->epg.fDisplayEPGByLCN == FALSE)
	{
		while (nIndex < MAX_EIT_CHANNEL_DATA)
		{
			if (v->pChannelData[nIndex] != NULL)
			{
				nItemCount++;
				if (nIndex >= nProgram)
				{

					v->epg.nVerticalScrollPos = nItemCount;
					UpdateVerticalScroll(v->hWndEPGGrid);
					break;
				}
			}
			nIndex++;
		};
	}
	else
	{
		nIndex = -1;
		while (nIndex++ < MAX_EIT_CHANNEL_DATA)
		{
			int nProgramNumber = v->channel_maps[nIndex][0] & 0xffff;
		
			if (v->pChannelData[nProgramNumber] == NULL && v->pEvents[nProgramNumber] == NULL)
				continue;
			if (v->pChannelData[nProgramNumber]->nLogicalChannelNumber == 0)
				continue;

			nItemCount++;
			if (nProgramNumber == nProgram)
			{
				v->epg.nVerticalScrollPos = nItemCount;
				UpdateVerticalScroll(v->hWndEPGGrid);
				break;
			}
		};
	}
}

#ifndef LITE
BOOL SearchNextEPGEntry(HWND hWndParent)
{
	BOOL fRetVal = TRUE;

	if (!lstrlen(v->epg.szSearchString))
	{
		MessageBeep(0);
		return FALSE;
	}

	while(TRUE)
	{
		int nSearchProgram;
		PEITEVENT pCurrent;

		if (v->pEvents[v->epg.nSearchChannel] == NULL)
		{
			v->epg.nSearchProgramIndex = 0;
			v->epg.nSearchChannel++;
			if (v->epg.nSearchChannel == MAX_EIT_CHANNEL_DATA)
			{
				if (v->epg.fFoundSomething)
					MessageBeep(0);
				else
				{
					MessageBox(hWndParent, "Nothing found", gszAppName, MB_ICONINFORMATION);
					fRetVal = FALSE;
				}
				v->epg.nSearchChannel = 0;
				return fRetVal;
			}
			continue;
		}

		if (v->epg.fDisplayEPGByLCN)
		{
			int lcn = v->pChannelData[v->epg.nSearchChannel]->nLogicalChannelNumber;
			if ((int)(v->channel_maps[lcn][0] & 0xffff) != v->epg.nSearchChannel)
				goto SearchNextEPGEntry_NextChannel;
		}
		pCurrent = v->pEvents[v->epg.nSearchChannel];
		nSearchProgram = 0;
		do
		{
			char szTemp[1024 * 10];

			if (nSearchProgram >= v->epg.nSearchProgramIndex)
			{
				BOOL fMatched = FALSE;

				if (v->epg.nSearchFlag & 1)
				{
					lstrcpy(szTemp, pCurrent->szEventName);
					strlwr(szTemp);
					if (strstr(szTemp, v->epg.szSearchString) != NULL)
						fMatched = TRUE;
				}
				if (v->epg.nSearchFlag & 2)
				{
					if (pCurrent->szShortEventDescription != NULL)
					{
						lstrcpy(szTemp, pCurrent->szShortEventDescription);
						strlwr(szTemp);
						if (strstr(szTemp, v->epg.szSearchString) != NULL)
							fMatched = TRUE;
					}
					if (pCurrent->szLongEventDescription != NULL)
					{
						lstrcpy(szTemp, pCurrent->szLongEventDescription);
						strlwr(szTemp);
						if (strstr(szTemp, v->epg.szSearchString) != NULL)
							fMatched = TRUE;
					}
				}

				v->epg.nSearchProgramIndex++;
				if (fMatched)
				{
					int nScreenEventOffset = 0;
					BOOL fSetEPGOffsetNeeded = TRUE;
					__int64 lnStartTime;
					__int64 lnTimeOffset;

					v->epg.fFoundSomething = TRUE;
					v->epg.pSelectedEvent = pCurrent;
					v->epg.nSelectedChannel = -1;
#ifdef DEBUG_OUTPUT
					{
						char szTemp[256];
						wsprintf(szTemp, "TSReader EPG: Found %s %04d/%02d/%02d %02d:%02d:%02d prog %d (%d %s)\n",
							     pCurrent->szEventName,
								 pCurrent->stStartTime.wYear, pCurrent->stStartTime.wMonth, pCurrent->stStartTime.wDay,
								 pCurrent->stStartTime.wHour, pCurrent->stStartTime.wMinute, pCurrent->stStartTime.wSecond,
								 v->pChannelData[v->epg.nSearchChannel]->nChannelNumber,
								 v->pChannelData[v->epg.nSearchChannel]->nLogicalChannelNumber,
								 v->pChannelData[v->epg.nSearchChannel]->szShortName);
						OutputDebugString(szTemp);
					}
#endif DEBUG_OUTPUT

					// If event is currently on screen, it'll be highlighted when we repaint
					while (nScreenEventOffset < MAX_SCREEN_EVENTS && v->epg.screenevents[nScreenEventOffset].rc.top != 0)
					{
						if (v->epg.screenevents[nScreenEventOffset].pEITEvent == v->epg.pSelectedEvent)
						{
							InvalidateRect(v->hWndEPGGrid, NULL, FALSE);
							return fRetVal;
						}
						if (v->epg.screenevents[nScreenEventOffset].nChannel == v->epg.nSearchChannel)
							fSetEPGOffsetNeeded = FALSE;
						nScreenEventOffset++;
					}

					// Not on the screen so scroll there
					SystemTimeToFileTime(&pCurrent->stStartTime, (FILETIME *)&lnStartTime);
					lnTimeOffset = lnStartTime - v->epg.nActualStartTime;
					lnTimeOffset /= (__int64)10000000 * (__int64)30 * (__int64)60;
					v->epg.nHorizontalScrollPos = 32500 + (int)lnTimeOffset - 1;
					if (fSetEPGOffsetNeeded)
						SetEPGOffsetNearChannel(v->epg.nSearchChannel);
					UpdateHorizontalScroll(v->hWndEPGGrid);
					return fRetVal;
				}
			}
			nSearchProgram++;
			pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
		} while (pCurrent != NULL);
SearchNextEPGEntry_NextChannel:
		v->epg.nSearchProgramIndex = 0;
		v->epg.nSearchChannel++;
	};
}


BOOL CALLBACK EPGGotoDateDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			SYSTEMTIME st;

			FileTimeToSystemTime((FILETIME *)&v->epg.nActualStartTime, &st);
			DateTime_SetSystemtime(GetDlgItem(hDlg, IDC_MONTHCALENDAR), GDT_VALID, &st);
			SetFocus(GetDlgItem(hDlg, IDOK));
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			{
				SYSTEMTIME st;
							
				memset(&st, 0, sizeof(st));
				DateTime_GetSystemtime(GetDlgItem(hDlg, IDC_MONTHCALENDAR), &st);
				st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
				SystemTimeToFileTime(&st, (FILETIME *)&v->epg.nStartDisplayTime);
				v->epg.nActualStartTime = v->epg.nStartDisplayTime;
				SetupHorizontalScrollRange(GetParent(hDlg));
				EndDialog(hDlg, TRUE);
			}
			break;
		}
		break;
	}

	return FALSE;
}

BOOL CALLBACK EPGFindStringDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		SetDlgItemText(hDlg, IDD_EPG_SEARCH_STRING, v->epg.szSearchString);
		if (v->epg.nSearchFlag & 1)
			CheckDlgButton(hDlg, IDC_EPG_SEARCH_TITLE, BST_CHECKED);
		if (v->epg.nSearchFlag & 2)
			CheckDlgButton(hDlg, IDC_EPG_SEARCH_DESCRIPTION, BST_CHECKED);
		SendDlgItemMessage(hDlg, IDD_EPG_SEARCH_STRING, EM_SETSEL, 0, -1);
		SetFocus(GetDlgItem(hDlg, IDD_EPG_SEARCH_STRING));
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			v->epg.nSearchFlag = 0;
			if (IsDlgButtonChecked(hDlg, IDC_EPG_SEARCH_TITLE))
				v->epg.nSearchFlag |= 1;
			if (IsDlgButtonChecked(hDlg, IDC_EPG_SEARCH_DESCRIPTION))
				v->epg.nSearchFlag |= 2;
			if (v->epg.nSearchFlag == 0)
			{
				MessageBox(hDlg, "Please select what you wish to search", gszAppName, MB_ICONWARNING);
				break;
			}
			GetDlgItemText(hDlg, IDD_EPG_SEARCH_STRING, v->epg.szSearchString, sizeof(v->epg.szSearchString));
			strlwr(v->epg.szSearchString);
			v->epg.nSearchChannel = 0;
			v->epg.nSearchProgramIndex = 0;
			v->epg.fFoundSomething = FALSE;
			if (SearchNextEPGEntry(hDlg) == TRUE)
				EndDialog(hDlg, TRUE);
			else
			{
				SendDlgItemMessage(hDlg, IDD_EPG_SEARCH_STRING, EM_SETSEL, 0, -1);
				SetFocus(GetDlgItem(hDlg, IDD_EPG_SEARCH_STRING));
			}
			break;
		}
		break;
	case WM_DESTROY:
		InvalidateRect(GetParent(hDlg), NULL, FALSE);
		break;
	}

	return FALSE;
}

void GetEPGScheduleDisplayInfo(LV_DISPINFO *pnmv) 
{ 
    // Provide the item or subitem's text, if requested. 
    if (pnmv->item.mask & LVIF_TEXT)
	{ 
        int nScheduleIndex = (int)(pnmv->item.lParam);
		switch(pnmv->item.iSubItem)
		{
		case 0:
			if (pepgschedule[nScheduleIndex].nChannel & 0xffff0000)
			{
				int nMajorChannel = pepgschedule[nScheduleIndex].nChannel >> 24;
				int nMinorChannel = (pepgschedule[nScheduleIndex].nChannel >> 16) & 0xff;
				wsprintf(pnmv->item.pszText, "%d.%d", nMajorChannel, nMinorChannel);
			}
			else
			{
				wsprintf(pnmv->item.pszText, "%d", pepgschedule[nScheduleIndex].nChannel);
				if (v->pChannelData[pepgschedule[nScheduleIndex].nChannel] != NULL)
				{
					char szTemp[128];
					wsprintf(szTemp, " (%s)", v->pChannelData[pepgschedule[nScheduleIndex].nChannel]->szShortName);
					lstrcat(pnmv->item.pszText, szTemp);
				}
			}
			break;
		case 1:
			{
				SYSTEMTIME st, stLocal;
				char szStartDate[128];
				char szStartTime[64];

				FileTimeToSystemTime((FILETIME *)&pepgschedule[nScheduleIndex].nStartTime, &st);
				SystemTimeToTzSpecificLocalTime(NULL, &st, &stLocal);
				GetDateFormat(LOCALE_SYSTEM_DEFAULT, DATE_SHORTDATE, &stLocal, NULL, szStartDate, sizeof(szStartDate));
				GetTimeFormat(LOCALE_USER_DEFAULT, 0, &stLocal, NULL, szStartTime, sizeof(szStartTime));		
				lstrcat(szStartDate, " ");
				lstrcat(szStartDate, szStartTime);
				lstrcpy(pnmv->item.pszText, szStartDate);
			}
			break;
		case 2:
			{
				int nHours, nMinutes, nSeconds;
				int nRunTime = pepgschedule[nScheduleIndex].nDuration;
				nHours = nRunTime / (60 * 60);
				nRunTime -= nHours * 60 * 60;
				nMinutes = nRunTime / 60;
				nRunTime -= nMinutes * 60;
				nSeconds = nRunTime;
				wsprintf(pnmv->item.pszText, "%02d:%02d:%02d", nHours, nMinutes, nSeconds);
			}
			break;
		case 3:
			lstrcpy(pnmv->item.pszText, pepgschedule[nScheduleIndex].szEventName);
			break;
		}
	}
}

void UpdateManualRecordingEndDateTime(HWND hDlg)
{
	int nDuration;
	SYSTEMTIME stDate, stTime, stDuration;
	DWORD64 dwDate;
	char szDate[128];
	char szTime[128];
	char szTemp[256];

	SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_GETSYSTEMTIME, 0, (LPARAM)&stDuration);
	nDuration = (stDuration.wHour * 60 * 60) + (stDuration.wMinute * 60) + stDuration.wSecond;
	if (nDuration == 0)
	{
		SetDlgItemText(hDlg, IDC_EPG_MANUAL_COMPLETE, "");
		ShowWindow(GetDlgItem(hDlg, IDC_EPG_MANUAL_COMPLETE_CAPTION), SW_HIDE);
		return;
	}
	SendDlgItemMessage(hDlg, IDC_EPG_DATETIMEPICKER_DATE, DTM_GETSYSTEMTIME, 0, (LPARAM)&stDate);
	SendDlgItemMessage(hDlg, IDC_EPG_DATETIMEPICKER_TIME, DTM_GETSYSTEMTIME, 0, (LPARAM)&stTime);

	stDate.wHour = stTime.wHour;
	stDate.wMinute = stTime.wMinute;
	stDate.wSecond = stTime.wSecond;
	stDate.wMilliseconds = 0;

	SystemTimeToFileTime(&stDate, (FILETIME *)&dwDate);
	dwDate += (DWORD64)nDuration * (DWORD64)10000000;
	FileTimeToSystemTime((FILETIME *)&dwDate, &stDuration);
	GetDateFormat(LOCALE_USER_DEFAULT, DATE_LONGDATE, &stDuration, NULL, szDate, sizeof(szDate));
	GetTimeFormat(LOCALE_USER_DEFAULT, 0, &stDuration, NULL, szTime, sizeof(szTime));		
	wsprintf(szTemp, "%s %s", szDate, szTime);
	SetDlgItemText(hDlg, IDC_EPG_MANUAL_COMPLETE, szTemp);
	ShowWindow(GetDlgItem(hDlg, IDC_EPG_MANUAL_COMPLETE_CAPTION), SW_SHOW);
}

BOOL CALLBACK ManualEPGNoSourceParametersDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		SetFocus(GetDlgItem(hDlg, IDOK));
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			EndDialog(hDlg, TRUE);
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
		case IDC_DONT_ASK:
			v->fManualEPGNoSourceParametersWarning = IsDlgButtonChecked(hDlg, IDC_DONT_ASK);
			break;
		}
		break;
	}

	return FALSE;
}

void CheckForBadFilenameChars(char * szOutputName)
{
	int i;
	for (i = 0; i < lstrlen(szOutputName); i++)
	{
		if (   (szOutputName[i] == ':') 
			|| (szOutputName[i] == '\\') 
			|| (szOutputName[i] == '"')
			|| (szOutputName[i] == '.')
			|| (szOutputName[i] == '?')
			|| (szOutputName[i] == '*')
			|| (szOutputName[i] == '@')
			|| (szOutputName[i] == '/') )
			szOutputName[i] = '_';
	}
}

int SpecificLocalTimeToSystemTime(SYSTEMTIME* i_stLocal, SYSTEMTIME* o_stUniversal)
{

    FILETIME ft, ft_utc;

    if (!(SystemTimeToFileTime(i_stLocal, &ft) && 
          LocalFileTimeToFileTime(&ft, &ft_utc) &&
          FileTimeToSystemTime(&ft_utc,o_stUniversal)))
         {
            return 0;
         }

        return 1;
}

BOOL ScheduleManualRecording(HWND hDlg)
{
	int nProgramNumber;
	int nDuration;
	SYSTEMTIME stLocalStart, stTime, stDuration;
	char * szSourceName;
	BYTE * pScheduleData;

	char szSourceParameters[256];
	char szExecutable[MAX_PATH];
	char szTaskName[256];
	char szComment[256] = {""};
	char szParameters[256];
	char szRecordFileName[MAX_PATH];
	char szHexEPGScheduleData[2048] = {""};
	char szProfile[256] = {""};
	char szProgramStreamRecordSwitch[16] = {""};

	nProgramNumber = GetDlgItemInt(hDlg, IDC_EPG_MANUAL_PROGRAM, NULL, FALSE);
	if (nProgramNumber == 0)
	{
		MessageBox(hDlg, "Please set the MPEG program number", gszAppName, MB_ICONWARNING);
		SetFocus(GetDlgItem(hDlg, IDC_EPG_MANUAL_PROGRAM));
		return FALSE;
	}
	GetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szSourceParameters, sizeof(szSourceParameters));
	if (!lstrlen(szSourceParameters) && !v->fManualEPGNoSourceParametersWarning)
	{
		if (DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_MANUAL_NO_SOURCE_PARAMETERS), hDlg, ManualEPGNoSourceParametersDlgProc) == FALSE)
		{
			SetFocus(GetDlgItem(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS));
			return FALSE;
		}
	}
	
	// Get the times
	SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_GETSYSTEMTIME, 0, (LPARAM)&stDuration);
	nDuration = (stDuration.wHour * 60 * 60) + (stDuration.wMinute * 60) + stDuration.wSecond;
	if (nDuration == 0)
	{
		MessageBox(hDlg, "Please set the recording duration", gszAppName, MB_ICONWARNING);
		SetFocus(GetDlgItem(hDlg, IDC_EPG_MANUAL_DURATION));
		return FALSE;
	}
	SendDlgItemMessage(hDlg, IDC_EPG_DATETIMEPICKER_DATE, DTM_GETSYSTEMTIME, 0, (LPARAM)&stLocalStart);
	SendDlgItemMessage(hDlg, IDC_EPG_DATETIMEPICKER_TIME, DTM_GETSYSTEMTIME, 0, (LPARAM)&stTime);
	stLocalStart.wHour = stTime.wHour;
	stLocalStart.wMinute = stTime.wMinute;
	stLocalStart.wSecond = stTime.wSecond;
	stLocalStart.wMilliseconds = 0;

	// Come up with the recording filename
	wsprintf(szTaskName, "%04d%02d%02d:%02d%02d%02d-%d - Manual Recording",
			 stLocalStart.wYear, stLocalStart.wMonth, stLocalStart.wDay,
			 stLocalStart.wHour, stLocalStart.wMinute, stLocalStart.wSecond,
			 nProgramNumber);
	CheckForBadFilenameChars(szTaskName);
	wsprintf(szRecordFileName, "%s%s.mpg", v->szSchedulerDirectory, szTaskName);

	// Parameters
#ifdef PRO
	if (lstrlen(v->szProfileName))
		wsprintf(szProfile, "-L \"%s\" ", v->szProfileName);
#endif PRO
	for (szSourceName = v->szSourceName + lstrlen(v->szSourceName); szSourceName > v->szSourceName; szSourceName--)
	{
		if (*(szSourceName - 1) == '\\')
			break;
	}
	if (v->fEPGRecordPS)
		lstrcpy(szProgramStreamRecordSwitch, "-p ");

	wsprintf(szParameters, "%s-s %s -Mm -i %s-r %d \"%s\" %d %s",
		     szProfile,
			 szSourceName,
			 szProgramStreamRecordSwitch,
			 nProgramNumber,
			 szRecordFileName,
			 nDuration - 1,
			 szSourceParameters);
	GetModuleFileName(v->hInstance, szExecutable, sizeof(szExecutable));	

	pepgschedule[nEPGScheduleItems].nChannel = nProgramNumber;
	pepgschedule[nEPGScheduleItems].nDuration = nDuration;
	pepgschedule[nEPGScheduleItems].wPreRoll = 0;
	pepgschedule[nEPGScheduleItems].wPostRoll = 0;
	SystemTimeToFileTime(&stLocalStart, (FILETIME*)&pepgschedule[nEPGScheduleItems].nStartTime);
	lstrcpy(pepgschedule[nEPGScheduleItems].szEventName, "Manual Recording");

	// Encode the pepgschedule for this event into ASCII hex
	// so we can get it back later
	{
		int nIndex;
		pScheduleData = (BYTE *)&pepgschedule[nEPGScheduleItems];
		for (nIndex = 0; nIndex < sizeof(EPGSCHEDULE); nIndex++)
		{
			char szTemp[16];
			wsprintf(szTemp, "%02x", *(pScheduleData));
			pScheduleData++;
			lstrcat(szHexEPGScheduleData, szTemp);
		}
	}
	nEPGScheduleItems++;
	if (nEPGScheduleItems == nEPGScheduleMax)
	{
		MessageBox(hDlg, "Too many scheduled events - please tell support@tsreader.co.uk you saw this", gszAppName, MB_ICONSTOP);
	}
	wsprintf(szComment, "Do NOT alter this! TSReader:[%s]", szHexEPGScheduleData);
	
	// Now we can schedule this recording
	CursorWait(hDlg);
	{
		SYSTEMTIME stUniversal;

		SpecificLocalTimeToSystemTime(&stLocalStart, &stUniversal);
		memcpy(&stLocalStart, &stUniversal, sizeof(stUniversal));
	}
	if (Scheduler_CreateNewTask(szExecutable,
								szParameters,
								szTaskName,
								szComment,
								v->szSchedulerUsername,
								v->szSchedulerPassword,
								&stLocalStart,
								nDuration - 1,
								FALSE,
								v->fSchedulerRequiresLogin,
								v->fSchedulerWake) == FALSE)
	{
		nEPGScheduleItems--;
		pepgschedule[nEPGScheduleItems].nChannel = 0;
		pepgschedule[nEPGScheduleItems].nDuration = 0;
		pepgschedule[nEPGScheduleItems].nStartTime = 0;
 		MessageBox(hDlg, "Failed to create a scheduled event correctly - sure you set username/password right?", gszAppName, MB_ICONSTOP);
		return FALSE;
	}
	CursorNormal();
	return TRUE;
}

BOOL CALLBACK EPGManualScheduleDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			int nPMTIndex;
			int nPrograms = 0;
			int nFirstProgramIndex = -1;
			SYSTEMTIME st;
			char szSourceParameters[128];

			BOOL (* GetDescription) (char * szDescription, char * szCommandLineParameters, BOOL * fCanBeStopped, int * nMaxPIDs, DWORD * dwCapabilities);
			GetDescription = (td_GetDescription)GetProcAddress(v->hSource, "TSReader_GetDescription");
			GetDescription(NULL, szSourceParameters, NULL, NULL, NULL);

			SetDlgItemText(hDlg, IDC_EPG_MANUAL_COMPLETE, "");
			SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS_FORMAT, szSourceParameters);
			GetLocalTime(&st);
			st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
			SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
			SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_SETFORMAT, 0, (LPARAM)"HH:mm:ss");

			// If only one program, setup that program as default
			for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
			{
				if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
					break;
				if (v->pat.pmt[nPMTIndex].nProgramNumber)
				{
					nPrograms++;
					if (nFirstProgramIndex == -1)
						nFirstProgramIndex = nPMTIndex;
				}
			}
			if (nPrograms == 1)
				SetDlgItemInt(hDlg, IDC_EPG_MANUAL_PROGRAM, v->pat.pmt[nFirstProgramIndex].nProgramNumber, FALSE);
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			if (ScheduleManualRecording(hDlg) == TRUE)
				EndDialog(hDlg, TRUE);
			break;
		case IDC_EPG_MANUAL_ADD_15:
		case IDC_EPG_MANUAL_ADD_30:
		case IDC_EPG_MANUAL_ADD_60:
			{
				DWORD64 dwNewTime;
				SYSTEMTIME st;
				int nAddSeconds = 15 * 60;

				if (LOWORD(wParam) == IDC_EPG_MANUAL_ADD_30)
					nAddSeconds = 30 * 60;
				else if (LOWORD(wParam) == IDC_EPG_MANUAL_ADD_60)
					nAddSeconds = 60 * 60;

				SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_GETSYSTEMTIME, 0, (LPARAM)&st);
				SystemTimeToFileTime(&st, (FILETIME *)&dwNewTime);
				dwNewTime += (DWORD64)nAddSeconds * (DWORD64)10000000;
				FileTimeToSystemTime((FILETIME *)&dwNewTime, &st);
				SendDlgItemMessage(hDlg, IDC_EPG_MANUAL_DURATION, DTM_SETSYSTEMTIME, GDT_VALID, (LPARAM)&st);
				UpdateManualRecordingEndDateTime(hDlg);
			}
			break;
		case IDC_EPG_MANUAL_CURRENT_MUX:
			{
				HWND hWndButton = (HWND)lParam;
				RECT rcButton;
				POINT pt;
				HMENU hMenu;            // menu template         
				HMENU hMenuTrackPopup;  // floating pop-up menu

				GetWindowRect(hWndButton, &rcButton);

				// Get the client coordinates for the mouse click
				pt.x = rcButton.right; 
				pt.y = rcButton.top; 
				hMenu = LoadMenu(v->hInstance, MAKEINTRESOURCE(IDR_MANUAL_EPG_CURRENT)); 
				if (hMenu == NULL) 
					break; 

				// Get the first pop-up menu in the menu template. This is the
				// menu that TrackPopupMenu displays. 
				hMenuTrackPopup = GetSubMenu(hMenu, 0);
				//ClientToScreen(hDlg, (LPPOINT) &pt); 
				TrackPopupMenu(hMenuTrackPopup, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, hDlg, NULL); 
				DestroyMenu(hMenu); 

			}
			break;
		case ID_IDRMANUALEPGCURRENT_FORDVBSTUNERS:
			{
				char szCommandLine[256];

				wsprintf(szCommandLine, "%d %d %d %d %d",
					     v->ss.nFrequency,
						 v->ss.nPolarity,
						 v->ss.nSymbolRate,
						 v->ss.nLNBFrequency,
						 v->ss.n22KHz);
				if (v->ss.nDiSEqCInput)
				{
					char szTemp[16];
					wsprintf(szTemp, " %d", v->ss.nDiSEqCInput);
					lstrcat(szCommandLine, szTemp);
				}
				SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szCommandLine);
			}
			break;
		case ID_IDRMANUALEPGCURRENT_FORADVANCEDSATELLITETUNERS:
			{
				char szCommandLine[256];

				wsprintf(szCommandLine, "%d %d %d %d %d %d %d",
					     v->ss.nFrequency,
						 v->ss.nPolarity,
						 v->ss.nSymbolRate,
						 v->ss.nLNBFrequency,
						 v->ss.n22KHz,
						 v->ss.nADVModulationMode,
						 v->ss.nCodeRate);
				if (v->ss.nDiSEqCInput)
				{
					char szTemp[16];
					wsprintf(szTemp, " %d", v->ss.nDiSEqCInput);
					lstrcat(szCommandLine, szTemp);
				}
				SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szCommandLine);
			}
			break;
		case ID_IDRMANUALEPGCURRENT_FORDVBTTUNERS:
			{
				char szCommandLine[256];

				wsprintf(szCommandLine, "%d %d %d",
					     v->ss.nFrequency,
						 v->ss.fSpectrumInversion,
						 v->ss.nBandwidth);
				SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szCommandLine);
			}
			break;
		case ID_IDRMANUALEPGCURRENT_FORDVBCTUNERS:
			{
				char szCommandLine[256];

				wsprintf(szCommandLine, "%d %d %d %d",
					     v->ss.nFrequency,
						 v->ss.nSymbolRate,
						 v->ss.nQAM,
						 v->ss.fSpectrumInversion);
				SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szCommandLine);
			}
			break;
		case ID_IDRMANUALEPGCURRENT_FORATSCTUNERS:
			{
				char szCommandLine[256];

				wsprintf(szCommandLine, "%d",
					     v->ss.nFrequency);
				SetDlgItemText(hDlg, IDC_EPG_MANUAL_SOURCE_PARAMETERS, szCommandLine);
			}
			break;
		}
		break;
	case WM_NOTIFY:
		switch (((LPNMHDR) lParam)->code)
		{ 
		case DTN_DATETIMECHANGE:
			UpdateManualRecordingEndDateTime(hDlg);
			break;
		}
		break;
	}

	return FALSE;
}

BOOL CALLBACK EPGShowScheduleDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			int nColumnPosition = 0;
			int nIndex;
			HWND hWndLV = GetDlgItem(hDlg, IDC_EPG_SCHEDULE_LIST);
			LV_COLUMN lvc; 
			char szTemp[128];

			// Setup list view columns. 
			lvc.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM; 
			lvc.pszText = szTemp; 

			lvc.fmt = LVCFMT_LEFT; 
			lvc.cx = 100; 
			lstrcpy(szTemp, TEXT("Channel"));
			ListView_InsertColumn(hWndLV, nColumnPosition++, &lvc); 

			lvc.fmt = LVCFMT_LEFT; 
			lvc.cx = 130; 
			lstrcpy(szTemp, TEXT("Date & Time"));
			ListView_InsertColumn(hWndLV, nColumnPosition++, &lvc); 

			lvc.fmt = LVCFMT_LEFT; 
			lvc.cx = 60; 
			lstrcpy(szTemp, TEXT("Duration"));
			ListView_InsertColumn(hWndLV, nColumnPosition++, &lvc); 

			lvc.fmt = LVCFMT_LEFT; 
			lvc.cx = 200; 
			lstrcpy(szTemp, TEXT("Program"));
			ListView_InsertColumn(hWndLV, nColumnPosition++, &lvc); 

			ListView_SetExtendedListViewStyle(hWndLV, LVS_EX_FULLROWSELECT);

			// Populate list view
			for (nIndex = 0; nIndex < nEPGScheduleItems; nIndex++)
			{
				LV_ITEM lvi; 
				memset(&lvi, 0, sizeof(lvi));
				lvi.state = 0; 
				lvi.stateMask = 0; 
				lvi.pszText = LPSTR_TEXTCALLBACK;   // app. maintains text 
				lvi.mask = LVIF_TEXT | LVIF_PARAM | LVIF_STATE; 
				lvi.iItem = 0; 
				lvi.iSubItem = 0; 
				lvi.lParam = (LPARAM)nIndex;    // item data 
				ListView_InsertItem(hWndLV, &lvi);
			}
			SetFocus(GetDlgItem(hDlg, IDOK));
		}
		break;
	case WM_NOTIFY: 
		switch (((LPNMHDR) lParam)->code)
		{ 
		case LVN_GETDISPINFO:
			{
				NMLVDISPINFO * pnmv = (NMLVDISPINFO *)lParam;
				GetEPGScheduleDisplayInfo((LV_DISPINFO *) lParam);
			}
			break;
		case NM_DBLCLK:
			//PostMessage(hDlg, WM_COMMAND, IDC_EDIT_MANUAL_CHANNEL, 0);
			break;
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			EndDialog(hDlg, TRUE);
			break;
		}
		break;
	}

	return FALSE;
}

BOOL CALLBACK EPGGotoChannelDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		SetDlgItemInt(hDlg, IDC_GOTO_CHANNEL_NUMBER, v->epg.nGotoChannel, FALSE);
		SendDlgItemMessage(hDlg, IDC_GOTO_CHANNEL_NUMBER, EM_SETSEL, 0, -1);
		SetFocus(GetDlgItem(hDlg, IDC_GOTO_CHANNEL_NUMBER));
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		case IDOK:
			{
				char szChannelName[128];
				
				GetDlgItemText(hDlg, IDC_GOTO_CHANNEL_NUMBER, szChannelName, sizeof(szChannelName));
				if (szChannelName[0] < '0' || szChannelName[0] > '9')
				{
					int nChannel;

					strlwr(szChannelName);
					for (nChannel = 0; nChannel < MAX_EIT_CHANNEL_DATA; nChannel++)
					{
						if (v->pChannelData[nChannel] != NULL)
						{
							char szShortName[256];

							lstrcpy(szShortName, v->pChannelData[nChannel]->szShortName);
							strlwr(szShortName);
							if (strstr(szShortName, szChannelName) != NULL)
							{
								v->epg.nGotoChannel = nChannel;
								break;
							}
						}
					}
					if (nChannel == MAX_EIT_CHANNEL_DATA)
					{
						MessageBox(hDlg, "Unable to locate any channel with that name", gszAppName, MB_ICONINFORMATION);
						SendDlgItemMessage(hDlg, IDC_GOTO_CHANNEL_NUMBER, EM_SETSEL, 0, -1);
						SetFocus(GetDlgItem(hDlg, IDC_GOTO_CHANNEL_NUMBER));
						return FALSE;
					}
				}
				else
					v->epg.nGotoChannel = GetDlgItemInt(hDlg, IDC_GOTO_CHANNEL_NUMBER, NULL, FALSE);
				SetEPGOffsetNearChannel(v->epg.nGotoChannel);
			}
			EndDialog(hDlg, TRUE);
			break;
		}
		break;
	}

	return FALSE;
}

BOOL CALLBACK EPGSchedulerWarningDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		SetFocus(GetDlgItem(hDlg, IDOK));
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			EndDialog(hDlg, TRUE);
			break;
		case IDC_DONT_SHOW:
			v->fDontShowSchedulerWarning = IsDlgButtonChecked(hDlg, IDC_DONT_SHOW);
			break;
		}
		break;
	}

	return FALSE;
}

void SchedulerStartup(HWND hWnd)
{
	char szDLLName[MAX_PATH];

	SourceHelper_GetTSReaderEXEDirectory(v->hInstance, szDLLName, sizeof(szDLLName));
	lstrcat(szDLLName, "\\TSReader_Scheduler.dll");
	v->epg.hSchedulerDLL = LoadLibrary(szDLLName);
	if (v->epg.hSchedulerDLL != NULL)
	{
		Scheduler_EnsureServiceRunning = (tdScheduler_EnsureServiceRunning)GetProcAddress(v->epg.hSchedulerDLL, "Scheduler_EnsureServiceRunning");
		Scheduler_CreateNewTask = (tdScheduler_CreateNewTask)GetProcAddress(v->epg.hSchedulerDLL, "Scheduler_CreateNewTask");
		Scheduler_EnumTasks = (tdScheduler_EnumTasks)GetProcAddress(v->epg.hSchedulerDLL, "Scheduler_EnumTasks");
		Schedule_DeleteSchedule = (tdSchedule_DeleteSchedule)GetProcAddress(v->epg.hSchedulerDLL, "Schedule_DeleteSchedule");
		
		v->epg.fWindowsSchedulerActive = Scheduler_EnsureServiceRunning();
		if (v->epg.fWindowsSchedulerActive == FALSE)
		{
			if (!v->fDontShowSchedulerWarning)
				DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_SCHEDULER_WARNING), hWnd, EPGSchedulerWarningDlgProc);
		}
		else
		{
			char * szSourceNamePtr;

			szSourceNamePtr = v->szSourceName + lstrlen(v->szSourceName);
			while (szSourceNamePtr > v->szSourceName)
			{
				if (*szSourceNamePtr == '\\')
				{
					szSourceNamePtr++;
					break;
				}
				szSourceNamePtr--;
			}
			Scheduler_EnumTasks(pepgschedule, &nEPGScheduleItems, &nEPGScheduleMax, szSourceNamePtr);
		}
	}
}

BOOL EventCurrentlyRunning(PEITEVENT pEvent)
{
	SYSTEMTIME stSystemTime;
	DWORD64 lnProgramStart, lnProgramEnd, lnNow;
	DWORD64 lnMultiplier = 10000000;
	DWORD64 lnRunTime = ( (pEvent->stRunTime.wHour * 60 * 60)
					    + (pEvent->stRunTime.wMinute * 60)
						+ (pEvent->stRunTime.wSecond) ) * lnMultiplier;
	
	GetSystemTime(&stSystemTime);
	SystemTimeToFileTime(&stSystemTime, (FILETIME *)&lnNow);

	SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&lnProgramStart);
	lnProgramEnd = lnProgramStart + lnRunTime;

	if (lnNow >= lnProgramStart && lnNow <= lnProgramEnd)
		return TRUE;

	return FALSE;
}

void GetRecordTaskName(char * szTaskName, int * nDuration, SYSTEMTIME * stLocalStart)
{
	*nDuration = v->epg.pSelectedEvent->stRunTime.wHour * 60 * 60
			   + v->epg.pSelectedEvent->stRunTime.wMinute * 60
			   + v->epg.pSelectedEvent->stRunTime.wSecond;
	SystemTimeToTzSpecificLocalTime(NULL, &v->epg.pSelectedEvent->stStartTime, stLocalStart);
	if (v->fSchedulerNoDateTime == FALSE)
	{
		wsprintf(szTaskName, "Record %d (%s) - %s - %04d%02d%02d:%02d%02d%02d",
				 v->epg.nSelectedEPGChannel, 
				 v->pChannelData[v->epg.nSelectedEPGChannel]->szShortName,
				 v->epg.pSelectedEvent->szEventName,
				 stLocalStart->wYear, stLocalStart->wMonth, stLocalStart->wDay,
				 stLocalStart->wHour, stLocalStart->wMinute, stLocalStart->wSecond);
	}
	else
	{
		wsprintf(szTaskName, "Record %d (%s) - %s",
				 v->epg.nSelectedEPGChannel, 
				 v->pChannelData[v->epg.nSelectedEPGChannel]->szShortName,
				 v->epg.pSelectedEvent->szEventName);
	}
	CheckForBadFilenameChars(szTaskName);
}

BOOL CheckEPGRecordSettings(HWND hWnd)
{
	do
	{
		if (lstrlen(v->szSchedulerDirectory) == 0 || lstrlen(v->szSchedulerUsername) == 0)
		{
			if (MessageBox(hWnd, "One or more settings required for scheduled recordings are not set.\n\nWould you like to set these now?", gszAppName, MB_ICONWARNING | MB_YESNO) == IDYES)
				DialogBoxParam(v->hInstance, MAKEINTRESOURCE(IDD_EPG_GRID_SETTINGS), hWnd, EPGGridSettingsDlgProc, TRUE);				
			else
				return FALSE;
		}
		else
			break;
	} while (TRUE);

	return TRUE;
}

void ScheduleRecording(HWND hWnd, LPARAM lParam, WORD wPreRoll, WORD wPostRoll)
{
	BOOL fRunNow = FALSE;
	int nIndex;
	int nDuration;
	int nDuplicateEventIndex;
	int nMapOffset, nRecordProgram;
	BYTE * pScheduleData;
	char * szSourceName;
	SYSTEMTIME stLocalStart;
	SYSTEMTIME stActualStartTime;
	char szExecutable[MAX_PATH];
	char szTaskName[256];
	char szComment[256] = {""};
	char szParameters[256];
	char szRecordFileName[MAX_PATH];
	char szRetuneParameters[128] = {""};
	char szHexEPGScheduleData[2048] = {""};
	char szProfile[256] = {""};
	char szProgramStreamRecordSwitch[16] = {""};

	// Make sure settings are done
	if (CheckEPGRecordSettings(hWnd) == FALSE)
		return;

	// Make sure there's NIT data on DVB networks
	if (v->nNetworkPID == 0x0010)
	{
		if (!(v->dwSourceCapabilities & CAPABILITIES_TUNE_BY_CHANNEL))
		{
			if (v->pChannelData[v->epg.nSelectedEPGChannel] != NULL)
			{
				int nNITIndex;

				for (nNITIndex = 0; nNITIndex < MAX_TS_ENTRIES; nNITIndex++)
				{
					if (v->pNITData[nNITIndex] != NULL)
					{
						if (v->pNITData[nNITIndex]->nTransportStreamID == v->pChannelData[v->epg.nSelectedEPGChannel]->nTransportStreamID)
						{
							GetRetuneCommandLineParameters(szRetuneParameters, nNITIndex);
							break;
						}
					}
				}
			}
			if (lstrlen(szRetuneParameters) == 0)
			{
				MessageBox(hWnd, "Unable to locate network information required to schedule this event.\n\nNormally this is cured simply by waiting a few moments.", gszAppName, MB_ICONINFORMATION);
				return;
			}
		}
		else
		{
			// Tune by channel
			if (v->fSkyEPG)
			{
				// TV channels are normal 3 digit
				// Radio is channel >= 3100, but you actually send 0100 - very stupid
				int nSkyChannel = v->pChannelData[v->epg.nSelectedEPGChannel]->nLogicalChannelNumber;
				if (nSkyChannel >= 3100)
					wsprintf(szRetuneParameters, "0%03d", nSkyChannel - 3000);			
				else
					wsprintf(szRetuneParameters, "%d", nSkyChannel);			
			}
			else
				wsprintf(szRetuneParameters, "%d", v->pChannelData[v->epg.nSelectedEPGChannel]->nChannelNumber);
		}
	}
	else
	{
		// Command line for ATSC
		wsprintf(szRetuneParameters, "%d", v->ss.nFrequency);
	}

	// Check for events completely in the past
	if (EventInPastPC(v->epg.pSelectedEvent) == TRUE)
	{
		MessageBox(hWnd, "This event is in the past and despite many saying TSReader is great\nsoftware, it doesn't currently contain any time travel functions...", gszAppName, MB_ICONINFORMATION);
		return;
	}

	// Figure the task name
	GetRecordTaskName(szTaskName, &nDuration, &stLocalStart);
	
	// See if this is already a scheduled recording, popup the options menu
	nDuplicateEventIndex = IsRecordingScheduled(v->epg.nSelectedEPGChannel, v->epg.pSelectedEvent);
	if (nDuplicateEventIndex != -1)
	{
		POINT pt;
		HMENU hMenu;            // menu template         
		HMENU hMenuTrackPopup;  // floating pop-up menu

		// Get the client coordinates for the mouse click
		pt.x = LOWORD(lParam); 
		pt.y = HIWORD(lParam); 
		hMenu = LoadMenu(v->hInstance, MAKEINTRESOURCE(IDR_EPG_POPUP)); 
		if (hMenu == NULL) 
			return; 

		// Get the first pop-up menu in the menu template. This is the
		// menu that TrackPopupMenu displays. 
		hMenuTrackPopup = GetSubMenu(hMenu, 0);
		ClientToScreen(hWnd, (LPPOINT) &pt); 
		TrackPopupMenu(hMenuTrackPopup, TPM_LEFTALIGN | TPM_LEFTBUTTON, pt.x, pt.y, 0, hWnd, NULL); 
		DestroyMenu(hMenu); 
		return;
	}
	
	// Check for conflicting scheduled events 
	nDuplicateEventIndex = CheckForDuplicateRecording(v->epg.pSelectedEvent);
	if (nDuplicateEventIndex != -1)
	{
		__int64 nStartTime;
		char szTemp[256];
		char szChannel[16];

		SystemTimeToFileTime(&v->epg.pSelectedEvent->stStartTime, (FILETIME *)&nStartTime);		
		if (pepgschedule[nDuplicateEventIndex].nChannel & 0xffff0000)
		{
			int nMajorChannel = pepgschedule[nDuplicateEventIndex].nChannel >> 24;
			int nMinorChannel = (pepgschedule[nDuplicateEventIndex].nChannel >> 16) & 0xff;
			wsprintf(szChannel, "%d.%d", nMajorChannel, nMinorChannel);
		}
		else
			wsprintf(szChannel, "%d", pepgschedule[nDuplicateEventIndex].nChannel);

		// Do they start at the same time?
		if (pepgschedule[nDuplicateEventIndex].nStartTime == nStartTime)
		{
			wsprintf(szTemp, "Unable to schedule recording - a recording on channel %s is already scheduled to start at the same time", szChannel);
			MessageBox(hWnd, szTemp, gszAppName, MB_ICONINFORMATION);
			return;
		}
		else
		{
			wsprintf(szTemp, "This recording overlaps with a recording on channel %s. If you decide to record\nthis program, TSReader will stop the prior recording when this one starts.\n\nWould you like to record anyway?", szChannel);
			if (MessageBox(hWnd, szTemp, gszAppName, MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) == IDNO)
				return;
		}
	}

	// Check for currently running events
	if (EventCurrentlyRunning(v->epg.pSelectedEvent) == TRUE)
	{
		DWORD64 dwStartTime, dwEndTime, dwNow;
		DWORD64 dwRemainingTime;
		DWORD64 lnMultiplier = 10000000;
		SYSTEMTIME stNow, stRemaining;

		if (MessageBox(hWnd, "This program is currently running. TSReader will schedule\na task to record this program immediately and then quit.\n\nOK to continue?", gszAppName, MB_ICONQUESTION | MB_YESNO) == IDNO)
			return;

		SystemTimeToFileTime(&v->epg.pSelectedEvent->stStartTime, (FILETIME *)&dwStartTime);
		dwEndTime = dwStartTime + ((DWORD64)nDuration * lnMultiplier);
		GetSystemTime(&stNow);
		SystemTimeToFileTime(&stNow, (FILETIME *)&dwNow);
		dwRemainingTime = dwEndTime - dwNow;
		nDuration = (int)(dwRemainingTime / lnMultiplier);
		nDuration -= 4;
		fRunNow = TRUE;
		FileTimeToSystemTime((FILETIME *)&dwRemainingTime, &stRemaining);
		{
			char szTemp[128];
			wsprintf(szTemp, "Remaining: %02d:%02d:%02d\n", stRemaining.wHour, stRemaining.wMinute, stRemaining.wSecond);
			OutputDebugString(szTemp);
		}
	}

	// Come up with the recording filename
	{
		char szTemp[512];
		
		if (v->fSchedulerNoDateTime == FALSE)
		{
			wsprintf(szTemp, "%04d%02d%02d:%02d%02d%02d-%d (%s) - %s",
					 stLocalStart.wYear, stLocalStart.wMonth, stLocalStart.wDay,
					 stLocalStart.wHour, stLocalStart.wMinute, stLocalStart.wSecond,
					 v->epg.nSelectedEPGChannel, 
					 v->pChannelData[v->epg.nSelectedEPGChannel]->szShortName,
					 v->epg.pSelectedEvent->szEventName);
		}
		else
		{
			wsprintf(szTemp, "%d (%s) - %s",
					 v->epg.nSelectedEPGChannel, 
					 v->pChannelData[v->epg.nSelectedEPGChannel]->szShortName,
					 v->epg.pSelectedEvent->szEventName);
		}
		CheckForBadFilenameChars(szTemp);
		wsprintf(szRecordFileName, "%s%s.mpg", v->szSchedulerDirectory, szTemp);
	}

	// Parameters
#ifdef PRO
	if (lstrlen(v->szProfileName))
		wsprintf(szProfile, "-L \"%s\" ", v->szProfileName);
#endif PRO
	for (szSourceName = v->szSourceName + lstrlen(v->szSourceName); szSourceName > v->szSourceName; szSourceName--)
	{
		if (*(szSourceName - 1) == '\\')
			break;
	}
	if (v->fEPGRecordPS)
		lstrcpy(szProgramStreamRecordSwitch, "-p ");

	nRecordProgram = v->epg.nSelectedEPGChannel;
	for (nMapOffset = 0; nMapOffset < MAX_EPG_MAPS; nMapOffset++)
	{
		if (v->epg.nMapSourceProgram[nMapOffset] == 0)
			break;
		if (v->epg.nMapSourceProgram[nMapOffset] == nRecordProgram)
		{
			nRecordProgram = v->epg.nMapDestinationProgram[nMapOffset];
			break;
		}
	}
	wsprintf(szParameters, "%s-s %s -Mm -i %s-r %d \"%s\" %d %s",
		     szProfile,
			 szSourceName,
			 szProgramStreamRecordSwitch,
			 nRecordProgram,
			 szRecordFileName,
			 nDuration - 1 + ((wPreRoll + wPostRoll) * 60),
			 szRetuneParameters);
	GetModuleFileName(v->hInstance, szExecutable, sizeof(szExecutable));	

	pepgschedule[nEPGScheduleItems].nChannel = v->epg.nSelectedEPGChannel;
	if (v->pChannelData[v->epg.nSelectedEPGChannel] != NULL)
	{
		if (v->pChannelData[v->epg.nSelectedEPGChannel]->fATSC)
			pepgschedule[nEPGScheduleItems].nChannel |= v->pChannelData[v->epg.nSelectedEPGChannel]->nMajorChannelNumber << 24 
			                                         |  v->pChannelData[v->epg.nSelectedEPGChannel]->nMinorChannelNumber << 16;
	}
	pepgschedule[nEPGScheduleItems].nDuration = nDuration - 1;
	pepgschedule[nEPGScheduleItems].wPreRoll = wPreRoll;
	pepgschedule[nEPGScheduleItems].wPostRoll = wPostRoll;

	SystemTimeToFileTime(&v->epg.pSelectedEvent->stStartTime, (FILETIME*)&pepgschedule[nEPGScheduleItems].nStartTime);
	{
		char szTemp[256];

		lstrcpy(szTemp, v->epg.pSelectedEvent->szEventName);
		if (lstrlen(szTemp) > sizeof(pepgschedule[nEPGScheduleItems].szEventName))
			szTemp[sizeof(pepgschedule[nEPGScheduleItems].szEventName) - 1] = '\0';
		lstrcpy(pepgschedule[nEPGScheduleItems].szEventName, szTemp);
	}

	// Encode the pepgschedule for this event into ASCII hex
	// so we can get it back later
	pScheduleData = (BYTE *)&pepgschedule[nEPGScheduleItems];
	for (nIndex = 0; nIndex < sizeof(EPGSCHEDULE); nIndex++)
	{
		char szTemp[16];
		wsprintf(szTemp, "%02x", *(pScheduleData));
		pScheduleData++;
		lstrcat(szHexEPGScheduleData, szTemp);
	}
	nEPGScheduleItems++;
	if (nEPGScheduleItems == nEPGScheduleMax)
	{
		MessageBox(hWnd, "Too many scheduled events - please tell support@tsreader.co.uk you saw this", gszAppName, MB_ICONSTOP);
	}
	wsprintf(szComment, "Do NOT alter this! TSReader:[%s]", szHexEPGScheduleData);
	

	// Adjust times if we use pre/post roll
	memcpy(&stActualStartTime, &v->epg.pSelectedEvent->stStartTime, sizeof(stActualStartTime));
	if (wPreRoll)
	{
		DWORD64 dwNewStartTime;

		SystemTimeToFileTime(&stActualStartTime, (FILETIME *)&dwNewStartTime);
		dwNewStartTime -= wPreRoll * 60 * (DWORD64)10000000;
		FileTimeToSystemTime((FILETIME *)&dwNewStartTime, &stActualStartTime);
	}

	// Now we can schedule this recording
	CursorWait(hWnd);
	if (Scheduler_CreateNewTask(szExecutable,
								szParameters,
								szTaskName,
								szComment,
								v->szSchedulerUsername,
								v->szSchedulerPassword,
								&stActualStartTime,
								nDuration - 1,
								fRunNow,
								v->fSchedulerRequiresLogin,
								v->fSchedulerWake) == FALSE)
	{
		nEPGScheduleItems--;
		pepgschedule[nEPGScheduleItems].nChannel = 0;
		pepgschedule[nEPGScheduleItems].nDuration = 0;
		pepgschedule[nEPGScheduleItems].nStartTime = 0;
 		MessageBox(hWnd, "Failed to create a scheduled event correctly - sure you set username/password right?", gszAppName, MB_ICONSTOP);
	}
	CursorNormal();
	InvalidateRect(hWnd, NULL, FALSE);
	if (fRunNow)
		PostMessage(v->hWndMainWindow, WM_CLOSE, 0, 0);
}

void DeletedSelectedScheduledRecording()
{
	int nDuplicateEventIndex;

	nDuplicateEventIndex = IsRecordingScheduled(v->epg.nSelectedEPGChannel, v->epg.pSelectedEvent);
	if (nDuplicateEventIndex != -1)
	{
		int nDuration;
		SYSTEMTIME stLocalTime;
		char szTaskName[256];

		GetRecordTaskName(szTaskName, &nDuration, &stLocalTime);
		pepgschedule[nDuplicateEventIndex].nChannel = 0;
		pepgschedule[nDuplicateEventIndex].nDuration = 0;
		pepgschedule[nEPGScheduleItems].nStartTime = 0;
		memset(pepgschedule[nEPGScheduleItems].szEventName, 0, sizeof(pepgschedule[nEPGScheduleItems].szEventName));
		Schedule_DeleteSchedule(szTaskName);
	}
}

BOOL CALLBACK EPGScheduleSettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch(uMsg)
	{
	case WM_INITDIALOG:
		{
			int nScheduleIndex;

			nScheduleIndex = IsRecordingScheduled(v->epg.nSelectedEPGChannel, v->epg.pSelectedEvent);
			if (nScheduleIndex != -1)
			{
				SetDlgItemInt(hDlg, IDC_EPG_PREROLL, pepgschedule[nScheduleIndex].wPreRoll, FALSE);
				SetDlgItemInt(hDlg, IDC_EPG_POSTROLL, pepgschedule[nScheduleIndex].wPostRoll, FALSE);
			}
			SetFocus(GetDlgItem(hDlg, IDOK));
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case IDOK:
			{
				int nScheduleIndex;

				nScheduleIndex = IsRecordingScheduled(v->epg.nSelectedEPGChannel, v->epg.pSelectedEvent);
				if (nScheduleIndex != -1)
				{
					WORD wPreRoll = GetDlgItemInt(hDlg, IDC_EPG_PREROLL, NULL, FALSE);
					WORD wPostRoll = GetDlgItemInt(hDlg, IDC_EPG_POSTROLL, NULL, FALSE);

					if (wPreRoll != pepgschedule[nScheduleIndex].wPreRoll ||
						wPostRoll != pepgschedule[nScheduleIndex].wPostRoll)
					{
						DeletedSelectedScheduledRecording();
						ScheduleRecording(GetParent(hDlg), 0, wPreRoll, wPostRoll);
					}
				}
				EndDialog(hDlg, TRUE);
			}
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		}
		break;
	}

	return FALSE;
}

void LoadEPGMaps()
{
	HANDLE hInputFile;
	BOOL fFoundStartTag = FALSE;
	int nMapOffset = 0;
	char szINIFilename[MAX_PATH];

	SourceHelper_GetTSReaderEXEDirectory(v->hInstance, szINIFilename, sizeof(szINIFilename));
	lstrcat(szINIFilename, "\\epgmap.ini");
	hInputFile = CreateFile(szINIFilename, GENERIC_READ, 0, (LPSECURITY_ATTRIBUTES)NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, (HANDLE)NULL);
	if (hInputFile == INVALID_HANDLE_VALUE)
		return;

	do
	{
		char * szEqual;
		char szInputLine[256];

		if (SourceHelper_ReadLine(hInputFile, szInputLine, sizeof(szInputLine)) == 0)
			break;
		strupr(szInputLine);
		if (lstrcmp(szInputLine, "[EPGMAP]") == 0)
			fFoundStartTag = TRUE;

		if (fFoundStartTag)
		{
			szEqual = strstr(szInputLine, "=");
			if (szEqual != NULL)
			{
				*szEqual = '\0';
				v->epg.nMapDestinationProgram[nMapOffset] = atoi(szInputLine);
				v->epg.nMapSourceProgram[nMapOffset] = atoi(szEqual + 1);
				if (++nMapOffset == MAX_EPG_MAPS)
					break;
			}
		}
	} while (TRUE);
	CloseHandle(hInputFile);

}
#endif LITE

LRESULT FAR PASCAL EPGridWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	static BOOL fClosing;

	switch(msg)
	{
	case WM_CREATE:
		{
			HDC hDC;
			SYSTEMTIME stLocal;

			v->epg.fTimerRunning = FALSE;
			v->epg.screenevents = NULL;
			v->epg.pSelectedEvent = NULL;
			v->epg.nSelectedChannel = -1;
			v->epg.fHideChannelSelectMode = FALSE;
			SendMessage(hWnd, WM_SETICON, ICON_BIG, (LPARAM)v->hDialogIcon);
			if (v->nEPGTimeSource < EPG_TIME_SOURCE_STREAM || v->nEPGTimeSource > EPG_TIME_SOURCE_PC)
				v->nEPGTimeSource = EPG_TIME_SOURCE_STREAM;	// a set time is not kept between sessions
			CreateEPGTimeSourceMenu(hWnd);
			EPGGotoNow(hWnd);
			UpdateEPGTitle(hWnd);
			SetTimer(hWnd, EPG_TITLE_TIMER, 1000, NULL);

			// Count number of EIT channels for the vertical scroll
			v->epg.nVerticalScrollPos = 0;
			SetupVerticalScrollMax(hWnd);

			hDC = GetDC(hWnd);
			SetupGridFonts(hDC);
			ReleaseDC(hWnd, hDC);

			// Brushes etc
			v->epg.hCellEntryBackground = CreateSolidBrush(v->dwEPGEventColor);
			v->epg.hCellEntrySelectedBackground = CreateSolidBrush(v->dwEPGSelectedColor);
			v->epg.hChannelEntryBackground = CreateSolidBrush(v->dwEPGChannelColor);
			v->epg.hCellEntryRecordBackground = CreateSolidBrush(RGB(224, 64, 0));
			v->epg.hCellEntryPrePostRollPen = CreatePen(PS_SOLID, 2, RGB(224, 64, 0));
			v->epg.hCellEntryHideModeBackground = CreateSolidBrush(RGB(128, 128, 128));
			v->epg.hTimeGridPen = CreatePen(PS_SOLID, 1, v->dwEPGTimeGridColor);

			// Icons
			v->epg.hTV_14 = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_14));
			v->epg.hTV_G = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_G));
			v->epg.hTV_MA = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_MA));
			v->epg.hTV_PG = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_PG));
			v->epg.hTV_Y = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_Y));
			v->epg.hTV_Y7 = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_Y7));
			v->epg.hTV_Y7FV = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_TV_Y7FV));
			v->epg.hCC = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_CC));
			v->epg.hEPGChannelHidden = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_DELETE));

			if (v->nSelectedProgram != -1)
				v->epg.nGotoChannel = v->pat.pmt[v->nSelectedProgram].nProgramNumber;
			else
				v->epg.nGotoChannel = 1;
			v->epg.szSearchString[0] = '\0';
			v->epg.nSearchChannel = 0;
			v->epg.nSearchProgramIndex = 0;

			fClosing = FALSE;

#ifndef LITE
			nEPGScheduleMax = 1000;
			nEPGScheduleItems = 0;
			pepgschedule = LocalAlloc(LPTR, sizeof(EPGSCHEDULE) * nEPGScheduleMax);
			SchedulerStartup(hWnd);

			if (v->fSkyEPG)
				UpdateSkyEPGMap(v->nCurrentBATID);

			LoadEPGMaps();
#endif LITE
			v->fEPGDisplayActive = TRUE;
			fShownScreenEventsWarning = FALSE;
		}
		break;
	case WM_DESTROY:
		KillTimer(hWnd, EPG_TITLE_TIMER);
		v->fEPGDisplayActive = FALSE;
		Sleep(100);
		{
			MSG msg;

			while (PeekMessage(&msg, hWnd, WM_USER + 2, WM_USER + 2, PM_REMOVE))
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}

		DeleteObject(v->epg.hGridTextFont);
		DeleteObject(v->epg.hGridTextFontBold);
		DeleteObject(v->epg.hGridSmallTextFont);		
		DeleteObject(v->epg.hTitleEventFontBold);
		DeleteObject(v->epg.hEventSmallTextFont);

		DeleteObject(v->epg.hCellEntryBackground);
		DeleteObject(v->epg.hCellEntrySelectedBackground);
		DeleteObject(v->epg.hChannelEntryBackground);
		DeleteObject(v->epg.hCellEntryRecordBackground);
		DeleteObject(v->epg.hCellEntryPrePostRollPen);
		DeleteObject(v->epg.hCellEntryHideModeBackground);
		DeleteObject(v->epg.hTimeGridPen);
		
		DestroyIcon(v->epg.hTV_14); 
		DestroyIcon(v->epg.hTV_G); 
		DestroyIcon(v->epg.hTV_MA); 
		DestroyIcon(v->epg.hTV_PG); 
		DestroyIcon(v->epg.hTV_Y); 
		DestroyIcon(v->epg.hTV_Y7); 
		DestroyIcon(v->epg.hTV_Y7FV); 
		DestroyIcon(v->epg.hCC); 
		DestroyIcon(v->epg.hEPGChannelHidden);

		if (v->epg.screenevents != NULL)
			LocalFree(v->epg.screenevents);
		if (!v->fEPGMinimizedFlag && !v->fEPGMaximizedFlag)
		{
			RECT rcEPGWindow;

			GetWindowRect(hWnd, &rcEPGWindow);
			v->nEPGWindowX = rcEPGWindow.left;
			v->nEPGWindowY = rcEPGWindow.top;
			v->nEPGWindowW = rcEPGWindow.right - rcEPGWindow.left;
			v->nEPGWindowH = rcEPGWindow.bottom - rcEPGWindow.top;
		}

#ifndef LITE
		if (v->epg.hSchedulerDLL != NULL)
			FreeLibrary(v->epg.hSchedulerDLL);
		LocalFree(pepgschedule);
#endif LITE
		SetForegroundWindow(v->hWndMainWindow);
		PostMessage(v->hWndMainWindow, WM_USER + 9, 1, 0);
		break;
#ifndef LITE
	case WM_COMMAND:
		switch(LOWORD(wParam))
		{
		case ID_IDREPGPOPUP_CANCELRECORDING:
			CursorWait(hWnd);
			DeletedSelectedScheduledRecording();
			CursorNormal();
			InvalidateRect(hWnd, NULL, FALSE);
			break;
		case ID_IDREPGPOPUP_RECORDOPTIONS:
			if (DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_SCHEDULE_SETTINGS), hWnd, EPGScheduleSettingsDlgProc) == TRUE)
				InvalidateRect(hWnd, NULL, FALSE);
			break;
		case ID_EPG_TIME_STREAM:
			SetEPGTimeSource(hWnd, EPG_TIME_SOURCE_STREAM);
			break;
		case ID_EPG_TIME_PC:
			SetEPGTimeSource(hWnd, EPG_TIME_SOURCE_PC);
			break;
		case ID_EPG_TIME_USER:
			SetEPGTimeSource(hWnd, EPG_TIME_SOURCE_USER);
			break;
		case ID_EPG_TIME_GOTO_NOW:
			EPGGotoNow(hWnd);
			break;
		}
		break;
#endif LITE
	case WM_TIMER:
		if (wParam == EPG_TITLE_TIMER)
		{
			UpdateEPGTitle(hWnd);
			// First TDT/TOT has arrived since the grid opened: move to the stream's "now"
			if (fEPGWaitingForStreamTime && v->nEPGTimeSource == EPG_TIME_SOURCE_STREAM)
			{
				DWORD64 lnNow;
				if (GetEPGNow(&lnNow))
					EPGGotoNow(hWnd);
			}
			break;
		}
		if (GetMaxEITChannels())
			EPGPaint(hWnd, wParam, lParam);
		break;
	case WM_PAINT:
		EPGPaint(hWnd, wParam, lParam);
		break;
	case WM_SIZE:
		if (!fClosing)
		{
			v->fEPGMinimizedFlag = wParam == SIZE_MINIMIZED;
			v->fEPGMaximizedFlag = wParam == SIZE_MAXIMIZED;		
			InvalidateRect(hWnd, NULL, FALSE);
		}
		break;
	case WM_CLOSE:
		fClosing = TRUE;
		DestroyWindow(hWnd);
		break;
	case WM_VSCROLL:
		switch(LOWORD(wParam))
		{
		case SB_LINEUP:
			if (v->epg.nVerticalScrollPos > 0)
				v->epg.nVerticalScrollPos--;
			UpdateVerticalScroll(hWnd);
			break;
		case SB_LINEDOWN:
			if (v->epg.nVerticalScrollPos < v->epg.nMaxEITChannels)
				v->epg.nVerticalScrollPos++;
			UpdateVerticalScroll(hWnd);
			break;
		case SB_PAGEDOWN:
			v->epg.nVerticalScrollPos += v->epg.nEITChannelsDisplayed;
			if (v->epg.nVerticalScrollPos >= v->epg.nMaxEITChannels)
				v->epg.nVerticalScrollPos = v->epg.nMaxEITChannels;
			UpdateVerticalScroll(hWnd);
			break;
		case SB_PAGEUP:
			v->epg.nVerticalScrollPos -= v->epg.nEITChannelsDisplayed;
			if (v->epg.nVerticalScrollPos < 0)
				v->epg.nVerticalScrollPos = 0;
			UpdateVerticalScroll(hWnd);
			break;
		case SB_THUMBTRACK:
			v->epg.nVerticalScrollPos = HIWORD(wParam);
			UpdateVerticalScroll(hWnd);
			break;
		}
		break;
	case WM_HSCROLL:
		switch(LOWORD(wParam))
		{
		case SB_LINELEFT:
			if (v->epg.nHorizontalScrollPos > 0)
				v->epg.nHorizontalScrollPos--;
			UpdateHorizontalScroll(hWnd);
			break;
		case SB_LINERIGHT:
			if (v->epg.nHorizontalScrollPos < 65000)
				v->epg.nHorizontalScrollPos++;
			UpdateHorizontalScroll(hWnd);
			break;
		case SB_PAGELEFT:
			v->epg.nHorizontalScrollPos -= v->epg.nTimeRangeShown;
			if (v->epg.nHorizontalScrollPos < 0)
				v->epg.nHorizontalScrollPos = 0;
			UpdateHorizontalScroll(hWnd);
			break;
		case SB_PAGERIGHT:
			v->epg.nHorizontalScrollPos += v->epg.nTimeRangeShown;
			if (v->epg.nHorizontalScrollPos > 65000)
				v->epg.nHorizontalScrollPos = 65000;
			UpdateHorizontalScroll(hWnd);
			break;
		case SB_THUMBTRACK:
			v->epg.nHorizontalScrollPos = HIWORD(wParam);
			UpdateHorizontalScroll(hWnd);
			break;
		}
		break;
	case WM_LBUTTONDOWN:
		{
			int xPos = LOWORD(lParam); 
			int yPos = HIWORD(lParam);
			int nScreenEventOffset = 0;

			v->epg.pSelectedEvent = NULL;
			v->epg.nSelectedChannel = -1;
			while (nScreenEventOffset < MAX_SCREEN_EVENTS && v->epg.screenevents[nScreenEventOffset].rc.top != 0)
			{
				if (    (xPos >= v->epg.screenevents[nScreenEventOffset].rc.left && xPos <= v->epg.screenevents[nScreenEventOffset].rc.right)
					 && (yPos >= v->epg.screenevents[nScreenEventOffset].rc.top && yPos <= v->epg.screenevents[nScreenEventOffset].rc.bottom) )
				{
					if (v->epg.screenevents[nScreenEventOffset].pEITEvent == NULL)
					{
						v->epg.nSelectedChannel = v->epg.screenevents[nScreenEventOffset].nChannel;
#ifndef LITE
						if (v->epg.fHideChannelSelectMode)
						{
							int nByteOffset;
							int nBit;

							nByteOffset = v->epg.nSelectedChannel / 8;
							nBit = v->epg.nSelectedChannel % 8;
							if (v->epg.bHiddenChannels[nByteOffset] & (1 << nBit))
								v->epg.bHiddenChannels[nByteOffset] &=  ~(1 << nBit);
							else
								v->epg.bHiddenChannels[nByteOffset] |= (1 << nBit);
						}
#endif LITE
					}
					else
					{
						v->epg.pSelectedEvent = v->epg.screenevents[nScreenEventOffset].pEITEvent;
						v->epg.nSelectedEPGChannel = v->epg.screenevents[nScreenEventOffset].nChannel;
					}
					break;
				}
				nScreenEventOffset++;
			}
			InvalidateRect(hWnd, NULL, FALSE);
		}
		break;
	case WM_LBUTTONDBLCLK:
#ifndef LITE
		if (v->nNetworkPID != 0x0010 && v->nNetworkPID != 0x1ffb)
		{
			MessageBox(hWnd, "Currently tuning and scheduling from the EPG\nis only supported on DVB and ATSC networks", gszAppName, MB_ICONWARNING);
			break;
		}
		if (v->epg.nSelectedChannel != -1)
		{
			if (v->pChannelData[v->epg.nSelectedChannel] != NULL)
			{
				if (v->nAutoRecord == AUTO_RECORD_VLC)	// already playing?
				{
					SendMessage(v->hWndMainWindow, WM_COMMAND, ID_PLAYBACK_VLC_1 + (v->nAutoVLCConfiguration - 1), 0);
				}
				v->nAutoRecordProgram = v->epg.nSelectedChannel;
				v->nAutoVLCConfiguration = 1;
				v->nAutoRecord = AUTO_RECORD_VLC;				
				v->nSDTRightClickIndex = v->epg.nSelectedChannel;
				PostMessage(v->hDlgSIParser, WM_COMMAND, ID_IDRNITPOPUP_RETUNETOTHISMUX_SDT, 0);
			}
		}
		else if (v->epg.pSelectedEvent != NULL)
		{
			ScheduleRecording(hWnd, lParam, v->nSchedulerDefaultPreRoll, v->nSchedulerDefaultPostRoll);
		}
#else LITE
		MessageBox(hWnd, "Double-click functions such as retuning and\nscheduling are not supported in this version\nof TSReader.", gszAppName, MB_ICONSTOP);
#endif LITE
		break;
	case WM_KEYDOWN:
		switch(wParam)
		{
		case VK_ESCAPE:
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			break;
		case VK_HOME:
			v->epg.nVerticalScrollPos = 0;
			UpdateVerticalScroll(hWnd);
			break;
		case VK_END:
			v->epg.nVerticalScrollPos = v->epg.nMaxEITChannels - v->epg.nEITChannelsDisplayed + 2;
			UpdateVerticalScroll(hWnd);
			break;
		case VK_UP:
			if (GetKeyState(VK_SHIFT) & 0x8000)
			{
#ifndef LITE
				v->nEPGChannelHeight--;
				if (v->nEPGChannelHeight < 2)
					v->nEPGChannelHeight = 2;
				InvalidateRect(hWnd, NULL, FALSE);
#else LITE
				MessageBox(hWnd, szLiteWarning, gszAppName, MB_ICONSTOP);
#endif LITE
			}
			else
			{
				if (v->epg.nVerticalScrollPos > 0)
					v->epg.nVerticalScrollPos--;
				UpdateVerticalScroll(hWnd);
			}
			break;
		case VK_DOWN:
			if (GetKeyState(VK_SHIFT) & 0x8000)
			{
#ifndef LITE
				v->nEPGChannelHeight++;
				if (v->nEPGChannelHeight > 10)
					v->nEPGChannelHeight = 10;
				InvalidateRect(hWnd, NULL, FALSE);
#else LITE
				MessageBox(hWnd, szLiteWarning, gszAppName, MB_ICONSTOP);
#endif LITE
			}
			else
			{
				if (v->epg.nVerticalScrollPos < v->epg.nMaxEITChannels)
					v->epg.nVerticalScrollPos++;
				UpdateVerticalScroll(hWnd);
			}
			break;
		case VK_LEFT:
			if (GetKeyState(VK_SHIFT) & 0x8000)
			{
#ifndef LITE
				// Make time interval smaller
				v->nEPGHalfHourWidth--;
				if (v->nEPGHalfHourWidth < 1)
					v->nEPGHalfHourWidth = 1;
				InvalidateRect(hWnd, NULL, FALSE);
#else LITE
				MessageBox(hWnd, szLiteWarning, gszAppName, MB_ICONSTOP);
#endif LITE
			}
			else
			{
				// Scroll time
				if (v->epg.nHorizontalScrollPos > 0)
					v->epg.nHorizontalScrollPos--;
				UpdateHorizontalScroll(hWnd);
			}
			break;
		case VK_RIGHT:
			if (GetKeyState(VK_SHIFT) & 0x8000)
			{
#ifndef LITE
				// Make time interval bigger
				v->nEPGHalfHourWidth++;
				if (v->nEPGHalfHourWidth > 24)
					v->nEPGHalfHourWidth = 24;
				InvalidateRect(hWnd, NULL, FALSE);
#else LITE
				MessageBox(hWnd, szLiteWarning, gszAppName, MB_ICONSTOP);
#endif LITE
			}
			else
			{
				// Scroll time
				if (v->epg.nHorizontalScrollPos < 65000)
					v->epg.nHorizontalScrollPos++;
				UpdateHorizontalScroll(hWnd);
			}
			break;
		case VK_NEXT:
		case ' ':
			v->epg.nVerticalScrollPos += v->epg.nEITChannelsDisplayed - 1;
			if (v->epg.nVerticalScrollPos >= v->epg.nMaxEITChannels)
				v->epg.nVerticalScrollPos = v->epg.nMaxEITChannels;
			UpdateVerticalScroll(hWnd);
			break;
		case VK_PRIOR:
			v->epg.nVerticalScrollPos -= v->epg.nEITChannelsDisplayed;
			if (v->epg.nVerticalScrollPos < 0)
				v->epg.nVerticalScrollPos = 0;
			UpdateVerticalScroll(hWnd);
			break;
		case 'n':
		case 'N':
			EPGGotoNow(hWnd);
			break;
		case 't':
		case 'T':
			SetEPGTimeSource(hWnd, EPG_TIME_SOURCE_USER);
			break;
		case VK_F1:
			MessageBox(hWnd, "Keys available in the EPG Grid:\n"
				             "\n"
							 "Left\t\tMove back in time\n"
							 "Right\t\tMove forward in time\n"
							 "Up\t\tMove channel list up\n"
							 "Down\t\tMove channel list down\n"
							 "N\t\tGo to \"now\" (see the Time Source menu)\n"
							 "T\t\tSet the EPG date and time\n"
#ifndef LITE
							 "Shift+Left\tShrink time width\n"
							 "Shift+Right\tExpand time width\n"
							 "Shift+Up\t\tExpand channel size\n"
							 "Shift+Down\tShrink channel size\n"
							 "Ctrl+F\t\tFind a program title\n"
							 "Ctrl+G\t\tGoto a channel\n"
							 "F3\t\tFind next program\n"
							 "H\t\tToggle channel hide mode\n"
							 "Ctrl+D\t\tGo to a particular date\n"
							 "S\t\tShow scheduled recordings\n"
							 "M\t\tSchedule a manual recording\n"
							 "\n"
							 "Double click channel to tune there\n"
							 "Double click event to schedule recording\n"
#endif LITE
							 "\n",
							 gszAppName, MB_ICONINFORMATION);
			break;
#ifndef LITE
		case VK_F3:		// Find next
			if (v->epg.fHideChannelSelectMode)
			{
				MessageBeep(0);
				break;
			}
			if (GetKeyState(VK_SHIFT) & ~1)
			{
				// todo -- search backwards
			}
			else
				SearchNextEPGEntry(hWnd);
			break;
		case 'c':
		case 'C':
			if (v->epg.fHideChannelSelectMode)
			{
				memset(v->epg.bHiddenChannels, 0, sizeof(v->epg.bHiddenChannels));
				InvalidateRect(hWnd, NULL, FALSE);
			}
			break;
		case 'd':
		case 'D':
			if (GetKeyState(VK_CONTROL) & ~1)
			{
				if (v->epg.fHideChannelSelectMode)
				{
					MessageBeep(0);
					break;
				}
				if (DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_DATE), hWnd, EPGGotoDateDlgProc) == TRUE)
					InvalidateRect(hWnd, NULL, FALSE);
			}
			break;
		case 'f':
		case 'F':		// Ctrl+F
			if (GetKeyState(VK_CONTROL) & ~1)
			{
				if (v->epg.fHideChannelSelectMode)
				{
					MessageBeep(0);
					break;
				}
				DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_FIND_STRING), hWnd, EPGFindStringDlgProc);
			}
			break;
		case 'G':		// Ctrl+G
			if (GetKeyState(VK_CONTROL) & ~1)
			{
				if (v->epg.fHideChannelSelectMode)
				{
					MessageBeep(0);
					break;
				}
				DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_GOTO_CHANNEL), hWnd, EPGGotoChannelDlgProc);
			}
			break;
		case 'h':
		case 'H':
			v->epg.fHideChannelSelectMode = (~v->epg.fHideChannelSelectMode) & 1;
			InvalidateRect(hWnd, NULL, FALSE);
			break;
		case 's':
		case 'S':
			if (v->epg.fHideChannelSelectMode)
			{
				MessageBeep(0);
				break;
			}
			DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_SHOW_SCHEDULE), hWnd, EPGShowScheduleDlgProc);
			break;
		case 'm':
		case 'M':
			if (CheckEPGRecordSettings(hWnd) == FALSE)
				break;
			DialogBox(v->hInstance, MAKEINTRESOURCE(IDD_EPG_MANUAL), hWnd, EPGManualScheduleDlgProc);
			break;
#endif LITE
		}
		//return DefWindowProc(hWnd, msg, wParam, lParam);
		break;
	case WM_MOUSEWHEEL:
		{
			short zDelta = (short) HIWORD(wParam);
			int nLoops;

			nLoops = zDelta / 120;
			if (nLoops < 0)
				nLoops = -(nLoops);
			switch(nLoops)
			{
			case 1:
				if (zDelta > 0)
					SendMessage(hWnd, WM_VSCROLL, SB_LINEUP, 0);
				else
					SendMessage(hWnd, WM_VSCROLL, SB_LINEDOWN, 0);
				break;
			case 2:
				if (zDelta < 0)
				{
					v->epg.nVerticalScrollPos += (v->epg.nEITChannelsDisplayed / 2) - 1;
					if (v->epg.nVerticalScrollPos >= v->epg.nMaxEITChannels)
						v->epg.nVerticalScrollPos = v->epg.nMaxEITChannels;
				}
				else
				{
					v->epg.nVerticalScrollPos -= (v->epg.nEITChannelsDisplayed / 2) - 1;
					if (v->epg.nVerticalScrollPos < 0)
						v->epg.nVerticalScrollPos = 0;
				}
				UpdateVerticalScroll(hWnd);
				break;
			default:
				if (zDelta > 0)
					SendMessage(hWnd, WM_VSCROLL, SB_PAGEUP, 0);
				else
					SendMessage(hWnd, WM_VSCROLL, SB_PAGEDOWN, 0);
				break;
			}
		}
		break;
//	case WM_ACTIVATE:
//		InvalidateThumbnails();
//		break;
	case WM_USER + 2:
		{
			switch(wParam & 0xf0000000)
			{
			case SI_PARSER_SDT:
			case SI_PARSER_VCT:
				{
					int nChannelNumber = (int)lParam;
					if (nChannelNumber > GetEITOffset(v->epg.nVerticalScrollPos))
					{
						if (nChannelNumber < v->nMaxEPGDisplayChannel)
							InvalidateRect(hWnd, NULL, FALSE);
					}
				}
				break;
			case SI_PARSER_EIT:
				{
					int nChannelNumber = (int)wParam & 0x0fffffff;
					PEITEVENT pEvent = (PEITEVENT)lParam;
					
					if (v->epg.fDisplayEPGByLCN == TRUE)
					{
						if (v->pChannelData[nChannelNumber] != NULL)						
							nChannelNumber = v->pChannelData[nChannelNumber]->nLogicalChannelNumber;
						if (nChannelNumber == 0)
							break;
					}

					if (nChannelNumber > GetEITOffset(v->epg.nVerticalScrollPos))
					{
						if (nChannelNumber <= v->nMaxEPGDisplayChannel)
						{
							int nDuration;
							__int64 nEventEnd;
							__int64 nEventStart;

							SystemTimeToFileTime(&pEvent->stStartTime, (FILETIME *)&nEventStart);
							nDuration =  pEvent->stRunTime.wHour * 60 * 60
									   + pEvent->stRunTime.wMinute * 60
									   + pEvent->stRunTime.wSecond;
							nEventEnd = nEventStart + ((__int64)nDuration * (__int64)10000000);
							if (nEventStart >= v->epg.nStartDisplayTime && nEventEnd < v->nLastEPGDisplayTime)
								InvalidateRect(hWnd, NULL, FALSE);
						}
					}
				}
				break;
			}
		}
		break;
	default:
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}

	return FALSE;
}

