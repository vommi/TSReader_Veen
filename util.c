#include <windows.h>
#include <commctrl.h>
#include <time.h>
#include <winsock.h>
#include <shlobj.h>
#include "tsreader.h"
#include "bcdmux.h"
#include "util.h"
#include "formatter.h"

#include "resource.h"

void GetSourceInfoLine(int nLine, char * szOutput);

// Stuff in ATSC_huffman.c
void ATSCHuffmanDecode(int nBitBufferIndex, int type, int bytes, char * outtext);

extern PVARIABLES v;
extern BOOL (* GetTunerString) (char * szString);
extern BOOL (* GetSignalString) (char * szString);

char * GetExtensionPtr(char * szInputString)
{
	int nPos;
	for (nPos = lstrlen(szInputString); nPos > 0; nPos--)
	{
		if (szInputString[nPos] == '.')
			return &szInputString[nPos];
	}
	return NULL;
}

void CopyListControlToClipboard(HWND hListControl, BOOL fAddCR)
{
	if (!OpenClipboard(v->hDlgSIParser))
	{
		MessageBeep(0);
		return;
	}
	{
		char * szBuffer = GlobalAlloc(GMEM_DDESHARE, 1024 * 1024);
		int nItemCount = SendMessage(hListControl, LB_GETCOUNT, 0, 0);
		int i;

		EmptyClipboard(); 
		for (i = 0; i < nItemCount; i++)
		{
			char szTemp[256];
			SendMessage(hListControl, LB_GETTEXT, (WPARAM)i, (LPARAM)szTemp);
			lstrcat(szBuffer, szTemp);
			if (fAddCR)
				lstrcat(szBuffer, "\r\n");
		}
		GlobalUnlock(szBuffer);  
		SetClipboardData(CF_TEXT, szBuffer);
		CloseClipboard();
		GlobalFree(szBuffer);
	}
}

void UpdateMainStatusText(char * szText)
{
	if (lstrlen(szText) == 0)
	{
		int a=1;
	}
	EnterCriticalSection(&v->csStatusbar);
	lstrcpy(v->szStatusTextMain, szText);
	v->fStatusDirty = TRUE;
	LeaveCriticalSection(&v->csStatusbar);
}

void UpdateSecondaryStatusText(char * szText)
{
	EnterCriticalSection(&v->csStatusbar);
	lstrcpy(v->szStatusTextSecondary, szText);
	v->fStatusDirty = TRUE;
	LeaveCriticalSection(&v->csStatusbar);
}

BOOL ATSCPIDs()
{
	if (v->nStreamTo == 0 && v->fATSCRecordMode == TRUE)
		return TRUE;
	if (v->nStreamTo == STREAM_TO_DVHS && v->fDVHSForceATSC)
		return TRUE;

	return FALSE;
}

__int64 DecodeMPEG2PCR(BYTE * bAB)
{
	unsigned __int64 nPCR_base = ((__int64)bAB[2] << 32 | (__int64)bAB[3] << 24 | (__int64)bAB[4] << 16 | (__int64)bAB[5] << 8 | (__int64)bAB[6]);
	unsigned __int64 nPCR_ext = (__int64) ((bAB[6] << 8 | bAB[7]) & 0x1ff);

	return ((nPCR_base >> 7) * (__int64)300) + nPCR_ext;
}

int GetTotalPMTChannels()
{
	int nRetVal = 0;
	int nPMTIndex;

	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		nRetVal++;
	}

	return nRetVal;
}

BOOL IsDuplicateDescriptor(BYTE * pDescriptor1, BYTE * pDescriptor2)
{
	if (pDescriptor1[0] != pDescriptor2[0])
		return FALSE;		// not the same descriptor tag
	if (pDescriptor1[1] != pDescriptor2[1])
		return FALSE;		// not the same descriptor length
	if (memcmp(&pDescriptor1[2], &pDescriptor2[2], pDescriptor1[1]) != 0)
		return FALSE;		// not the same data

	return TRUE;		
}

BOOL FillAddr(PSOCKADDR_IN psin, TCHAR * szHostName, unsigned short usPort)
{
	int nConvertCount;
	int nTemp;
	unsigned long dwDotted;
	HOSTENT	phe;
	LPHOSTENT pHostent;
	
	pHostent = &phe;
	psin->sin_family = AF_INET;

	// See if this is a dotted notation
	nConvertCount = sscanf(szHostName, TEXT("%d.%d.%d.%d"),
						   &nTemp,
						   &nTemp,
						   &nTemp,
						   &nTemp);
	if (nConvertCount == 4)
	{
		// Is an IP address, use without name lookup
		dwDotted = inet_addr(szHostName);
		memcpy((char FAR *)&(psin->sin_addr), (char FAR *)&dwDotted, 4);
	}
	else
	{
		// Not an IP address, do name lookup
		pHostent = gethostbyname(szHostName);
		if (pHostent == NULL)
		{
			//"Couldn't resolve Host Name"
			return FALSE;
		}
		memcpy((char FAR *)&(psin->sin_addr), pHostent->h_addr, pHostent->h_length);
	}
	
	//Setup port to connect to
	psin->sin_port = htons(usPort);
	return TRUE;
}

BOOL IsMPEGAudioStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].nStreamType == 0x03)
		return TRUE;
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].nStreamType == 0x04)
		return TRUE;
	return FALSE;
}

BOOL IsAC3AudioStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x6a)	// AC3 tag?
				return TRUE;

			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x05)	// registration descriptor?
			{
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "AC-3", 4) == 0)
					return TRUE;
			}
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

BOOL IsPCMAudioStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x05)	// registration descriptor?
			{
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "BSSD", 4) == 0)
					return TRUE;
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "PCM", 3) == 0)
					return TRUE;
			}
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

BOOL IsDTSAudioStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x05)	// registration descriptor?
			{
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS1", 4) == 0)
					return TRUE;
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS2", 4) == 0)
					return TRUE;
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS3", 4) == 0)
					return TRUE;
			}
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

int GetDTSAudioDescriptor(int nPMTProgramIndex, int nESIndex, BYTE * pBuffer)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x73)	// DTS Audio descriptor?
			{
				if (pBuffer != NULL)
					memcpy(pBuffer,
					       &v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex],
						   v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1] + 2);
				return v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1] + 2;
			}
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return 0;
}

int GetDTSFrameSize(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x05)	// registration descriptor?
			{
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS1", 4) == 0)
					return 512;
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS2", 4) == 0)
					return 1024;
				if (memcmp(&v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2], "DTS3", 4) == 0)
					return 2048;
			}
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return 0;
}

BOOL IsDataBroadcastStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x66)	// data broadcast ID descriptor?
				return TRUE;
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

BOOL IsTeleTextOrVBIStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x45)	// VBI data descriptor?
				return TRUE;
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x46)	// VBI Teletext descriptor?
				return TRUE;
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x56)	// Teletext descriptor?
				return TRUE;
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

BOOL IsSubtitleStream(int nPMTProgramIndex, int nESIndex)
{
	if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors != NULL)
	{
		int nDescriptorsLength = v->pat.pmt[nPMTProgramIndex].es[nESIndex].nDescriptorsLength;
		int nCurrentIndex = 0;

		do
		{
			if (v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x59)	// subtitling descriptor?
				return TRUE;
			nCurrentIndex += (BYTE)v->pat.pmt[nPMTProgramIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
			nCurrentIndex += 2;	// descriptor tag and length
		} while (nCurrentIndex < nDescriptorsLength);
	}
	return FALSE;
}

void GetATSCMultipleString(int nBitBufferIndex, char * szOutputString, int nLength)
{
	int k, l, m;
	int number_of_strings;

	if (nLength == 0)
		return;

	number_of_strings = get_bits(nBitBufferIndex, 8);
#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader: GetATSCMultipleString+\n");
#endif DEBUG_MESSAGES

	if (nLength != -1)
	{
		if (nLength < 2)
		{
			// Actually a null string
			return;
		}
	}

	for (k = 0; k < number_of_strings; k++)
	{
		int ISO_639_language_code = get_bits(nBitBufferIndex, 24);
		int number_of_segments = get_bits(nBitBufferIndex, 8);

		for (l = 0; l < number_of_segments; l++)
		{
			BYTE compressed_string_byte[1024];

			int compression_type = get_bits(nBitBufferIndex, 8);
			int mode = get_bits(nBitBufferIndex, 8);
			int number_bytes = get_bits(nBitBufferIndex, 8);

			switch(compression_type)
			{
			case 0:		// not compressed
				for (m = 0; m < number_bytes; m++)
					compressed_string_byte[m] = get_bits(nBitBufferIndex, 8);
				compressed_string_byte[m] = '\0';
				lstrcat(szOutputString, compressed_string_byte);
				break;
			case 1:		// Huffman
			case 2:		// Huffman
				compressed_string_byte[0] = '\0';
				ATSCHuffmanDecode(nBitBufferIndex, compression_type, number_bytes, compressed_string_byte);
				lstrcat(szOutputString, compressed_string_byte);
				break;
			}
		}
	}
#ifdef DEBUG_MESSAGES
	{
		char szTemp[32 * 1024];
		wsprintf(szTemp, "GetATSCMultipleString len = %d string: %s\n", lstrlen(szOutputString), szOutputString);
		OutputDebugString(szTemp);
	}
	OutputDebugString("TSReader: GetATSCMultipleString-\n");
#endif DEBUG_MESSAGES
}

void GetExtendedChannelName(BYTE * pSectionPointer, char * szLongName)
{
	BYTE * pDescriptorData = pSectionPointer;
	int number_strings = pDescriptorData[2];
	BYTE * pDescriptor = &pDescriptorData[3];
	int l;
	char szExtendedName[256] = {0};

	for (l = 0; l < number_strings; l++)
	{
		int number_segments;
		int m;

		pDescriptor += 3; // skip the language_code
		number_segments = *pDescriptor++;
		for (m = 0; m < number_segments; m++)
		{
			int compression_type = *pDescriptor++;
			int mode = *pDescriptor++;
			int number_bytes = *pDescriptor++;
			if (compression_type == 0) // uncompressed
			{
				int n;

				for (n = 0; n < number_bytes; n++)
					szExtendedName[n] = *pDescriptor++;
				szExtendedName[n] = 0;
			}
			else
			{
				lstrcpy(szExtendedName, "[Unsupported]");
				pDescriptor += number_bytes;
			}
		}
	}
	if (lstrlen(szExtendedName))
		lstrcpy(szLongName, szExtendedName);
}

void CursorNormal()
{
	ReleaseCapture();
	SetCursor(LoadCursor(NULL, IDC_ARROW));
}

void CursorWait(HWND hWnd)
{
	SetCapture(hWnd);
	SetCursor(LoadCursor(NULL, IDC_WAIT));
}

void ConvertDVBBCDTimeOffsets(DWORD dwInput, int * nHour, int * nMinute)
{
	int nLocalHour, nLocalMinute;
	char szTemp[16];

	wsprintf(szTemp, "%02x", dwInput >> 8);
	sscanf(szTemp, "%d", &nLocalHour);
	wsprintf(szTemp, "%02x", dwInput & 0xff);
	sscanf(szTemp, "%d", &nLocalMinute);

	*nHour = nLocalHour;
	*nMinute = nLocalMinute;
}

DWORD ConvertBCD(DWORD nInput)
{
	char szTemp[16];
	int nRetVal;

	wsprintf(szTemp, "%08x", nInput);
	sscanf(szTemp, "%d", &nRetVal);
	return nRetVal;
}

void ConvertDVBDate(int nMJD, int * nYear, int * nMonth, int * nDay)
{
	float dYear, dMonth, dDay, dMJD, dTemp, dTemp2;
	int nCarry;

	dMJD = (float)nMJD;
	dYear = (dMJD - 15078.2f) / 365.25f;
	dYear = (float)((int)dYear);
	
	dTemp = dYear * 365.25f;
	dTemp = (float)((int)dTemp);
	dMonth = ((dMJD - 14956.1f) - dTemp) / 30.6001f;
	dMonth = (float)((int)dMonth);
	
	dTemp = dYear * 365.25f;
	dTemp = (float)((int)dTemp);
	dTemp2 = dMonth * 30.6001f;
	dTemp2 = (float)((int)dTemp2);
	dDay = dMJD - 14956.0f - dTemp - dTemp2;

	*nYear = (int)dYear;
	*nMonth = (int)dMonth;
	*nDay = (int)dDay;
	if ( (*nMonth == 14) || (*nMonth == 15) )
		nCarry = 1;
	else
		nCarry = 0;
	*nYear += nCarry;
	*nYear += 1900;
	*nMonth = *nMonth - 1 - (nCarry * 12);
}

void ConvertDVBTime(int nBCDTime, int * nHour, int * nMinute, int * nSecond)
{
	char szTemp[16];

	wsprintf(szTemp, "%06x", nBCDTime);
	sscanf(szTemp, "%02d%02d%02d", nHour, nMinute, nSecond);
}

//#define UNIXGPS 315964787
#define UNIXGPS 315964800
void ConvertATSCDateTime(DWORD dwGPSTime, SYSTEMTIME * st)
{
	struct tm *atsctime;
	DWORD dwUnixTime = dwGPSTime + UNIXGPS;
	memset(st, 0, sizeof(SYSTEMTIME));
	
	atsctime = gmtime((time_t *)&dwUnixTime);
	if (atsctime != NULL)
	{
		st->wYear = atsctime->tm_year + 1900;
		st->wMonth = atsctime->tm_mon + 1;
		st->wDay = atsctime->tm_mday;
		st->wHour = atsctime->tm_hour;
		st->wMinute = atsctime->tm_min;
		st->wSecond = atsctime->tm_sec;
	}
}

// Current time for EPG purposes as a UTC FILETIME, according to v->nEPGTimeSource.
// Returns FALSE when the stream is the time source but no TDT/TOT/STT has been seen
// yet; *plnNow is then the PC clock.
BOOL GetEPGNow(DWORD64 * plnNow)
{
	SYSTEMTIME stSystemTime;

	switch (v->nEPGTimeSource)
	{
	case EPG_TIME_SOURCE_USER:
		*plnNow = v->nEPGUserTime;
		return TRUE;
	case EPG_TIME_SOURCE_STREAM:
		{
			__int64 nStreamTime = InterlockedCompareExchange64(&v->nEPGStreamTime, 0, 0);
			if (nStreamTime != 0)
			{
				// Stream time moves on in real time between TDT/TOT updates
				*plnNow = nStreamTime + (GetTickCount64() - v->nEPGStreamTimeTick) * (DWORD64)10000;
				return TRUE;
			}
		}
		GetSystemTime(&stSystemTime);
		SystemTimeToFileTime(&stSystemTime, (FILETIME *)plnNow);
		return FALSE;
	default:
		GetSystemTime(&stSystemTime);
		SystemTimeToFileTime(&stSystemTime, (FILETIME *)plnNow);
		return TRUE;
	}
}

// Called by the parser for each TDT, TOT and STT (all UTC)
void SetEPGStreamTime(int nYear, int nMonth, int nDay, int nHour, int nMinute, int nSecond)
{
	SYSTEMTIME st;
	__int64 nTime;

	memset(&st, 0, sizeof(st));
	st.wYear = nYear;
	st.wMonth = nMonth;
	st.wDay = nDay;
	st.wHour = nHour;
	st.wMinute = nMinute;
	st.wSecond = nSecond;
	if (SystemTimeToFileTime(&st, (FILETIME *)&nTime))
	{
		v->nEPGStreamTimeTick = GetTickCount64();
		InterlockedExchange64(&v->nEPGStreamTime, nTime);
	}
}

void ResetEPGStreamTime()
{
	InterlockedExchange64(&v->nEPGStreamTime, 0);
}

BOOL EventInPast(PEITEVENT pEvent, BOOL fAllowPastEITData)
{
	FILETIME ftProgramEnd;
	DWORD64 lnProgramEnd, lnNow;
	DWORD64 lnMultiplier = 10000000;
	DWORD64 lnRunTime = ( (pEvent->stRunTime.wHour * 60 * 60)
					    + (pEvent->stRunTime.wMinute * 60)
						+ (pEvent->stRunTime.wSecond) ) * lnMultiplier;
	
	if (fAllowPastEITData)
		return FALSE;

	// Without a known "now" (stream time not seen yet) keep everything
	if (!GetEPGNow(&lnNow))
		return FALSE;

	SystemTimeToFileTime(&pEvent->stStartTime, &ftProgramEnd);
	memcpy(&lnProgramEnd, &ftProgramEnd, sizeof(DWORD64));
	lnProgramEnd = lnProgramEnd + lnRunTime;

	if (lnProgramEnd <= lnNow)
		return TRUE;

	return FALSE;
}

void LogDescriptor(int nDescriptorIndex, int nDescriptorTag)
{
#ifdef PRO
	v->bDescriptorTagArray[nDescriptorIndex][nDescriptorTag] = 1;
#endif PRO
}

void ExpireOldEIT_debug(PEITEVENT pCurrent, int nServiceID, int nType)
{
	/*
	char szTemp[256];
	wsprintf(szTemp, "TSReader: Expire EIT: type=%d prog=%d %02d:%02d:%02d %s\n",
		     nType, nServiceID,
		     pCurrent->stStartTime.wHour, pCurrent->stStartTime.wMinute, pCurrent->stStartTime.wSecond,
			 pCurrent->szEventName);
	OutputDebugString(szTemp);*/
}

void ExpireOldEITData(int nServiceID)
{
	if (!v->fKeepPastEITData)
	{
		PEITEVENT pCurrent, pPrior = NULL;
		DWORD64 lnNow;

		if (!GetEPGNow(&lnNow))
			return;

		pCurrent = v->pEvents[nServiceID];
		if (pCurrent != NULL)
		{
			do
			{
				DWORD64 lnProgramEnd;
				DWORD64 lnMultiplier = 10000000;
				DWORD64 lnRunTime = ( (pCurrent->stRunTime.wHour * 60 * 60)
									+ (pCurrent->stRunTime.wMinute * 60)
									+ (pCurrent->stRunTime.wSecond) ) * lnMultiplier;

				SystemTimeToFileTime(&pCurrent->stStartTime, (FILETIME *)&lnProgramEnd);
				lnProgramEnd = lnProgramEnd + lnRunTime;				
				if (lnNow > lnProgramEnd)
				{
					// pCurrent is in the past - remove it from the list
					PEITEVENT pNext = (PEITEVENT)pCurrent->dwNextEvent;

					if (pPrior == NULL && pNext == NULL)
					{
						// First & last event (hmmm)
						ExpireOldEIT_debug(pCurrent, nServiceID, 1);
						LocalFree(pCurrent);
						v->pEvents[nServiceID] = NULL;
						pCurrent = NULL;
					}
					else if (pPrior == NULL)
					{
						// First event - point to the next one
						ExpireOldEIT_debug(pCurrent, nServiceID, 2);
						LocalFree(pCurrent);
						v->pEvents[nServiceID] = pNext;
						pCurrent = pNext;
					}
					else if (pNext == NULL)
					{
						// Last event
						ExpireOldEIT_debug(pCurrent, nServiceID, 3);
						LocalFree(pCurrent);
						pPrior->dwNextEvent = (DWORD)NULL;
						pCurrent = NULL;
					}
					else
					{
						// Normal event with next and prior
						ExpireOldEIT_debug(pCurrent, nServiceID, 4);
						LocalFree(pCurrent);
						pPrior->dwNextEvent = (DWORD)pNext;
						pCurrent = pNext;
					}
				}
				else
				{
					pPrior = pCurrent;
					pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
				}
			} while (pCurrent != NULL);
		}
	}
}

#ifndef LITE
void SaveEPGData(PEITEVENT pEITItem, int nChannelNumber)
{
	if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
	{
		if (pEITItem->stStartTime.wDay != v->stEPGSaveCurrentDate.wDay
			|| pEITItem->stStartTime.wMonth != v->stEPGSaveCurrentDate.wMonth
			|| pEITItem->stStartTime.wYear != v->stEPGSaveCurrentDate.wYear)
		{
			// New date - close file -- code down the line will reopen
			CloseHandle(v->hEPGSaveHandle);
			v->hEPGSaveHandle = INVALID_HANDLE_VALUE;
		}
	}
	if (v->hEPGSaveHandle == INVALID_HANDLE_VALUE)
	{
		char szFilename[MAX_PATH];
		char szTemp[128];

		lstrcpy(szFilename, v->szEPGSaveFolder);
		if (szFilename[lstrlen(szFilename) - 1] != '\\')
			lstrcat(szFilename, "\\");
		wsprintf(szTemp, "EPG_%04d%02d%02d.txt",
			     pEITItem->stStartTime.wYear, pEITItem->stStartTime.wMonth, pEITItem->stStartTime.wDay);
		lstrcat(szFilename, szTemp);
		v->hEPGSaveHandle = CreateFile(szFilename, GENERIC_WRITE, FILE_SHARE_READ, (LPSECURITY_ATTRIBUTES) NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, (HANDLE) NULL);
		if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
		{
			SetFilePointer(v->hEPGSaveHandle, 0, NULL, FILE_END);
			v->stEPGSaveCurrentDate.wDay = pEITItem->stStartTime.wDay;
			v->stEPGSaveCurrentDate.wMonth = pEITItem->stStartTime.wMonth;
			v->stEPGSaveCurrentDate.wYear = pEITItem->stStartTime.wYear;
		}
	}
	if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
	{
		DWORD dwWritten;
		char szOutputLine[10 * 1024] = {0};
		char szTemp[5 * 1024];

		wsprintf(szTemp, "%08x\t", pEITItem->nEventID);
		lstrcat(szOutputLine, szTemp);

		//Date/time & duration
		wsprintf(szTemp, "%04d/%02d/%02d %02d:%02d\t%02d:%02d\t",
			     pEITItem->stStartTime.wYear, pEITItem->stStartTime.wMonth, pEITItem->stStartTime.wDay,
				 pEITItem->stStartTime.wHour, pEITItem->stStartTime.wMinute,
				 pEITItem->stRunTime.wHour, pEITItem->stRunTime.wMinute);
		lstrcat(szOutputLine, szTemp);

		// Channel number and names
		wsprintf(szTemp, "%d\t", nChannelNumber);
		lstrcat(szOutputLine, szTemp);
		if (v->pChannelData[nChannelNumber] != NULL)
		{
			wsprintf(szTemp, "%s\t%s\t", v->pChannelData[nChannelNumber]->szShortName, v->pChannelData[nChannelNumber]->szLongName);
			lstrcat(szOutputLine, szTemp);
		}
		else
			lstrcat(szOutputLine, "\t\t");
		
		// event name & description
		wsprintf(szTemp, "%s\t", pEITItem->szEventName);
		lstrcat(szOutputLine, szTemp);

		if (pEITItem->szShortEventDescription != NULL)
			wsprintf(szTemp, "%s\t", pEITItem->szShortEventDescription);
		else
			lstrcpy(szTemp, "\t");
		lstrcat(szOutputLine, szTemp);

		if (pEITItem->szLongEventDescription != NULL)
			wsprintf(szTemp, "%s\t", pEITItem->szLongEventDescription);
		else
			lstrcpy(szTemp, "\t");
		lstrcat(szOutputLine, szTemp);
			
		// all done
		lstrcat(szOutputLine, "\r\n");
		WriteFile(v->hEPGSaveHandle, szOutputLine, lstrlen(szOutputLine), &dwWritten, NULL);
		v->nEPGSaveCount++;
	}
}

void SaveExistingEPGData()
{
	int nChannel;

#ifdef DEBUG_MESSAGES
	OutputDebugString("SaveExistingEPGData\n");
#endif DEBUG_MESSAGES

	for (nChannel = 0; nChannel < MAX_EIT_CHANNEL_DATA; nChannel++)
	{
		if (v->pEvents[nChannel] != NULL)
		{
			PEITEVENT pCurrent = v->pEvents[nChannel];

			while(pCurrent != NULL)
			{
				SaveEPGData(pCurrent, nChannel);
				pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
			}
		}
	}

}

#endif LITE

BOOL myGetSaveFileName(LPOPENFILENAME lpofn)
{
	int i;
	char * szFileName = lpofn->lpstrFile;
	char * szInitialDir = (char *)lpofn->lpstrInitialDir;

	if (GetSaveFileName(lpofn) == FALSE)
		return FALSE;

	strcpy(szInitialDir, szFileName);
	for (i = strlen(szInitialDir); i > 0; i--)
	{
		if (szInitialDir[i] == '\\')
			break;
		szInitialDir[i] = 0;
	}
	return TRUE;
}

void GetLanguageFromDescriptor(char * szLanguage, int nPMTIndex, int nESIndex)
{
	int descriptors_length = v->pat.pmt[nPMTIndex].es[nESIndex].nDescriptorsLength;
	BYTE * descriptor_pointer = v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors;
	if (descriptor_pointer != NULL)
	{
		do
		{
			int i;
			BYTE descriptor_tag = *descriptor_pointer++;
			BYTE descriptor_length = *descriptor_pointer++; 
			descriptors_length =- 2;

			if (descriptor_tag == 10)
			{
				szLanguage[0] = descriptor_pointer[0];
				szLanguage[1] = descriptor_pointer[1];
				szLanguage[2] = descriptor_pointer[2];
				szLanguage[3] = 0;
				return;
			}
			for (i = 0; i < descriptor_length; i++)
			{
				descriptor_pointer++;
				descriptors_length--;
			}
		} while (descriptors_length > 0);
	}
}

void StripTrailingSpaces(char * szString)
{
	while (szString[lstrlen(szString) - 1] == ' ')
		szString[lstrlen(szString) - 1] = '\0';
}

char * TrueFalseString(BOOL fTrue)
{
	static char szTrue[] = {"True"};
	static char szFalse[] = {"False"};

	if (fTrue)
		return szTrue;
	return szFalse;
}

void EscapeReplaceXML(char * szBuffer)
{
	int nStringLength = lstrlen(szBuffer);	
	int i;
	char * szOutputBuffer;
	char * szCurrentOutput;

	if (!nStringLength)
		return;

	szOutputBuffer = szCurrentOutput = LocalAlloc(LPTR, nStringLength * 10);
	
	for (i = 0; i < nStringLength; i++)
	{
		switch(szBuffer[i])
		{
		case 34:
			memcpy(szCurrentOutput, "&quot;", 6); szCurrentOutput += 6;
			break;
		case '&':
			memcpy(szCurrentOutput, "&amp;", 5); szCurrentOutput += 5;
			break;
		case '<':
			memcpy(szCurrentOutput, "&lt;", 4); szCurrentOutput += 4;
			break;
		case '>':
			memcpy(szCurrentOutput, "&gt;", 4); szCurrentOutput += 4;
			break;
		case ':':
			if (v->fKeepSpecialXMLCharacters == FALSE)
				break;		// ignore these - screw up filenames
			*szCurrentOutput++ = szBuffer[i];
			break;	
		default:
			if ((unsigned char)szBuffer[i] >= ' ')
				*szCurrentOutput++ = szBuffer[i];
			break;	
		}
	}

	*szCurrentOutput = 0;
	lstrcpy(szBuffer, szOutputBuffer);
	LocalFree(szOutputBuffer);
}

void WriteHTMLLine(HANDLE hFile, char * szString)
{
	DWORD dwWritten;

	WriteFile(hFile, szString, lstrlen(szString), &dwWritten, NULL);
	if (dwWritten != (DWORD)lstrlen(szString))
		OutputDebugString("TSReader: WriteHTMLLine write errror\n");
	WriteFile(hFile, "\r\n", 2, &dwWritten, NULL);
}

void WriteHTMLASCII(HANDLE hFile, char * szBuffer)
{
	DWORD dwWritten;
	int i;
	int nOutputIndex = 0;
	int nInputLength = lstrlen(szBuffer);
	char * szNewBuffer = LocalAlloc(LMEM_FIXED, nInputLength * 2);

	for (i = 0; i < nInputLength; i++)
	{
		switch(szBuffer[i])
		{
		case '\r':
			break;
		case '\n':
			szNewBuffer[nOutputIndex++] = '<';
			szNewBuffer[nOutputIndex++] = 'B';
			szNewBuffer[nOutputIndex++] = 'R';
			szNewBuffer[nOutputIndex++] = '>';
			break;
		default:
			szNewBuffer[nOutputIndex++] = szBuffer[i];
			break;
		}
	}
	WriteFile(hFile, szNewBuffer, nOutputIndex, &dwWritten, NULL);
	LocalFree(szNewBuffer);

}

int GetVideoStreamCount()
{
	int nRetVal = 0;
	int nPMTIndex, nESIndex;
	
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
			case 0x01:	// mpeg-1 video
			case 0x02:	// mpeg-2 video
			case 0x10:	// MPEG-4 video
			case 0x1b:	// H.264 video
				nRetVal++;
				break;
			case 0x80:	// dc-ii video
				if (v->nNetworkPID != 0x0010)
					nRetVal++;
				break;
			}
		}
	}
	return nRetVal;
}

int GetProgramCount()
{
	int nRetVal = 0;
	int nPMTIndex;
	
	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		if (v->pat.pmt[nPMTIndex].nProgramNumber == 0)
			continue;
		nRetVal++;
	}

	return nRetVal;
}

void LoadVideoDecoderCrashThumbnail(int nESParsePMTIndex, int nESParseESIndex)
{
	HISSRC hSourceObject;
	char szPictureName[MAX_PATH];

	SourceHelper_GetTSReaderEXEDirectory(v->hInstance, szPictureName, sizeof(szPictureName));
	if (v->nThumbnailSize == 1)
		lstrcat(szPictureName, "\\videocrash_small.bmp");
	else
		lstrcat(szPictureName, "\\videocrash.bmp");
	hSourceObject = _ISOpenFileSource(szPictureName);
	if (hSourceObject != NULL)
	{
		DWORD dwPictureWidth, dwPictureHeight;

		EnterCriticalSection(&v->csThumbnails);
		v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame = _ISReadBMPToRGB(hSourceObject, &dwPictureWidth, &dwPictureHeight);
		if (v->nThumbnailSize == 2)
		{
			BYTE *pNewImage;
			DWORD dwNewWidth, dwNewHeight;

			/*if (v->nThumbnailSize == 1)
			{
				dwNewWidth = (dwPictureWidth * 2) / 3;
				dwNewHeight = (dwPictureHeight * 2) / 3;
				pNewImage = LocalAlloc(LPTR, dwNewWidth * dwNewHeight * 3);
				_ISDecimateRGB(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame,
				   dwPictureWidth, dwPictureHeight,
				   pNewImage,
				   dwNewWidth, dwNewHeight);
			}
			else*/
			{
				dwNewWidth = dwPictureWidth * 2;
				dwNewHeight = dwPictureHeight * 2;
				pNewImage = LocalAlloc(LPTR, dwNewWidth * dwNewHeight * 3);
				_ISResizeRGB(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame,
				   dwPictureWidth, dwPictureHeight,
				   pNewImage,
				   dwNewWidth, dwNewHeight);
			}
			LocalFree(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame);
			v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame = pNewImage;
			dwPictureWidth = dwNewWidth;
			dwPictureHeight = dwNewHeight;
		}
		v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoWidth = dwPictureWidth;
		v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoHeight = dwPictureHeight;
		v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].fDecoderCrashed = TRUE;
		
		LeaveCriticalSection(&v->csThumbnails);
		_ISCloseSource(hSourceObject);
		PostMessage(v->hDlgSIParser, WM_USER + 3, 0, 1);
	}
}

void YUVtoRGB(BYTE * pImage, BYTE * pY, BYTE * pU, BYTE * pV, int x, int y)
{
	int i;
	int uv_width  = x / 2;
	int uv_height = y / 2;

	for (i = 0; i < y; i++)
	{
		int sub_i_uv = ((i * uv_height) / y);
		int j;

		for (j = 0; j < x; ++j)
		{
			int sub_j_uv = ((j * uv_width) / x);
			int y = pY[(i * x) + j] - 16;
			int u = pU[(sub_i_uv * uv_width) + sub_j_uv] - 128;
			int v = pV[(sub_i_uv * uv_width) + sub_j_uv] - 128;
			int r = (int)((1.1644 * (float)y) + (1.5960 * (float)v));
			int g = (int)((1.1644 * (float)y) - (0.3918 * (float)u) - (0.8130 * (float)v));
			int b = (int)((1.1644 * (float)y) + (2.0172 * (float)u));

			if (r < 0)		r = 0;
			if (g < 0)		g = 0;
			if (b < 0)		b = 0;
			if (r > 255)	r = 255;
			if (g > 255)	g = 255;
			if (b > 255)	b = 255;

			pImage[(i * x + j) * 3 + 0] = r;
			pImage[(i * x + j) * 3 + 1] = g;
			pImage[(i * x + j) * 3 + 2] = b;
		}
	}
}

void GetNewThumbnailSize(int * nSourceHeight, int * nDestHeight, int * nDestWidth)
{
	*nDestHeight = 180;	// assume NTSC
	*nDestWidth = 240;

	if (*nSourceHeight == 1088)
		*nSourceHeight = 1080;
	if (*nSourceHeight == 576 || *nSourceHeight == 288)
		*nDestHeight = 216;		// PAL
	if (*nSourceHeight == 525)
		*nDestHeight = 192;		// NTSC 4:2:2 full frame
	if (*nSourceHeight == 625)
		*nDestHeight = 234;		// PAL 4:2:2 full frame
	
	switch(v->nThumbnailSize)
	{
	case 1:
		*nDestWidth *= 2; *nDestWidth /= 3;
		*nDestHeight *= 2; *nDestHeight /= 3;
		break;
	case 2:
		*nDestWidth *= 2;
		*nDestHeight *= 2;
		break;
	}
}

void GenerateSizedThumbnail(BYTE * pImage, int nDestWidth, int nDestHeight, int x, int nSourceHeight,
							int nESParsePMTIndex, int nESParseESIndex)
{
	BYTE * pDestBuffer = NULL;

	EnterCriticalSection(&v->csThumbnails);
	pDestBuffer = v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame;
	if (pDestBuffer != NULL)
	{
		if (nDestWidth != v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoWidth ||
			nDestHeight != v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoHeight)
		{
			LocalFree(pDestBuffer);
			pDestBuffer = NULL;
		}
	}
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].fDecoderCrashed = FALSE;
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoWidth = nDestWidth;
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoHeight = nDestHeight;
	if (pDestBuffer == NULL)
	{
		pDestBuffer = LocalAlloc(LPTR, nDestWidth * nDestHeight * 3);
		v->nThumbnailImageCount++;
	}
	//if (nDestWidth < x || nDestHeight < nSourceHeight)
		_ISResizeRGB(pImage,
				   x,
				   nSourceHeight,
				   pDestBuffer,
				   nDestWidth,
				   nDestHeight);
	/*else
		_ISDecimateRGB(pImage,
				   x,
				   nSourceHeight,
				   pDestBuffer,
				   nDestWidth,
				   nDestHeight);*/
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame = pDestBuffer;

	LeaveCriticalSection(&v->csThumbnails);
	PostMessage(v->hDlgSIParser, WM_USER + 3, 0, 1);
}

void GenerateAudioThumbnail(signed short * pSamples, int nAudioChannels, int nDestWidth, int nDestHeight, BYTE * pThumbnail,
							int nESParsePMTIndex, int nESParseESIndex)
{
	int nRightAverage = 0;
	int nLeftAverage = 0;
	int nAverageCount = 0;
	int nSampleIndex;
	int nDestXPos = 5;
	int nSampleNumber = 0;
	DWORD dwChannelTextColor = RGB(0x00, 0xff, 0x00);
	char szChannelDesc[] = {"L"};

	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].as.nChannels = nAudioChannels;
	for (nSampleIndex = 0; nSampleIndex < SAMPLES_REQUIRED; nSampleIndex += 2)
	{
		nLeftAverage += (int)pSamples[nSampleIndex];
		nRightAverage += (int)pSamples[nSampleIndex + 1];
		nAverageCount++;
		if (nAverageCount == CHARTTIMES)
		{
			int nPixelOffset;
			int nRow;
			int nOffset;

			nAverageCount = 0;
			nLeftAverage /= CHARTTIMES;
			nRightAverage /= CHARTTIMES;

			nRow = 24;
			nOffset = (int)((double)(nLeftAverage * 4) * (20.0/65536.0));
			if (nOffset < -10)
				nOffset = -10;
			if (nOffset > 10)
				nOffset = 10;
			nRow -= nOffset;
			nPixelOffset = ((nDestWidth * nRow) + nDestXPos) * 3;
			pThumbnail[nPixelOffset++] = 0x00;
			pThumbnail[nPixelOffset++] = 0xff;
			pThumbnail[nPixelOffset++] = 0x00;

			v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].as.nSampleData[0][nSampleNumber] = nOffset;

			if(nAudioChannels == 2)
			{
				nRow = 38;
				nOffset = (int)((double)(nRightAverage * 4) * (20.0/65536.0));
				if (nOffset < -10)
					nOffset = -10;
				if (nOffset > 10)
					nOffset = 10;
				nRow -= nOffset;
				nPixelOffset = ((nDestWidth * nRow) + nDestXPos) * 3;
				pThumbnail[nPixelOffset++] = 0xff;
				pThumbnail[nPixelOffset++] = 0xff;
				pThumbnail[nPixelOffset++] = 0x00;
				v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].as.nSampleData[1][nSampleNumber] = nOffset;
			}
				
			nDestXPos++;
			if (nDestXPos >= nDestWidth - 15)
				break;
			nSampleNumber++;

			nLeftAverage = 0;
			nRightAverage = 0;
		}
	}

	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].as.nSamples = nSampleNumber;
	EnterCriticalSection(&v->csThumbnails);
	if (v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame != NULL)
		LocalFree(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame);
	else
	{
		v->nThumbnailImageCount++;
	}
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame = pThumbnail;
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoWidth = nDestWidth;
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoHeight = nDestHeight;

	if(nAudioChannels != 2)
		szChannelDesc[0] = 'M';	
	_ISDrawTextOnRGB2(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame,
					  nDestWidth,
					  nDestHeight,
					  szChannelDesc,
					  &v->logfontChannelFont,
					  nDestWidth - 12,
					  16,
					  dwChannelTextColor);				
	
	if(nAudioChannels == 2)
	{
		dwChannelTextColor = RGB(0xff, 0xff, 0x00);
		szChannelDesc[0] = 'R';
		_ISDrawTextOnRGB2(v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame,
						  nDestWidth,
						  nDestHeight,
						  szChannelDesc,
						  &v->logfontChannelFont,
						  nDestWidth - 12,
						  32,
						  dwChannelTextColor);				
	}
	LeaveCriticalSection(&v->csThumbnails);
	PostMessage(v->hDlgSIParser, WM_USER + 3, 0, 1);
}

void SetupScrambledChannelThumbnail(int nESParsePMTIndex, int nESParseESIndex)
{
	BOOL fNewThumbnail = TRUE;
	BYTE * pDestBuffer;

	if (v->hScrambledPicture[v->nThumbnailSize] == NULL)
		return;		// no picture to display

	if (   v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nStreamType != 0x01
		&& v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nStreamType != 0x02
		&& v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nStreamType != 0x1b
		&& v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nStreamType != 0x80)
		return;		// not video ES

	pDestBuffer = v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame;
	if (pDestBuffer != NULL)
		fNewThumbnail = FALSE;

	EnterCriticalSection(&v->csThumbnails);
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoWidth = v->dwScrambledPictureWidth[v->nThumbnailSize];
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nVideoHeight = v->dwScrambledPictureHeight[v->nThumbnailSize];
	if (fNewThumbnail)
	{
		pDestBuffer = LocalAlloc(LPTR, v->dwScrambledPictureWidth[v->nThumbnailSize] * v->dwScrambledPictureHeight[v->nThumbnailSize] * 3);
		v->nThumbnailImageCount++;
	}
	memcpy(pDestBuffer, v->hScrambledPicture[v->nThumbnailSize], v->dwScrambledPictureWidth[v->nThumbnailSize] * v->dwScrambledPictureHeight[v->nThumbnailSize] * 3);
	v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].pRGBVideoFrame = pDestBuffer;
	LeaveCriticalSection(&v->csThumbnails);
	PostMessage(v->hDlgSIParser, WM_USER + 3, 0, 1);
#ifdef PRO
	if (v->hWndVideoMosaic != NULL)
		InvalidateRect(v->hWndVideoMosaic, NULL, FALSE);
#endif PRO
}

void GetVideoArea(int * xStart, int * yStart, int * xWidth, int * yHeight)
{
	RECT rcVideo, rcParent, rcScrollbar;

	GetWindowRect(GetDlgItem(v->hDlgSIParser, IDC_VIDEO_FRAME), &rcVideo);
	GetWindowRect(v->hWndMainWindow, &rcParent);
	GetClientRect(GetDlgItem(v->hDlgSIParser, IDC_SCROLL_THUMBNAILS), &rcScrollbar);
	
	*xStart = rcVideo.left - rcParent.left + 1;
	*yStart = rcVideo.top - rcParent.top - GetSystemMetrics(SM_CYMENU) - GetSystemMetrics(SM_CYCAPTION) + 12;
	if (xWidth != NULL)
		*xWidth = (rcVideo.right - rcVideo.left - 12 - rcScrollbar.right);
	if (yHeight != NULL)
		*yHeight = (rcVideo.bottom - rcVideo.top) - 22;
}

void InvalidateThumbnails()
{
	int xStart, yStart, xWidth, yHeight;
	RECT rcInvalidate;

	GetVideoArea(&xStart, &yStart, &xWidth, &yHeight);
	rcInvalidate.left = xStart;
	rcInvalidate.top = yStart;
	rcInvalidate.right = rcInvalidate.left + xWidth; 
	rcInvalidate.bottom = rcInvalidate.top + yHeight;

	InvalidateRect(v->hDlgSIParser, &rcInvalidate, FALSE);
}

int ReadFromMPEG2ESPipe(BYTE * pBuffer, int nLength, int nES)
{
	int nRequestedLength = nLength;
	DWORD dwRead;

	while (nLength && v->fRunning)
	{
		ReadFile(v->hMPEGDecoderReadPipe[nES], pBuffer, nLength, &dwRead, NULL);
		if (dwRead == 0)
			return 0;
		pBuffer += dwRead;
		nLength -= dwRead;
	}

	return nRequestedLength;
}


#ifdef PRO
BOOL GetArchiveThumbnailName(int nPMTIndex, char * szThumbnailName);	// in archive.c
void SaveArchiveThumbnail(char * szStatus, int nES)
{
	HISDEST	hDestinationObject;
	char szThumbnailName[MAX_PATH];

	if (szStatus != NULL)
	{
		lstrcat(szStatus, " - archiving");
		UpdateMainStatusText(szStatus);
	}

	if (GetArchiveThumbnailName(v->nESParsePMTIndex[nES], szThumbnailName) == TRUE)
	{
		hDestinationObject = _ISOpenFileDest(szThumbnailName);
		if (hDestinationObject != NULL)
		{
			_ISWriteRGBToJPG(hDestinationObject,
							 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].pRGBVideoFrame,
							 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].nVideoWidth,
							 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].nVideoHeight,
							 100,
							 0);
			_ISCloseDest(hDestinationObject);
		}
	}
}
#endif PRO

#ifndef LITE
#ifdef PRO
void XMLLogCheckItemCount();											// in tsreader.c
#endif PRO

void DecoderThread_SaveThumbnail(char * szStatus, int nES, int width, int height, BYTE * picbuf)
{
	char szRealFilename[MAX_PATH];
	char * szExtension;

	if (szStatus != NULL)
	{
		lstrcat(szStatus, " - saving");
		UpdateMainStatusText(szStatus);
	}

	lstrcpy(szRealFilename, v->szThumbnailBaseFilename);
	szExtension = GetExtensionPtr(szRealFilename);
	if (szExtension != NULL)
	{
		HISDEST	hDestinationObject;
		char szSavedExtension[128];
		char szChannelTag[256];

		*szExtension = '\0';
		lstrcpy(szSavedExtension, szExtension + 1);
		if (v->fSaveAllThumbnailsSameName == FALSE)
		{
			SYSTEMTIME st;
	
			GetLocalTime(&st);	
			wsprintf(szChannelTag, "_%d_%04d%02d%02d_%02d%02d%02d.%s",
					 v->pat.pmt[v->nESParsePMTIndex[nES]].nProgramNumber,
					 st.wYear, st.wMonth, st.wDay,
					 st.wHour, st.wMinute, st.wSecond,
					 szSavedExtension);
		}
		else
		{
			wsprintf(szChannelTag, "_%d.%s",
					 v->pat.pmt[v->nESParsePMTIndex[nES]].nProgramNumber,
					 szSavedExtension);										      
		}
		lstrcat(szRealFilename, szChannelTag);
		hDestinationObject = _ISOpenFileDest(szRealFilename);
		if (hDestinationObject != NULL)
		{
			if (v->fSavedThumbnailsFullSize)
			{
				int nSourceHeight = height;
				if (nSourceHeight == 1088)
					nSourceHeight = 1080;

				_ISWriteRGBToJPG(hDestinationObject,
								 picbuf,
								 width,
								 nSourceHeight,
								 100,
								 0);
			}
			else
				_ISWriteRGBToJPG(hDestinationObject,
								 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].pRGBVideoFrame,
								 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].nVideoWidth,
								 v->pat.pmt[v->nESParsePMTIndex[nES]].es[v->nESParseESIndex[nES]].nVideoHeight,
								 100,
								 0);
			_ISCloseDest(hDestinationObject);
#ifdef PRO
			if (v->fStreamingXMLMode)
			{
				BOOL fAlreadyGotThisOne = FALSE;
				int nXMLLogIndex;
				char * szFilenamePtr;

				EnterCriticalSection(&v->csXMLLog);
				
				// If this thumbnail is already queued, don't add it again
				for (nXMLLogIndex = 0; nXMLLogIndex < v->nXMLLogCount; nXMLLogIndex++)
				{
					if (v->XMLLog[nXMLLogIndex].nXMLLogType == XML_LOG_TYPE_THUMBNAIL &&
						v->XMLLog[nXMLLogIndex].fSent == FALSE && 
						v->XMLLog[nXMLLogIndex].nProgram == v->pat.pmt[v->nESParsePMTIndex[nES]].nProgramNumber &&
						v->XMLLog[nXMLLogIndex].nESIndex == v->nESParseESIndex[nES])
					{
						fAlreadyGotThisOne = TRUE;
						break;
					}
				}

				if (!fAlreadyGotThisOne)
				{			
					XMLLogCheckItemCount();

					v->XMLLog[v->nXMLLogCount].nXMLLogType = XML_LOG_TYPE_THUMBNAIL;
					szFilenamePtr = &szRealFilename[lstrlen(szRealFilename)];
					while (szFilenamePtr > szRealFilename)
					{
						if (*szFilenamePtr == '\\')
						{
							szFilenamePtr++;
							break;
						}
						szFilenamePtr--;
					}
					lstrcpy(v->XMLLog[v->nXMLLogCount].szFilename, szFilenamePtr);
					v->XMLLog[v->nXMLLogCount].nProgram = v->pat.pmt[v->nESParsePMTIndex[nES]].nProgramNumber;
					v->XMLLog[v->nXMLLogCount].nESIndex = v->nESParseESIndex[nES];
					v->XMLLog[v->nXMLLogCount].fSent = FALSE;
					v->nXMLLogCount++;
				}

				LeaveCriticalSection(&v->csXMLLog);
			}
#endif PRO
		}
	}
}
#endif LITE

#ifdef PRO
BYTE ReverseBits(BYTE bInput)
{
	BYTE x = 0;

	if (bInput & 0x80) x |= 0x01;
	if (bInput & 0x40) x |= 0x02;
	if (bInput & 0x20) x |= 0x04;
	if (bInput & 0x10) x |= 0x08;
	if (bInput & 0x08) x |= 0x10;
	if (bInput & 0x04) x |= 0x20;
	if (bInput & 0x02) x |= 0x40;
	if (bInput & 0x01) x |= 0x80;
	return x;
}
#endif PRO

int DetermineSignalType(char * szSignal)
{
	int nRetVal;
	char * szCompareSignal;

	szCompareSignal = strstr(szSignal, " ") + 1;
	nRetVal = SIGNAL_CHART_MODE_UNKNOWN;
	if (strstr(szCompareSignal, "SNR") != NULL)
		nRetVal = SIGNAL_CHART_MODE_SNR;
	else if (strstr(szCompareSignal, "BER") != NULL)
		nRetVal = SIGNAL_CHART_MODE_BER;
	else if (strstr(szCompareSignal, "Quality") != NULL)
		nRetVal = SIGNAL_CHART_MODE_QUALITY;
	else if (strstr(szCompareSignal, "%") != NULL && strstr(szCompareSignal, "dBm") != NULL)
		nRetVal = SIGNAL_CHART_MODE_QUALITY_DBM;

	return nRetVal;
}

void ExtractSignalData(int nSignalChartMode, float * fNewValues0, float * fNewValues1)
{
	char szSignal[64];

	*fNewValues0 = 0.0f;
	*fNewValues1 = 0.0f;

	GetSourceInfoLine(2, szSignal);
	switch(nSignalChartMode)
	{
	case SIGNAL_CHART_MODE_SNR:
		{
			char * szSNR;
			szSNR = strstr(szSignal, "SNR");
			if (szSNR != NULL)
				*fNewValues0 = (float)atof(szSNR + 4);
		}
		break;
	case SIGNAL_CHART_MODE_BER:
		{
			char * szBER;
			szBER = strstr(szSignal, "BER");
			if (szBER != NULL)
				*fNewValues0 = (float)atof(szBER + 4);
		}
		break;
	case SIGNAL_CHART_MODE_QUALITY:
		{
			char * szBaseData = strstr(szSignal, " ");
			char * szQuality = strstr(szBaseData, "Quality");
			char * szSignalPtr = strstr(szBaseData, "Signal");
			
			if (szQuality != NULL && szSignalPtr != NULL)
			{
				szQuality += 8;
				szSignalPtr += 7;
				*fNewValues0 = (float)atoi(szSignalPtr);
				*fNewValues1 = (float)atoi(szQuality);
			}
		}
		break;
	case SIGNAL_CHART_MODE_QUALITY_DBM:
		{
			char * szPercentage = strstr(szSignal, "%");

			if (szPercentage != NULL)
			{
				char * szFindQuality = szPercentage;
				char * szdBmStart = strstr(szSignal, "(");
				char * szdBmEnd = strstr(szSignal, ")");
				char * szdBmSeperator = strstr(szSignal, ".");

				while (*szFindQuality != ' ')
					szFindQuality--;
				szFindQuality++;
				*szPercentage = '\0';
				*fNewValues0 = (float)atoi(szFindQuality);

				if (szdBmStart != NULL && szdBmEnd != NULL && szdBmSeperator != NULL)
				{
					szdBmStart++;
					*szdBmSeperator = '\0';
					szdBmSeperator++;
					*szdBmEnd = '\0';
					*fNewValues1 = (float)atoi(szdBmStart) + ((float)atoi(szdBmSeperator) / 1000.0f);
				}
			}
		}
		break;
	}
}

int GetLogicalChannelNumber(int nProgramNumber)
{
	if (v->pChannelData[nProgramNumber] != NULL)
		return v->pChannelData[nProgramNumber]->nLogicalChannelNumber;
	return 0;
}

void GetBouquetName(int nBouquetIndex, char * szOutput)
{
	szOutput[0] = '\0';
	if (v->bat[nBouquetIndex].bouquet_descriptors_length)
	{
		int nLength = v->bat[nBouquetIndex].bouquet_descriptors_length;
		BYTE * pDescriptors = v->bat[nBouquetIndex].bouquet_descriptors;
		while (nLength)
		{
			int nTag = pDescriptors[0];
			int nDescriptorLength = pDescriptors[1];
			if (nTag == 0x47)
			{
				memcpy(szOutput, &pDescriptors[2], nDescriptorLength);
				szOutput[nDescriptorLength] = '\0';
				break;
			}
			pDescriptors += nDescriptorLength + 2;
			nLength -= nDescriptorLength + 2;
		}
	}
}

BOOL GetPIDTooltipInfo(int nPID, char * szString)
{
	int nESIndex, nPMTIndex, nCATIndex, nIPIndex;

	// Try the base PIDs like PAT/PMT
	switch(nPID)
	{
	case 0x0000:
		if (v->nNullPID == 0x0000)
			wsprintf(szString, "%s DSS NULL Packet", FormatTooltipPID(0x0000));
		else
			wsprintf(szString, "%s MPEG-2 Program Assocation Table", FormatTooltipPID(0x0000));
		return TRUE;
	case 0x0001:
		if (v->nNullPID == 0x0000)
			wsprintf(szString, "%s DSS Master Guide Table", FormatTooltipPID(0x0001));
		else
			wsprintf(szString, "%s MPEG-2 Conditional Access Table", FormatTooltipPID(0x0001));
		return TRUE;
	case 0x1fff:
		if (v->nNullPID != 0x0000)
			wsprintf(szString, "%s MPEG-2 NULL Packet", FormatTooltipPID(0x1fff));
		return TRUE;
	}

	// Don't bother if it's DSS
	if (v->nNullPID == 0x0000)
	{
		wsprintf(szString, "%s DSS Packet", FormatTooltipPID(nPID));
		return TRUE;
	}

	// Check the CAT
	for (nCATIndex = 0; nCATIndex < MAX_CAT_DESCRIPTORS; nCATIndex++)
	{
		if (v->cat.pDescriptor[nCATIndex] == NULL)
			break;
		if (v->cat.pDescriptor[nCATIndex][0] == 9)
		{
			int nCAPID = ((v->cat.pDescriptor[nCATIndex][4] << 8) + v->cat.pDescriptor[nCATIndex][5]) & 0x1fff;

			if (nCAPID == nPID)
			{
				int nCASystemID = (v->cat.pDescriptor[nCATIndex][2] << 8) + v->cat.pDescriptor[nCATIndex][3];
				char szCAName[50] = {0};

				FormatCASystemName(nCASystemID, szCAName);
				wsprintf(szString, "%s EMM for system ID 0x%04x (%d) %s", FormatTooltipPID(nPID), nCASystemID, nCASystemID, szCAName);
				return TRUE;
			}
		}
	}

	// Try network extensions
	if (v->nNetworkPID == 0x0010)
	{
		// DVB system
		switch(nPID)
		{
		case 0x0002:
			wsprintf(szString, "%s DVB Transport Stream Descriptor Table", FormatTooltipPID(0x0002));
			return TRUE;
		case 0x0003:
		case 0x0004:
		case 0x0005:
		case 0x0006:
		case 0x0007:
		case 0x0008:
		case 0x0009:
		case 0x000a:
		case 0x000b:
		case 0x000c:
		case 0x000d:
		case 0x000e:
		case 0x000f:
			wsprintf(szString, "%s DVB Reserved", FormatTooltipPID(nPID));
			return TRUE;
		case 0x0010:
			wsprintf(szString, "%s DVB Network Information Table", FormatTooltipPID(0x0010));
			return TRUE;
		case 0x0011:
			wsprintf(szString, "%s DVB Service Definition Table", FormatTooltipPID(0x0011));
			return TRUE;
		case 0x0012:
			wsprintf(szString, "%s DVB Event Information Table", FormatTooltipPID(0x0012));
			return TRUE;
		case 0x0013:
			wsprintf(szString, "%s DVB Running Status Table", FormatTooltipPID(0x0013));
			return TRUE;
		case 0x0014:
			wsprintf(szString, "%s DVB Time Definition and Offset Tables", FormatTooltipPID(0x0014));
			return TRUE;
		case 0x0015:
			wsprintf(szString, "%s DVB Network Synchronization", FormatTooltipPID(0x0015));
			return TRUE;
		case 0x0016:
		case 0x0017:
		case 0x0018:
		case 0x0019:
		case 0x001a:
		case 0x001b:
			wsprintf(szString, "%s DVB Reserved for Future Use", FormatTooltipPID(nPID));
			return TRUE;
		case 0x001c:
			wsprintf(szString, "%s DVB In-Band Signalling", FormatTooltipPID(nPID));
			return TRUE;
		case 0x001d:
			wsprintf(szString, "%s DVB Measurement", FormatTooltipPID(nPID));
			return TRUE;
		case 0x001e:
			wsprintf(szString, "%s DVB DIT", FormatTooltipPID(nPID));
			return TRUE;
		case 0x001f:
			wsprintf(szString, "%s DVB SIT", FormatTooltipPID(nPID));
			return TRUE;
		}
	}
	else if (v->nNetworkPID == 0x1ffb)
	{
		int i;

		// ATSC system
		if (nPID == 0x1ffb)
		{
			wsprintf(szString, "%s ATSC Base PID (TVCT, MGT, RT, STT)", FormatTooltipPID(nPID));
			return TRUE;
		}

		// See if it's one of the tables indexed by the MGT
		for (i = 0; i < MAX_MGT_ENTRIES; i++)
		{
			char szTableName[50];

			if (v->mgt[i].nTableType == -1)
				break;

			if (v->mgt[i].nTablePID == nPID)
			{	
				if (v->mgt[i].nTableType == 0x0000)
					lstrcpy(szTableName, "Terrestrial VCT");
				else if (v->mgt[i].nTableType == 0x0001)
					lstrcpy(szTableName, "Terrestrial VCT");
				else if (v->mgt[i].nTableType == 0x0002)
					lstrcpy(szTableName, "Cable VCT");
				else if (v->mgt[i].nTableType == 0x0003)
					lstrcpy(szTableName, "Cable VCT");
				else if (v->mgt[i].nTableType == 0x0004)
					lstrcpy(szTableName, "Channel ETT");
				else if ( (v->mgt[i].nTableType >= 0x0100) && (v->mgt[i].nTableType <= 0x017f) )
					wsprintf(szTableName, "EIT-%d", v->mgt[i].nTableType - 0x100);
				else if ( (v->mgt[i].nTableType >= 0x0200) && (v->mgt[i].nTableType <= 0x027f) )
					wsprintf(szTableName, "Event ETT-%d", v->mgt[i].nTableType - 0x200);
				else if ( (v->mgt[i].nTableType >= 0x0301) && (v->mgt[i].nTableType <= 0x03ff) )
					wsprintf(szTableName, "RRT with rating_region %d", v->mgt[i].nTableType - 0x300);
				else if ( (v->mgt[i].nTableType >= 0x0400) && (v->mgt[i].nTableType <= 0x0fff) )
					lstrcpy(szTableName, "User Private");
				else
					lstrcpy(szTableName, "Reserved for future ATSC use");

				wsprintf(szString, "%s ATSC %s", FormatTooltipPID(nPID), szTableName);
				return TRUE;
			}
		}
	}
	else if (v->nNetworkPID == 0x0ffe)
	{
		// DCII system
		if (nPID == 0x0ffe)
		{
			wsprintf(szString, "%s DCII Network Tables", FormatTooltipPID(nPID));
			return TRUE;
		}
	}

	// Not a system table. Scan the PMTs and their ES PIDs to see if we get a hit
	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		if (v->pat.pmt[nPMTIndex].nPMTPID == nPID)
		{
			int nLCN = GetLogicalChannelNumber(v->pat.pmt[nPMTIndex].nProgramNumber);
			char szLogicalChannelNumber[64] = {0};
			if (nLCN != 0)
				wsprintf(szLogicalChannelNumber, "%d/", nLCN);
			wsprintf(szString, "%s MPEG-2 PMT for program %s%d", FormatTooltipPID(nPID), szLogicalChannelNumber, v->pat.pmt[nPMTIndex].nProgramNumber);
			return TRUE;
		}
		for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
		{
			if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
				break;
			if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == nPID)
			{
				int nLCN = GetLogicalChannelNumber(v->pat.pmt[nPMTIndex].nProgramNumber);
				char szStreamTypeEnglish[128];
				char szLogicalChannelNumber[64] = {0};

				DecodeStreamType(v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType, szStreamTypeEnglish, nPMTIndex, nESIndex);
				if (nLCN != 0)
					wsprintf(szLogicalChannelNumber, "%d/", nLCN);
				wsprintf(szString, "%s %s for program %s%d", FormatTooltipPID(nPID), szStreamTypeEnglish, szLogicalChannelNumber, v->pat.pmt[nPMTIndex].nProgramNumber);
				return TRUE;
			}

			// Wasn't this ES stream but check descriptors for the CA
			if (v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors != NULL)
			{
				int nDescriptorsLength = v->pat.pmt[nPMTIndex].es[nESIndex].nDescriptorsLength;
				int nCurrentIndex = 0;

				do
				{
					if (v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex] == 0x09)	// CA tag?
					{
						int nCASystemID = (v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex + 2] << 8)
							            + v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex + 3];
						int nCAPID = ((v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex + 4] << 8)
							         + v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex + 5]) & 0x1fff;

						if (nCAPID == nPID)
						{
							int nLCN = GetLogicalChannelNumber(v->pat.pmt[nPMTIndex].nProgramNumber);
							char szCAName[64] = {0};
							char szLogicalChannelNumber[64] = {0};

							FormatCASystemName(nCASystemID, szCAName);
							if (nLCN != 0)
								wsprintf(szLogicalChannelNumber, "%d/", nLCN);
							wsprintf(szString, "%s ECM for system ID 0x%04x (%d) %s for program %s%d", FormatTooltipPID(nPID), nCASystemID, nCASystemID, szCAName, szLogicalChannelNumber, v->pat.pmt[nPMTIndex].nProgramNumber);
							return TRUE;				
						}
					}

					nCurrentIndex += (BYTE)v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors[nCurrentIndex + 1]; // descriptor length
					nCurrentIndex += 2;	// descriptor tag and length
				} while (nCurrentIndex < nDescriptorsLength);
			}
		}
		if (v->pat.pmt[nPMTIndex].nPCRPID == nPID)
		{
			int nLCN = GetLogicalChannelNumber(v->pat.pmt[nPMTIndex].nProgramNumber);
			char szLogicalChannelNumber[64] = {0};

			if (nLCN != 0)
				wsprintf(szLogicalChannelNumber, "%d/", nLCN);
			wsprintf(szString, "%s MPEG-2 PCR for program %s%d", FormatTooltipPID(nPID), szLogicalChannelNumber, v->pat.pmt[nPMTIndex].nProgramNumber);
			return TRUE;
		}
	}

	// Go through the PMT's ProgramInfo to see if we have an ECM PID pointer
	for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
	{
		if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
			break;
		if (v->pat.pmt[nPMTIndex].nProgramInfoLength)
		{
			int nOffset;

			for (nOffset = 0; nOffset < v->pat.pmt[nPMTIndex].nProgramInfoLength;)
			{
				int nDescriptor = v->pat.pmt[nPMTIndex].pProgramInfo[nOffset];
				int nDescriptorLength = v->pat.pmt[nPMTIndex].pProgramInfo[nOffset + 1];

				if (nDescriptorLength)
				{
					if (nDescriptor == 0x09)
					{
						int nCASystemID = (v->pat.pmt[nPMTIndex].pProgramInfo[nOffset + 2] << 8) + v->pat.pmt[nPMTIndex].pProgramInfo[nOffset + 3];
						int nCAPID = ((v->pat.pmt[nPMTIndex].pProgramInfo[nOffset + 4] << 8) + v->pat.pmt[nPMTIndex].pProgramInfo[nOffset + 5]) & 0x1fff;

						if (nCAPID == nPID)
						{
							char szCAName[50] = {0};

							FormatCASystemName(nCASystemID, szCAName);
							wsprintf(szString, "%s ECM for system ID 0x%04x (%d) %s for program %d", FormatTooltipPID(nPID), nCASystemID, nCASystemID, szCAName, v->pat.pmt[nPMTIndex].nProgramNumber);
							return TRUE;				
						}
					}
				}
				nOffset += nDescriptorLength + 2;
			}
		}
	}

	// See if it's in the IPPID list
	for (nIPIndex = 0; nIPIndex < 16; nIPIndex++)
	{
		if (v->ippid[nIPIndex].nPID == nPID)
		{
			wsprintf(szString, "%s IP/DVB Traffic", FormatTooltipPID(nPID));
			return TRUE;
		}
	}

	// Unknown use
	wsprintf(szString, "%s Unknown usage", FormatTooltipPID(nPID));
	return FALSE;
}

void GetSourceInfoLine(int nLine, char * szOutput)
{
	switch(nLine)
	{
	case 0:
#ifndef DTVSENTINEL
		wsprintf(szOutput, "Source: %s", v->szSourceModuleDescription);
#else DTVSENTINEL
		szOutput[0] = '\0';
#endif DTVSENTINEL
		break;
	case 1:
		{
			char szTunerString[64] = {"n/a"};
			if (GetTunerString != NULL)
				GetTunerString(szTunerString);
			wsprintf(szOutput, "Tuner: %s", szTunerString);
		}
		break;
	case 2:
		{
			char szSignalString[64] = {"n/a"};
			if (GetSignalString != NULL)
				GetSignalString(szSignalString);
			wsprintf(szOutput, "Signal: %s", szSignalString);
		}
		break;
	case 5:
		{
			if (v->fEPGSaveEnabled == TRUE)
				wsprintf(szOutput, "Save All EPG: %d Items", v->nEPGSaveCount);
		}
		break;
#ifdef PRO
	case 6:
		{
			if (lstrlen(v->szProfileName))
				wsprintf(szOutput, "Profile: %s", v->szProfileName);
			else
				lstrcpy(szOutput, "Profile: Default");
		}
		break;
#endif PRO
	case 7:
		{
			char szNetworkMode[64] = {"Unknown"};

			switch(v->nNetworkPID)
			{
			case 0x0010:
				if (v->fISDB)
					lstrcpy(szNetworkMode, "ISDB");
				else
					lstrcpy(szNetworkMode, "DVB");
				break;
			case 0x0ffe:
				lstrcpy(szNetworkMode, "DCII");
				break;
			case 0x1ffb:
				lstrcpy(szNetworkMode, "ATSC");
				break;
			}
			wsprintf(szOutput, "Network Type: %s", szNetworkMode);
		}
		break;
	case 8:
		{
			SYSTEMTIME stSystemTime;
			FILETIME ftNow;
			DWORD64 lnNow;
			DWORD64 lnMultiplier = 10000000;
			int nRunTime, nHours, nMinutes, nSeconds;	

			GetSystemTime(&stSystemTime);
			SystemTimeToFileTime(&stSystemTime, &ftNow);
			memcpy(&lnNow, &ftNow, sizeof(DWORD64));

			nRunTime = (int)((lnNow - v->lnStartTime) / lnMultiplier);
			nHours = nRunTime / (60 * 60);
			nRunTime -= nHours * 60 * 60;
			nMinutes = nRunTime / 60;
			nRunTime -= nMinutes * 60;
			nSeconds = nRunTime;

			wsprintf(szOutput, "Run Time: %03d:%02d:%02d", nHours, nMinutes, nSeconds);

			if (v->lnRecordTime != 0)
			{
				int nRecordTime = (int)((lnNow - v->lnRecordTime) / lnMultiplier);
				char szTemp[128];

				nHours = nRecordTime / (60 * 60);
				nRecordTime -= nHours * 60 * 60;
				nMinutes = nRecordTime / 60;
				nRecordTime -= nMinutes * 60;
				nSeconds = nRecordTime;
				wsprintf(szTemp, " Record: %03d:%02d:%02d", nHours, nMinutes, nSeconds);
				lstrcat(szOutput, szTemp);
			}
		}
		break;
	default:
		szOutput[0] = '\0';
		break;
	}
}

char * FormatTooltipPID(int nPID)
{
	static char szResult[32];

	wsprintf(szResult, v->szOutputPIDFlags, nPID);
	return szResult;
}
