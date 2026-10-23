// Auto TS Recorder
//
// Makes a short recording of every selected mux of a DVB-C channel plan (CablePlans\*.lst),
// on a schedule or on demand. For each mux: retune quietly (the same way the control server
// retunes), wait for lock (retry, then skip), wait the stabilize time, check free disk space,
// record the whole TS for the set length, log signal quality to a CSV file and apply the
// retention policy. After the pass TSReader returns to the tune it had before.
//
// The source must take DVB-C parameters "freq sr QAM inversion bandwidth" (e.g. the DekTec
// DTU-236 QAM source). The engine runs on a one second timer on the main window.
//
// Command line: -autorecord runs one pass right after start-up and then closes TSReader.

#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include "TSReader.h"
#include "resource.h"

extern PVARIABLES v;
extern char gszAppName[];

// From TSReader.c
void RestartTSReader_Stop(HWND hWnd);
void RestartTSReader_Start(HWND hWnd);
void RecordStream(HWND hWnd, BOOL fEntireTS, int nPID);
void EnableOrDisableStreamMenuItems(HWND hDlg, BOOL fEnable);
void GetRecordStartTime();
void UpdateSecondaryStatusText(char * szText);
void SaveSettings();
void StreamDecoder(HWND hWnd);
void __cdecl SourceHelper_GetTSReaderEXEDirectory(HINSTANCE hInstance, char * szCurrentDir, int nCurrentDirLength);
extern BOOL (* ParseSourceModuleCommandLine) (PSOURCESTRUCT pss, char * szCommandLine, BOOL fQuiet);
extern BOOL (* GetSignalString) (char * szString);
extern BOOL (* GetDescription) (char * szDescription, char * szCommandLineParameters, BOOL * fCanBeStopped, int * nMaxPIDs, DWORD * dwCapabilities);

#define AUTOREC_TIMER_ID	0xA7EC
#define AUTOREC_MAX_MUXES	128

typedef struct
{
	int nFrequency;		// kHz
	int nSymbolRate;	// kS/s
	int nQAMIndex;		// 0-4 = QAM-16..QAM-256
	int nInverted;
	int nBandwidthIndex;	// 0-2 = 6/7/8 MHz
	char szLabel[48];
} AUTORECMUX;

enum { AR_IDLE, AR_TUNE, AR_WAIT_LOCK, AR_STABILIZE, AR_RECORDING, AR_NEXT, AR_RESTORE, AR_WAIT_RESTORE };

static BOOL fDialogOpen;		// a scheduled pass waits while the settings window is open

static struct
{
	int nState;
	AUTORECMUX muxes[AUTOREC_MAX_MUXES];		// selected muxes of this pass
	int nMuxes;
	int nCurrent;
	int nAttempt;
	DWORD dwStateStart;
	char szFile[MAX_PATH];
	BOOL fSavedSplit;
	BOOL fRecordingStarted;
	// tune to return to after the pass
	BOOL fHaveSavedTune;
	int nSavedFrequency, nSavedSymbolRate, nSavedQAM, nSavedBandwidth;
	BOOL fSavedInversion;
	// summary
	int nRecorded, nNoLock, nSkipped;
	__int64 nNextRun;			// local FILETIME, 0 = not scheduled
	char szLastResult[256];
	BOOL fExitWhenDone;
	BOOL fStartupPassDone;
} ar;

// ------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------

static __int64 NowLocal()
{
	SYSTEMTIME st;
	__int64 n;

	GetLocalTime(&st);
	SystemTimeToFileTime(&st, (FILETIME *)&n);
	return n;
}

#define FT_MINUTE ((__int64)600000000)

static void FormatFileTime(__int64 n, char * szOut)
{
	SYSTEMTIME st;

	if (n == 0)
	{
		lstrcpy(szOut, "-");
		return;
	}
	FileTimeToSystemTime((FILETIME *)&n, &st);
	wsprintf(szOut, "%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void GetPlanFolder(char * szPath)
{
	SourceHelper_GetTSReaderEXEDirectory(v->hInstance, szPath, MAX_PATH);
	lstrcat(szPath, "\\CablePlans");
}

// Same format as the DVB-C tune dialog's plans: freq_kHz sr_kS QAM inverted bw_MHz name
static int LoadPlan(const char * szPlan, AUTORECMUX * pMuxes, int nMax)
{
	char szPath[MAX_PATH];
	char szLine[256];
	FILE * f;
	int nMuxes = 0;

	GetPlanFolder(szPath);
	lstrcat(szPath, "\\");
	lstrcat(szPath, szPlan);
	lstrcat(szPath, ".lst");
	f = fopen(szPath, "r");
	if (f == NULL)
		return 0;
	while (nMuxes < nMax && fgets(szLine, sizeof(szLine), f))
	{
		int nFrequency, nSymbolRate, nQAM, nInverted, nBandwidth, nUsed = 0;
		char * pComment = strchr(szLine, '#');
		char * pLabel;
		AUTORECMUX * pMux = &pMuxes[nMuxes];

		if (pComment != NULL)
			*pComment = '\0';
		if (sscanf(szLine, "%d %d %d %d %d %n", &nFrequency, &nSymbolRate, &nQAM, &nInverted, &nBandwidth, &nUsed) < 5)
			continue;
		pMux->nFrequency = nFrequency;
		pMux->nSymbolRate = nSymbolRate;
		pMux->nQAMIndex = nQAM == 16 ? 0 : nQAM == 32 ? 1 : nQAM == 64 ? 2 : nQAM == 128 ? 3 : 4;
		pMux->nInverted = nInverted ? 1 : 0;
		pMux->nBandwidthIndex = nBandwidth == 6 ? 0 : nBandwidth == 7 ? 1 : 2;
		pLabel = szLine + nUsed;
		lstrcpyn(pMux->szLabel, pLabel, sizeof(pMux->szLabel));
		{
			int nEnd = lstrlen(pMux->szLabel);
			while (nEnd > 0 && (BYTE)pMux->szLabel[nEnd - 1] <= ' ')
				pMux->szLabel[--nEnd] = '\0';
		}
		nMuxes++;
	}
	fclose(f);
	return nMuxes;
}

// v->szAutoRecMuxes: "" = every mux of the plan, otherwise comma separated kHz values
static BOOL IsMuxSelected(int nFrequency)
{
	char szKey[16];
	char szList[1032];

	if (!lstrlen(v->szAutoRecMuxes))
		return TRUE;
	wsprintf(szKey, ",%d,", nFrequency);
	wsprintf(szList, ",%s,", v->szAutoRecMuxes);
	return strstr(szList, szKey) != NULL;
}

static void FormatFrequency(int nFrequency, char * szOut)
{
	if (nFrequency % 1000)
		wsprintf(szOut, "%d.%03d", nFrequency / 1000, nFrequency % 1000);
	else
		wsprintf(szOut, "%d", nFrequency / 1000);
}

// [prefix_]<freq>MHz_TS<id>[_suffix]_<yyyymmdd_hhmmss>.ts
static void BuildFileName(const AUTORECMUX * pMux, int nTSID, char * szFile)
{
	SYSTEMTIME st;
	char szFrequency[32];
	char szName[MAX_PATH];

	GetLocalTime(&st);
	FormatFrequency(pMux->nFrequency, szFrequency);
	szName[0] = '\0';
	if (lstrlen(v->szAutoRecPrefix))
		wsprintf(szName, "%s_", v->szAutoRecPrefix);
	wsprintf(szName + lstrlen(szName), "%sMHz_TS%d", szFrequency, nTSID);
	if (lstrlen(v->szAutoRecSuffix))
		wsprintf(szName + lstrlen(szName), "_%s", v->szAutoRecSuffix);
	wsprintf(szName + lstrlen(szName), "_%04d%02d%02d_%02d%02d%02d.ts",
			 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
	wsprintf(szFile, "%s\\%s", v->szAutoRecFolder, szName);
}

static double FreeSpaceGB()
{
	ULARGE_INTEGER uFree;

	if (!GetDiskFreeSpaceEx(v->szAutoRecFolder, &uFree, NULL, NULL))
		return -1.0;
	return (double)(__int64)uFree.QuadPart / (1024.0 * 1024.0 * 1024.0);
}

typedef struct { char szPath[MAX_PATH]; __int64 nTime; } RECFILE;

static int __cdecl CompareNewestFirst(const void * a, const void * b)
{
	__int64 na = ((const RECFILE *)a)->nTime, nb = ((const RECFILE *)b)->nTime;
	return na < nb ? 1 : na > nb ? -1 : 0;
}

// Retention: keep the newest N recordings of this mux and delete recordings older than
// N days. Only files that match the recorder's own name pattern are touched.
static int ApplyRetention(const AUTORECMUX * pMux)
{
	char szPattern[MAX_PATH];
	char szFrequency[32];
	WIN32_FIND_DATA fd;
	HANDLE hFind;
	static RECFILE files[512];
	int nFiles = 0, nDeleted = 0, i;
	__int64 nOldest = 0;

	if (v->nAutoRecMaxAgeDays > 0)
		nOldest = NowLocal() - (__int64)v->nAutoRecMaxAgeDays * 24 * 60 * FT_MINUTE;

	FormatFrequency(pMux->nFrequency, szFrequency);
	wsprintf(szPattern, "%s\\%s%s%sMHz_TS*.ts", v->szAutoRecFolder,
			 v->szAutoRecPrefix, lstrlen(v->szAutoRecPrefix) ? "_" : "", szFrequency);
	hFind = FindFirstFile(szPattern, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return 0;
	do
	{
		FILETIME ftLocal;
		if (nFiles == sizeof(files) / sizeof(files[0]))
			break;
		wsprintf(files[nFiles].szPath, "%s\\%s", v->szAutoRecFolder, fd.cFileName);
		FileTimeToLocalFileTime(&fd.ftLastWriteTime, &ftLocal);
		files[nFiles].nTime = *(__int64 *)&ftLocal;
		nFiles++;
	} while (FindNextFile(hFind, &fd));
	FindClose(hFind);

	qsort(files, nFiles, sizeof(RECFILE), CompareNewestFirst);
	for (i = 0; i < nFiles; i++)
	{
		BOOL fDelete = FALSE;
		if (v->nAutoRecKeep > 0 && i >= v->nAutoRecKeep)
			fDelete = TRUE;
		if (nOldest && files[i].nTime < nOldest && i > 0)	// never delete the newest one by age
			fDelete = TRUE;
		if (fDelete && DeleteFile(files[i].szPath))
			nDeleted++;
	}
	return nDeleted;
}

// One line per mux and pass: AutoRecord_log.csv in the recording folder
static void WriteLog(const AUTORECMUX * pMux, const char * szResult, int nTSID, const char * szSignal,
					 double dBitrate, int nCCErrors, int nServices, const char * szFile, double dSizeMB)
{
	char szPath[MAX_PATH];
	char szLine[1024];
	char szMER[16] = "", szLevel[16] = "";
	char szFrequency[32];
	HANDLE hFile;
	DWORD dwWritten;
	BOOL fNew;
	SYSTEMTIME st;
	const char * p;

	if (!v->fAutoRecCSV)
		return;
	wsprintf(szPath, "%s\\AutoRecord_log.csv", v->szAutoRecFolder);
	fNew = GetFileAttributes(szPath) == INVALID_FILE_ATTRIBUTES;
	hFile = CreateFile(szPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return;
	SetFilePointer(hFile, 0, NULL, FILE_END);
	if (fNew)
	{
		static char szHeader[] = "Date;Time;Plan;Frequency_MHz;Name;TS_ID;Result;Attempts;MER_dB;Level_dBmV;Bitrate_Mbps;CC_errors;Services;File;Size_MB;Signal\r\n";
		WriteFile(hFile, szHeader, lstrlen(szHeader), &dwWritten, NULL);
	}
	// MER and level as reported by the source, e.g. "Locked MER 35.8 dB Level 3.5 dBmV"
	if (szSignal && (p = strstr(szSignal, "MER ")) != NULL)
		sscanf(p + 4, "%15[0-9.-]", szMER);
	if (szSignal && (p = strstr(szSignal, "Level ")) != NULL)
		sscanf(p + 6, "%15[0-9.-]", szLevel);
	GetLocalTime(&st);
	FormatFrequency(pMux->nFrequency, szFrequency);
	_snprintf(szLine, sizeof(szLine) - 1,
			  "%04d-%02d-%02d;%02d:%02d:%02d;%s;%s;%s;%d;%s;%d;%s;%s;%.2f;%d;%d;%s;%.1f;%s\r\n",
			  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			  v->szAutoRecPlan, szFrequency, pMux->szLabel, nTSID, szResult, ar.nAttempt,
			  szMER, szLevel, dBitrate, nCCErrors, nServices,
			  szFile ? szFile : "", dSizeMB, szSignal ? szSignal : "");
	szLine[sizeof(szLine) - 1] = '\0';
	WriteFile(hFile, szLine, lstrlen(szLine), &dwWritten, NULL);
	CloseHandle(hFile);
}

static int CountServices()
{
	int i, nServices = 0;

	for (i = 0; i < MAX_PAT_ENTRIES; i++)
	{
		if (v->pat.pmt[i].nPMTPID == 0)
			break;
		if (v->pat.pmt[i].nProgramNumber != 0)
			nServices++;
	}
	return nServices;
}

static double CurrentBitrateMbps()
{
	double dBitrate = 0.0;

	EnterCriticalSection(&v->ss.csPIDCounter);
	if (v->dDisplayMuxRate > 0 && v->nMuxRateCounter > 0)
		dBitrate = (v->dDisplayMuxRate / (double)v->nMuxRateCounter) * 8.0 / 1000000.0;
	LeaveCriticalSection(&v->ss.csPIDCounter);
	return dBitrate;
}

static void SetStatus(const char * szFormat, ...)
{
	char szTemp[512];
	va_list args;

	va_start(args, szFormat);
	_vsnprintf(szTemp, sizeof(szTemp) - 1, szFormat, args);
	va_end(args);
	szTemp[sizeof(szTemp) - 1] = '\0';
	UpdateSecondaryStatusText(szTemp);
}

// ------------------------------------------------------------------------------------
// Schedule
// ------------------------------------------------------------------------------------

static BOOL WeekdayAllowed(__int64 nTime)
{
	SYSTEMTIME st;
	int nBit;

	FileTimeToSystemTime((FILETIME *)&nTime, &st);
	nBit = (st.wDayOfWeek + 6) % 7;		// Monday = bit 0 ... Sunday = bit 6
	return (v->nAutoRecWeekdays & (1 << nBit)) != 0;
}

// Next run after "now" (local FILETIME), 0 when the schedule is off or has no valid times
static __int64 ComputeNextRun(__int64 nNow)
{
	if (!v->fAutoRecEnabled || v->nAutoRecWeekdays == 0)
		return 0;

	if (v->nAutoRecMode == 2)
	{
		// Daily at the listed times "06:00,18:30" on the allowed weekdays
		SYSTEMTIME st;
		__int64 nMidnight, nBest = 0;
		int nDay;

		FileTimeToSystemTime((FILETIME *)&nNow, &st);
		st.wHour = st.wMinute = st.wSecond = st.wMilliseconds = 0;
		SystemTimeToFileTime(&st, (FILETIME *)&nMidnight);
		for (nDay = 0; nDay < 8 && nBest == 0; nDay++)
		{
			__int64 nDayStart = nMidnight + (__int64)nDay * 24 * 60 * FT_MINUTE;
			const char * p = v->szAutoRecTimes;

			if (!WeekdayAllowed(nDayStart))
				continue;
			while (*p)
			{
				int nHour, nMinute;
				if (sscanf(p, "%d:%d", &nHour, &nMinute) == 2 && nHour >= 0 && nHour < 24 && nMinute >= 0 && nMinute < 60)
				{
					__int64 nCandidate = nDayStart + (__int64)(nHour * 60 + nMinute) * FT_MINUTE;
					if (nCandidate > nNow && (nBest == 0 || nCandidate < nBest))
						nBest = nCandidate;
				}
				while (*p && *p != ',' && *p != ';' && *p != ' ')
					p++;
				while (*p == ',' || *p == ';' || *p == ' ')
					p++;
			}
		}
		return nBest;
	}
	else
	{
		// Every N minutes / hours, skipping days that are not allowed
		int nMinutes = v->nAutoRecMode == 1 ? v->nAutoRecInterval * 60 : v->nAutoRecInterval;
		__int64 nNext;
		int nGuard;

		if (nMinutes < 1)
			nMinutes = 1;
		nNext = nNow + (__int64)nMinutes * FT_MINUTE;
		for (nGuard = 0; nGuard < 10000 && !WeekdayAllowed(nNext); nGuard++)
			nNext += (__int64)nMinutes * FT_MINUTE;
		return nNext;
	}
}

void AutoRec_Reschedule()
{
	ar.nNextRun = ComputeNextRun(NowLocal());
}

// ------------------------------------------------------------------------------------
// Engine
// ------------------------------------------------------------------------------------

BOOL AutoRec_IsRunning()
{
	return ar.nState != AR_IDLE;
}

static BOOL SourceTakesDVBCParameters()
{
	char szDescription[256] = {0};
	char szParameters[256] = {0};
	BOOL fCanBeStopped;
	int nMaxPIDs;
	DWORD dwCapabilities;

	if (GetDescription == NULL)
		return FALSE;
	GetDescription(szDescription, szParameters, &fCanBeStopped, &nMaxPIDs, &dwCapabilities);
	strlwr(szParameters);
	return strstr(szParameters, "sr") != NULL && strstr(szParameters, "qam") != NULL;
}

// Retunes the source quietly (no dialogs; a failed lock just leaves it stopped)
static BOOL Retune(HWND hWnd, int nFrequency, int nSymbolRate, int nQAMIndex, int nInverted, int nBandwidthIndex)
{
	char szParameters[128];

	RestartTSReader_Stop(hWnd);
	v->fRunning = FALSE;
	wsprintf(szParameters, "%d %d %d %d %d", nFrequency, nSymbolRate, nQAMIndex, nInverted, nBandwidthIndex);
	if (ParseSourceModuleCommandLine == NULL || ParseSourceModuleCommandLine(&v->ss, szParameters, TRUE) == FALSE)
		return FALSE;
	v->ss.fQuietMode = TRUE;
	v->fTuneFromControlServer = TRUE;
	RestartTSReader_Start(hWnd);		// posts WM_USER + 5: tune, then start when locked
	return TRUE;
}

static BOOL StartMuxRecording(HWND hWnd, const char * szFile)
{
	v->hRecordFile = CreateFile(szFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (v->hRecordFile == INVALID_HANDLE_VALUE)
		return FALSE;
	lstrcpyn(v->szRecordFile, szFile, sizeof(v->szRecordFile));
	lstrcpyn(v->szActualRecordFile, szFile, sizeof(v->szActualRecordFile));
	ar.fSavedSplit = v->fSplitRecord;
	v->fSplitRecord = FALSE;
	v->fRecordAllTS = TRUE;
	v->fRecordPIDMode = FALSE;
	v->fRecordDialogStreamOnly = FALSE;
	v->nRecordProgram = 0;
	v->nAutoRecordSeconds = 0;
	v->nAutoRecordSecondCounter = 0;
	v->dTotalRecorded = 0;
	v->dThisFileRecorded = 0;
	v->nPATRecordCounter = 0;
	CheckMenuItem(GetMenu(hWnd), IDC_SI_PARSER_RECORD_ALL, MF_CHECKED | MF_BYCOMMAND);
	EnableMenuItem(GetMenu(hWnd), IDC_SI_PARSER_RECORD, MF_DISABLED | MF_GRAYED | MF_BYCOMMAND);
	EnableOrDisableStreamMenuItems(hWnd, FALSE);
	SendMessage(v->hWndST, SB_SETICON, 4, (LPARAM)v->hStatusBarIcons[5]);
	v->fLiteMonitorThreadActive = FALSE;
	v->dwRecordTickCounter = GetTickCount();
	GetRecordStartTime();
	v->fRecording = TRUE;
	ar.fRecordingStarted = TRUE;
	return TRUE;
}

static void StopMuxRecording(HWND hWnd)
{
	if (!ar.fRecordingStarted)
		return;
	if (v->fRecording && v->fRecordAllTS)
		RecordStream(hWnd, TRUE, -1);		// TSReader's own stop: closes the file and menus
	v->fSplitRecord = ar.fSavedSplit;
	ar.fRecordingStarted = FALSE;
}

static void EnterState(int nState)
{
	ar.nState = nState;
	ar.dwStateStart = GetTickCount();
}

static DWORD SecondsInState()
{
	return (GetTickCount() - ar.dwStateStart) / 1000;
}

static void FinishPass(HWND hWnd, const char * szReason)
{
	char szTime[32];

	StopMuxRecording(hWnd);
	FormatFileTime(NowLocal(), szTime);
	_snprintf(ar.szLastResult, sizeof(ar.szLastResult) - 1, "%s: %d recorded, %d no lock, %d skipped%s%s",
			  szTime, ar.nRecorded, ar.nNoLock, ar.nSkipped, szReason ? " - " : "", szReason ? szReason : "");
	ar.szLastResult[sizeof(ar.szLastResult) - 1] = '\0';
	SetStatus("Auto TS Recorder: %s", ar.szLastResult);
	CheckMenuItem(GetMenu(hWnd), ID_RECORD_AUTORECORDER_RUN, MF_UNCHECKED | MF_BYCOMMAND);

	if (v->fAutoRecRestore && ar.fHaveSavedTune)
	{
		Retune(hWnd, ar.nSavedFrequency, ar.nSavedSymbolRate, ar.nSavedQAM, ar.fSavedInversion, ar.nSavedBandwidth);
		EnterState(AR_WAIT_RESTORE);
	}
	else
		EnterState(AR_IDLE);
}

// Starts a pass; returns FALSE (with szWhy filled) when it can't run now
BOOL AutoRec_Start(HWND hWnd, char * szWhy)
{
	AUTORECMUX all[AUTOREC_MAX_MUXES];
	int nAll, i;

	szWhy[0] = '\0';
	if (ar.nState != AR_IDLE)
	{
		lstrcpy(szWhy, "A pass is already running.");
		return FALSE;
	}
	if (!SourceTakesDVBCParameters())
	{
		lstrcpy(szWhy, "The current source does not take DVB-C tuning parameters (freq sr QAM ...). Select a DVB-C source such as the DekTec DTU-236 QAM.");
		return FALSE;
	}
	if (!lstrlen(v->szAutoRecFolder) || GetFileAttributes(v->szAutoRecFolder) == INVALID_FILE_ATTRIBUTES)
	{
		lstrcpy(szWhy, "The recording folder is not set or does not exist.");
		return FALSE;
	}
	if (v->fRecording)
	{
		if (v->fAutoRecSkipBusy)
		{
			lstrcpy(szWhy, "TSReader is recording or playing - pass skipped.");
			return FALSE;
		}
		if (v->fStradisActive)
			StreamDecoder(hWnd);			// stop playback
		else
			RecordStream(hWnd, v->fRecordAllTS, -1);
	}

	nAll = LoadPlan(v->szAutoRecPlan, all, AUTOREC_MAX_MUXES);
	ar.nMuxes = 0;
	for (i = 0; i < nAll; i++)
	{
		if (IsMuxSelected(all[i].nFrequency))
			ar.muxes[ar.nMuxes++] = all[i];
	}
	if (ar.nMuxes == 0)
	{
		lstrcpy(szWhy, "No muxes selected (check the channel plan and the mux list).");
		return FALSE;
	}

	ar.fHaveSavedTune = v->ss.nFrequency != 0;
	ar.nSavedFrequency = v->ss.nFrequency;
	ar.nSavedSymbolRate = v->ss.nSymbolRate;
	ar.nSavedQAM = v->ss.nQAM;
	ar.fSavedInversion = v->ss.fSpectrumInversion;
	ar.nSavedBandwidth = v->ss.nBandwidth;
	ar.nCurrent = 0;
	ar.nRecorded = ar.nNoLock = ar.nSkipped = 0;
	ar.nAttempt = 0;
	CheckMenuItem(GetMenu(hWnd), ID_RECORD_AUTORECORDER_RUN, MF_CHECKED | MF_BYCOMMAND);
	EnterState(AR_TUNE);
	return TRUE;
}

void AutoRec_Stop(HWND hWnd)
{
	if (ar.nState != AR_IDLE && ar.nState != AR_WAIT_RESTORE)
		FinishPass(hWnd, "stopped");
}

static void CALLBACK AutoRecTimerProc(HWND hWnd, UINT uMsg, UINT_PTR idEvent, DWORD dwTime)
{
	AUTORECMUX * pMux = ar.nCurrent < ar.nMuxes ? &ar.muxes[ar.nCurrent] : NULL;
	char szFrequency[32] = "";

	if (pMux)
		FormatFrequency(pMux->nFrequency, szFrequency);

	switch (ar.nState)
	{
	case AR_IDLE:
		// command line -autorecord: one pass as soon as the source runs, then close
		if (ar.fExitWhenDone && !ar.fStartupPassDone)
		{
			if (v->fRunning)
			{
				char szWhy[256];
				ar.fStartupPassDone = TRUE;
				if (!AutoRec_Start(hWnd, szWhy))
				{
					lstrcpy(ar.szLastResult, szWhy);
					PostMessage(hWnd, WM_CLOSE, 0, 0);
				}
			}
			break;
		}
		if (ar.fExitWhenDone && ar.fStartupPassDone)
		{
			PostMessage(hWnd, WM_CLOSE, 0, 0);
			ar.fExitWhenDone = FALSE;
			break;
		}
		// schedule (not while the settings window is open: it would run inside its modal loop)
		if (v->fAutoRecEnabled && !fDialogOpen)
		{
			__int64 nNow = NowLocal();
			if (ar.nNextRun == 0)
				ar.nNextRun = ComputeNextRun(nNow);
			else if (nNow >= ar.nNextRun)
			{
				char szWhy[256];
				ar.nNextRun = ComputeNextRun(nNow);
				if (!AutoRec_Start(hWnd, szWhy))
				{
					char szTime[32];
					FormatFileTime(nNow, szTime);
					_snprintf(ar.szLastResult, sizeof(ar.szLastResult) - 1, "%s: not run - %s", szTime, szWhy);
					SetStatus("Auto TS Recorder: %s", ar.szLastResult);
				}
			}
		}
		break;

	case AR_TUNE:
		if (pMux == NULL)
		{
			FinishPass(hWnd, NULL);
			break;
		}
		ar.nAttempt++;
		SetStatus("Auto TS Recorder: mux %d/%d - tuning %s MHz (attempt %d)", ar.nCurrent + 1, ar.nMuxes, szFrequency, ar.nAttempt);
		if (Retune(hWnd, pMux->nFrequency, pMux->nSymbolRate, pMux->nQAMIndex, pMux->nInverted, pMux->nBandwidthIndex))
			EnterState(AR_WAIT_LOCK);
		else
		{
			WriteLog(pMux, "TUNE ERROR", 0, NULL, 0.0, 0, 0, NULL, 0.0);
			ar.nSkipped++;
			EnterState(AR_NEXT);
		}
		break;

	case AR_WAIT_LOCK:
		if (v->fRunning)
		{
			SetStatus("Auto TS Recorder: mux %d/%d - %s MHz locked, stabilizing %d s", ar.nCurrent + 1, ar.nMuxes, szFrequency, v->nAutoRecStabilize);
			EnterState(AR_STABILIZE);
		}
		else if ((!v->fTuneFromControlServer && SecondsInState() >= 1) || SecondsInState() >= (DWORD)max(v->nAutoRecLockTimeout, 1))
		{
			// tune finished without lock, or timed out
			v->fTuneFromControlServer = FALSE;
			if (ar.nAttempt <= v->nAutoRecRetries)
				EnterState(AR_TUNE);
			else
			{
				char szSignal[512] = "";
				if (GetSignalString)
					GetSignalString(szSignal);
				WriteLog(pMux, "NO LOCK", 0, szSignal, 0.0, 0, 0, NULL, 0.0);
				ar.nNoLock++;
				EnterState(AR_NEXT);
			}
		}
		break;

	case AR_STABILIZE:
		if (SecondsInState() >= (DWORD)v->nAutoRecStabilize)
		{
			double dFree = FreeSpaceGB();
			int nTSID = v->pat.nTransportStreamID;

			if (dFree >= 0 && dFree < (double)v->nAutoRecMinFreeGB)
			{
				char szSignal[512] = "";
				if (GetSignalString)
					GetSignalString(szSignal);
				WriteLog(pMux, "SKIPPED LOW DISK", nTSID, szSignal, CurrentBitrateMbps(), v->nContinuityErrors, CountServices(), NULL, 0.0);
				ar.nSkipped++;
				EnterState(AR_NEXT);
				break;
			}
			BuildFileName(pMux, nTSID, ar.szFile);
			if (!StartMuxRecording(hWnd, ar.szFile))
			{
				WriteLog(pMux, "FILE ERROR", nTSID, NULL, 0.0, 0, 0, ar.szFile, 0.0);
				ar.nSkipped++;
				EnterState(AR_NEXT);
				break;
			}
			v->nContinuityErrors = 0;	// count errors during the recording only
			SetStatus("Auto TS Recorder: mux %d/%d - recording %s MHz for %d s", ar.nCurrent + 1, ar.nMuxes, szFrequency, v->nAutoRecLength);
			EnterState(AR_RECORDING);
		}
		break;

	case AR_RECORDING:
		if (!v->fRecording)
		{
			// recording ended elsewhere (file error, user)
			WriteLog(pMux, "RECORDING STOPPED", v->pat.nTransportStreamID, NULL, 0.0, 0, 0, ar.szFile, 0.0);
			ar.fRecordingStarted = FALSE;
			v->fSplitRecord = ar.fSavedSplit;
			ar.nSkipped++;
			EnterState(AR_NEXT);
		}
		else if (SecondsInState() >= (DWORD)max(v->nAutoRecLength, 1))
		{
			char szSignal[512] = "";
			double dBitrate = CurrentBitrateMbps();
			int nCCErrors = v->nContinuityErrors;
			int nServices = CountServices();
			int nTSID = v->pat.nTransportStreamID;
			WIN32_FILE_ATTRIBUTE_DATA fad;
			double dSizeMB = 0.0;

			if (GetSignalString)
				GetSignalString(szSignal);
			StopMuxRecording(hWnd);
			if (GetFileAttributesEx(ar.szFile, GetFileExInfoStandard, &fad))
				dSizeMB = (double)(((__int64)fad.nFileSizeHigh << 32) | fad.nFileSizeLow) / (1024.0 * 1024.0);
			WriteLog(pMux, "OK", nTSID, szSignal, dBitrate, nCCErrors, nServices, ar.szFile, dSizeMB);
			ar.nRecorded++;
			ApplyRetention(pMux);
			EnterState(AR_NEXT);
		}
		break;

	case AR_NEXT:
		ar.nCurrent++;
		ar.nAttempt = 0;
		EnterState(ar.nCurrent < ar.nMuxes ? AR_TUNE : AR_IDLE);
		if (ar.nCurrent >= ar.nMuxes)
			FinishPass(hWnd, NULL);
		break;

	case AR_WAIT_RESTORE:
		if (v->fRunning || !v->fTuneFromControlServer || SecondsInState() > 15)
		{
			v->fTuneFromControlServer = FALSE;
			v->ss.fQuietMode = FALSE;
			EnterState(AR_IDLE);
		}
		break;
	}
}

// Called from the main window's WM_CREATE
void AutoRec_Init(HWND hWnd)
{
	memset(&ar, 0, sizeof(ar));
	if (v->nAutoRecWeekdays == 0 && !lstrlen(v->szAutoRecFolder))
	{
		// first use: sensible defaults
		v->nAutoRecWeekdays = 0x7f;
		v->nAutoRecMode = 1;
		v->nAutoRecInterval = 6;
		lstrcpy(v->szAutoRecTimes, "06:00,18:00");
		lstrcpy(v->szAutoRecPlan, "Finland (114 MHz +)");
		v->nAutoRecStabilize = 5;
		v->nAutoRecLockTimeout = 10;
		v->nAutoRecRetries = 1;
		v->nAutoRecLength = 30;
		v->nAutoRecKeep = 5;
		v->nAutoRecMinFreeGB = 5;
		v->fAutoRecCSV = TRUE;
		v->fAutoRecSkipBusy = TRUE;
		v->fAutoRecRestore = TRUE;
		// user profile root rather than Documents, which is often synced to OneDrive
		SHGetSpecialFolderPath(NULL, v->szAutoRecFolder, CSIDL_PROFILE, FALSE);
		lstrcat(v->szAutoRecFolder, "\\TSReader AutoRecord");
	}
	if (v->fAutoRecAtStart)
		ar.fExitWhenDone = TRUE;
	AutoRec_Reschedule();
	SetTimer(hWnd, AUTOREC_TIMER_ID, 1000, AutoRecTimerProc);
}

// -autorecord with no source parameters: start on the first selected mux of the plan
char * AutoRec_GetStartupParameters()
{
	static char szParameters[128];
	AUTORECMUX all[AUTOREC_MAX_MUXES];
	int nAll = LoadPlan(v->szAutoRecPlan, all, AUTOREC_MAX_MUXES), i;

	szParameters[0] = '\0';
	for (i = 0; i < nAll; i++)
	{
		if (IsMuxSelected(all[i].nFrequency))
		{
			wsprintf(szParameters, "%d %d %d %d %d", all[i].nFrequency, all[i].nSymbolRate,
					 all[i].nQAMIndex, all[i].nInverted, all[i].nBandwidthIndex);
			break;
		}
	}
	return szParameters;
}

// ------------------------------------------------------------------------------------
// Settings dialog
// ------------------------------------------------------------------------------------

static AUTORECMUX dlgMuxes[AUTOREC_MAX_MUXES];
static int nDlgMuxes;

static void FillMuxList(HWND hDlg, BOOL fUseSavedSelection)
{
	HWND hList = GetDlgItem(hDlg, IDC_AR_MUXES);
	char szPlan[MAX_PATH];
	int i, nIndex = (int)SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_GETCURSEL, 0, 0);

	ListView_DeleteAllItems(hList);
	nDlgMuxes = 0;
	if (nIndex == CB_ERR || SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_GETLBTEXT, nIndex, (LPARAM)szPlan) == CB_ERR)
		return;
	nDlgMuxes = LoadPlan(szPlan, dlgMuxes, AUTOREC_MAX_MUXES);
	for (i = 0; i < nDlgMuxes; i++)
	{
		LVITEM lvi;
		char szFrequency[32];
		char szText[128];

		FormatFrequency(dlgMuxes[i].nFrequency, szFrequency);
		wsprintf(szText, "%s MHz", szFrequency);
		memset(&lvi, 0, sizeof(lvi));
		lvi.mask = LVIF_TEXT;
		lvi.iItem = i;
		lvi.pszText = szText;
		ListView_InsertItem(hList, &lvi);
		ListView_SetItemText(hList, i, 1, dlgMuxes[i].szLabel);
		ListView_SetCheckState(hList, i, fUseSavedSelection ? IsMuxSelected(dlgMuxes[i].nFrequency) : TRUE);
	}
}

static void UpdateFileNamePreview(HWND hDlg)
{
	char szPrefix[64], szSuffix[64], szName[256];

	GetDlgItemText(hDlg, IDC_AR_PREFIX, szPrefix, sizeof(szPrefix));
	GetDlgItemText(hDlg, IDC_AR_SUFFIX, szSuffix, sizeof(szSuffix));
	wsprintf(szName, "Example: %s%s258MHz_TS14%s%s_20261023_171500.ts",
			 szPrefix, lstrlen(szPrefix) ? "_" : "", lstrlen(szSuffix) ? "_" : "", szSuffix);
	SetDlgItemText(hDlg, IDC_AR_PREVIEW, szName);
}

static void UpdateStatusText(HWND hDlg)
{
	char szNext[32], szTemp[512];

	FormatFileTime(ar.nNextRun, szNext);
	_snprintf(szTemp, sizeof(szTemp) - 1, "%s   Next run: %s\r\nLast pass: %s",
			  ar.nState != AR_IDLE ? "RUNNING" : "Idle", v->fAutoRecEnabled ? szNext : "schedule off",
			  lstrlen(ar.szLastResult) ? ar.szLastResult : "-");
	szTemp[sizeof(szTemp) - 1] = '\0';
	SetDlgItemText(hDlg, IDC_AR_STATUS, szTemp);
}

static void EnableModeFields(HWND hDlg)
{
	EnableWindow(GetDlgItem(hDlg, IDC_AR_MINUTES), IsDlgButtonChecked(hDlg, IDC_AR_MODE_MINUTES));
	EnableWindow(GetDlgItem(hDlg, IDC_AR_HOURS), IsDlgButtonChecked(hDlg, IDC_AR_MODE_HOURS));
	EnableWindow(GetDlgItem(hDlg, IDC_AR_TIMES), IsDlgButtonChecked(hDlg, IDC_AR_MODE_TIMES));
}

static BOOL SaveDialog(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_AR_MUXES);
	int i, nChecked = 0, nPlan;
	char szList[1024] = "";

	v->fAutoRecEnabled = IsDlgButtonChecked(hDlg, IDC_AR_ENABLED);
	v->nAutoRecMode = IsDlgButtonChecked(hDlg, IDC_AR_MODE_TIMES) ? 2 : IsDlgButtonChecked(hDlg, IDC_AR_MODE_HOURS) ? 1 : 0;
	v->nAutoRecInterval = GetDlgItemInt(hDlg, v->nAutoRecMode == 1 ? IDC_AR_HOURS : IDC_AR_MINUTES, NULL, FALSE);
	if (v->nAutoRecInterval < 1)
		v->nAutoRecInterval = 1;
	GetDlgItemText(hDlg, IDC_AR_TIMES, v->szAutoRecTimes, sizeof(v->szAutoRecTimes));
	v->nAutoRecWeekdays = 0;
	for (i = 0; i < 7; i++)
	{
		if (IsDlgButtonChecked(hDlg, IDC_AR_MON + i))
			v->nAutoRecWeekdays |= 1 << i;
	}
	nPlan = (int)SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_GETCURSEL, 0, 0);
	if (nPlan != CB_ERR)
		SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_GETLBTEXT, nPlan, (LPARAM)v->szAutoRecPlan);
	for (i = 0; i < nDlgMuxes; i++)
	{
		if (ListView_GetCheckState(hList, i))
		{
			char szItem[16];
			nChecked++;
			wsprintf(szItem, "%s%d", lstrlen(szList) ? "," : "", dlgMuxes[i].nFrequency);
			if (lstrlen(szList) + lstrlen(szItem) < (int)sizeof(szList) - 1)
				lstrcat(szList, szItem);
		}
	}
	if (nDlgMuxes && nChecked == 0)
	{
		MessageBox(hDlg, "Select at least one mux to record.", gszAppName, MB_ICONWARNING);
		return FALSE;
	}
	lstrcpy(v->szAutoRecMuxes, nChecked == nDlgMuxes ? "" : szList);	// "" = every mux of the plan
	v->nAutoRecStabilize = GetDlgItemInt(hDlg, IDC_AR_STABILIZE, NULL, FALSE);
	v->nAutoRecLockTimeout = GetDlgItemInt(hDlg, IDC_AR_LOCKTIMEOUT, NULL, FALSE);
	v->nAutoRecRetries = GetDlgItemInt(hDlg, IDC_AR_RETRIES, NULL, FALSE);
	v->nAutoRecLength = GetDlgItemInt(hDlg, IDC_AR_LENGTH, NULL, FALSE);
	GetDlgItemText(hDlg, IDC_AR_FOLDER, v->szAutoRecFolder, sizeof(v->szAutoRecFolder));
	GetDlgItemText(hDlg, IDC_AR_PREFIX, v->szAutoRecPrefix, sizeof(v->szAutoRecPrefix));
	GetDlgItemText(hDlg, IDC_AR_SUFFIX, v->szAutoRecSuffix, sizeof(v->szAutoRecSuffix));
	v->nAutoRecKeep = GetDlgItemInt(hDlg, IDC_AR_KEEP, NULL, FALSE);
	v->nAutoRecMaxAgeDays = GetDlgItemInt(hDlg, IDC_AR_MAXAGE, NULL, FALSE);
	v->nAutoRecMinFreeGB = GetDlgItemInt(hDlg, IDC_AR_MINFREE, NULL, FALSE);
	v->fAutoRecCSV = IsDlgButtonChecked(hDlg, IDC_AR_CSV);
	v->fAutoRecSkipBusy = IsDlgButtonChecked(hDlg, IDC_AR_SKIPBUSY);
	v->fAutoRecRestore = IsDlgButtonChecked(hDlg, IDC_AR_RESTORE);
	if (v->nAutoRecLength < 1)
		v->nAutoRecLength = 1;
	if (lstrlen(v->szAutoRecFolder))
		SHCreateDirectoryExA(NULL, v->szAutoRecFolder, NULL);
	AutoRec_Reschedule();
	SaveSettings();
	return TRUE;
}

BOOL CALLBACK AutoRecordDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			HWND hList = GetDlgItem(hDlg, IDC_AR_MUXES);

			fDialogOpen = TRUE;
			LVCOLUMN lvc;
			char szPath[MAX_PATH];
			WIN32_FIND_DATA fd;
			HANDLE hFind;
			int i, nIndex;

			CheckDlgButton(hDlg, IDC_AR_ENABLED, v->fAutoRecEnabled);
			CheckDlgButton(hDlg, IDC_AR_MODE_MINUTES, v->nAutoRecMode == 0);
			CheckDlgButton(hDlg, IDC_AR_MODE_HOURS, v->nAutoRecMode == 1);
			CheckDlgButton(hDlg, IDC_AR_MODE_TIMES, v->nAutoRecMode == 2);
			SetDlgItemInt(hDlg, IDC_AR_MINUTES, v->nAutoRecMode == 0 ? v->nAutoRecInterval : 30, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_HOURS, v->nAutoRecMode == 1 ? v->nAutoRecInterval : 6, FALSE);
			SetDlgItemText(hDlg, IDC_AR_TIMES, v->szAutoRecTimes);
			for (i = 0; i < 7; i++)
				CheckDlgButton(hDlg, IDC_AR_MON + i, (v->nAutoRecWeekdays >> i) & 1);
			EnableModeFields(hDlg);

			memset(&lvc, 0, sizeof(lvc));
			lvc.mask = LVCF_TEXT | LVCF_WIDTH;
			lvc.cx = 90;
			lvc.pszText = "Frequency";
			ListView_InsertColumn(hList, 0, &lvc);
			lvc.cx = 170;
			lvc.pszText = "Name";
			ListView_InsertColumn(hList, 1, &lvc);
			ListView_SetExtendedListViewStyle(hList, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);

			GetPlanFolder(szPath);
			lstrcat(szPath, "\\*.lst");
			hFind = FindFirstFile(szPath, &fd);
			if (hFind != INVALID_HANDLE_VALUE)
			{
				do
				{
					char * pExtension = strrchr(fd.cFileName, '.');
					if (pExtension)
						*pExtension = '\0';
					SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_ADDSTRING, 0, (LPARAM)fd.cFileName);
				} while (FindNextFile(hFind, &fd));
				FindClose(hFind);
			}
			nIndex = (int)SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)v->szAutoRecPlan);
			SendDlgItemMessage(hDlg, IDC_AR_PLAN, CB_SETCURSEL, nIndex == CB_ERR ? 0 : nIndex, 0);
			FillMuxList(hDlg, TRUE);

			SetDlgItemInt(hDlg, IDC_AR_STABILIZE, v->nAutoRecStabilize, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_LOCKTIMEOUT, v->nAutoRecLockTimeout, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_RETRIES, v->nAutoRecRetries, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_LENGTH, v->nAutoRecLength, FALSE);
			SetDlgItemText(hDlg, IDC_AR_FOLDER, v->szAutoRecFolder);
			SetDlgItemText(hDlg, IDC_AR_PREFIX, v->szAutoRecPrefix);
			SetDlgItemText(hDlg, IDC_AR_SUFFIX, v->szAutoRecSuffix);
			SetDlgItemInt(hDlg, IDC_AR_KEEP, v->nAutoRecKeep, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_MAXAGE, v->nAutoRecMaxAgeDays, FALSE);
			SetDlgItemInt(hDlg, IDC_AR_MINFREE, v->nAutoRecMinFreeGB, FALSE);
			CheckDlgButton(hDlg, IDC_AR_CSV, v->fAutoRecCSV);
			CheckDlgButton(hDlg, IDC_AR_SKIPBUSY, v->fAutoRecSkipBusy);
			CheckDlgButton(hDlg, IDC_AR_RESTORE, v->fAutoRecRestore);
			UpdateFileNamePreview(hDlg);
			UpdateStatusText(hDlg);
			EnableWindow(GetDlgItem(hDlg, IDC_AR_STOP), ar.nState != AR_IDLE);
		}
		return TRUE;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_AR_MODE_MINUTES:
		case IDC_AR_MODE_HOURS:
		case IDC_AR_MODE_TIMES:
			EnableModeFields(hDlg);
			break;
		case IDC_AR_PLAN:
			if (HIWORD(wParam) == CBN_SELCHANGE)
				FillMuxList(hDlg, FALSE);
			break;
		case IDC_AR_ALL:
		case IDC_AR_NONE:
			{
				int i;
				for (i = 0; i < nDlgMuxes; i++)
					ListView_SetCheckState(GetDlgItem(hDlg, IDC_AR_MUXES), i, LOWORD(wParam) == IDC_AR_ALL);
			}
			break;
		case IDC_AR_PREFIX:
		case IDC_AR_SUFFIX:
			if (HIWORD(wParam) == EN_CHANGE)
				UpdateFileNamePreview(hDlg);
			break;
		case IDC_AR_BROWSE:
			{
				BROWSEINFO bi;
				LPITEMIDLIST pidl;
				char szFolder[MAX_PATH];

				memset(&bi, 0, sizeof(bi));
				bi.hwndOwner = hDlg;
				bi.pszDisplayName = szFolder;
				bi.lpszTitle = "Folder for the Auto TS Recorder recordings";
				bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
				pidl = SHBrowseForFolder(&bi);
				if (pidl != NULL)
				{
					if (SHGetPathFromIDList(pidl, szFolder))
						SetDlgItemText(hDlg, IDC_AR_FOLDER, szFolder);
					CoTaskMemFree(pidl);
				}
			}
			break;
		case IDC_AR_RUNNOW:
			if (SaveDialog(hDlg))
			{
				char szWhy[256];
				if (AutoRec_Start(v->hWndMainWindow, szWhy))
					EndDialog(hDlg, TRUE);
				else
				{
					MessageBox(hDlg, szWhy, gszAppName, MB_ICONWARNING);
					UpdateStatusText(hDlg);
				}
			}
			break;
		case IDC_AR_STOP:
			AutoRec_Stop(v->hWndMainWindow);
			UpdateStatusText(hDlg);
			EnableWindow(GetDlgItem(hDlg, IDC_AR_STOP), FALSE);
			break;
		case IDOK:
			if (SaveDialog(hDlg))
				EndDialog(hDlg, TRUE);
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		}
		break;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_DESTROY:
		fDialogOpen = FALSE;
		break;
	}
	return FALSE;
}
