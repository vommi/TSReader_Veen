// TSReader source for the DekTec DTU-236 (QAM measurement receiver + ASI input for USB)
//
// Built against the current DekTec WinSDK (DTAPI) rather than the 2005 DTAPI 3.0.6 used
// by TSReader_Dektec.cpp, so it works with the current Dtu driver (v4.x).
//
// Two DLLs are built from this file:
//   TSReader_DektecDTU236_QAM.dll - tunes the RF input (DVB-C / J.83 annex A)
//   TSReader_DektecDTU236_ASI.dll - receives from the ASI input (define DTU236_ASI)

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>

#include "..\sources.h"
// Angle brackets: this folder also holds the old DTAPI 3.0.6 header; we want the WinSDK one
#include <DTAPI.h>

using namespace Dtapi;

#define DTU236_TYPE_NUMBER 236

PSOURCESTRUCT ss;
HANDLE hInstance;

BOOL fNeedTuneDialog = TRUE;
#ifndef DTU236_ASI
int nFrequency;
int nSymbolRate;
int nQAM;
BOOL fSpectrumInversion;
int nBandwidth;
char gszSourceName[] = {"Dektec DTU-236 QAM (DVB-C)"};
#else DTU236_ASI
char gszSourceName[] = {"Dektec DTU-236 ASI"};
#endif DTU236_ASI

char szLastTune[128] = {"n/a"};
char szLastSignalReport[128] = {"n/a"};
CRITICAL_SECTION csSignal;
BOOL fCSSignalInitDone = FALSE;
int nFIFOOverrunCounter;

DtDevice theCard;
DtInpChannel theInput;
char g_szLastError[256];					// Last error message

extern "C" {
	BOOL __cdecl SourceHelper_ValidateSourceContainer(PSOURCESTRUCT pss);
}

void DebugTrace(const char* szError)
{
	lstrcpy(g_szLastError, "Dektec_DTU236: ");
	lstrcat(g_szLastError, szError);
	lstrcat(g_szLastError, "\n");
	OutputDebugString(g_szLastError);
}

void DebugTraceResult(const char* szWhat, DTAPI_RESULT dr)
{
	char szTemp[256];
	_snprintf(szTemp, sizeof(szTemp) - 1, "%s failed: %s (0x%04x)", szWhat, DtapiResult2Str(dr), dr);
	szTemp[sizeof(szTemp) - 1] = 0;
	DebugTrace(szTemp);
}

bool AttachToBoard(DtDevice& rCard, DtInpChannel& rInputCh)
{
	char szTemp[256];
	int i, nPort, nEntries = 0;
	int nSourceIndex = ss->nSourceIndex;
	DtDeviceDesc DvcDescArr[64];
	DTAPI_RESULT dr;

	dr = DtapiDeviceScan(64, nEntries, DvcDescArr);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("DtapiDeviceScan", dr);
		return false;
	}

	wsprintf(szTemp, "Devices from DtapiDeviceScan == %d", nEntries);
	DebugTrace(szTemp);
	for (i = 0; i < nEntries; i++)
	{
		wsprintf(szTemp, "Category: %d TypeNumber: %d", DvcDescArr[i].m_Category, DvcDescArr[i].m_TypeNumber);
		DebugTrace(szTemp);
		if (DvcDescArr[i].m_TypeNumber == DTU236_TYPE_NUMBER)
		{
			if (nSourceIndex == 0)
				break;
			nSourceIndex--;
		}
	}
	if (i == nEntries)
	{
		DebugTrace("Unable to locate a DTU-236");
		return false;
	}

	//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Attach to Device -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
	dr = rCard.AttachToSerial(DvcDescArr[i].m_Serial);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("AttachToSerial", dr);
		return false;
	}

	//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Find the wanted port -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-
	for (nPort = 1; nPort <= DvcDescArr[i].m_NumPorts; nPort++)
	{
#ifndef DTU236_ASI
		if (rCard.HasCaps(nPort, DTAPI_CAP_DEMOD))
			break;
#else DTU236_ASI
		if (rCard.HasCaps(nPort, DTAPI_CAP_ASI))
			break;
#endif DTU236_ASI
	}
	if (nPort > DvcDescArr[i].m_NumPorts)
	{
		DebugTrace("DTU-236 has no port with the required capability");
		rCard.Detach();
		return false;
	}
	wsprintf(szTemp, "Using port %d", nPort);
	DebugTrace(szTemp);

	// The DTU-236 has a single receive channel: only one of its ports (RF or ASI) can be
	// an input at a time. Disable the other port(s) first, then make ours the input.
	// The device keeps this setting, so it must be done every time.
	for (int nOtherPort = 1; nOtherPort <= DvcDescArr[i].m_NumPorts; nOtherPort++)
	{
		if (nOtherPort != nPort)
			rCard.SetIoConfig(nOtherPort, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_DISABLED);
	}
	dr = rCard.SetIoConfig(nPort, DTAPI_IOCONFIG_IODIR, DTAPI_IOCONFIG_INPUT, DTAPI_IOCONFIG_INPUT);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetIoConfig(IODIR=INPUT)", dr);
		rCard.Detach();
		return false;
	}
#ifdef DTU236_ASI
	rCard.SetIoConfig(nPort, DTAPI_IOCONFIG_IOSTD, DTAPI_IOCONFIG_ASI);
#endif DTU236_ASI

	//.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.- Attach to Channel -.-.-.-.-.-.-.-.-.-.-.-.-.-.-.-.
	// Exclusive access is required for SetRxMode()
	dr = rInputCh.AttachToPort(&rCard, nPort, true);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("AttachToPort", dr);
		rCard.Detach();
		return false;
	}
	return true;
}

#ifndef DTU236_ASI
void UpdateSignalReport()
{
	bool fLocked = false;
	double dMER = 0.0;
	int nRFLevel = 0;
	char szTemp[128];

	if (DTAPI_OK != theInput.GetStatistic(DTAPI_STAT_LOCK, fLocked) || !fLocked)
		lstrcpy(szTemp, "Unlocked");
	else
	{
		int nLen = sprintf(szTemp, "Locked");
		if (DTAPI_OK == theInput.GetStatistic(DTAPI_STAT_MER, dMER))
			nLen += sprintf(szTemp + nLen, " MER %.1f dB", dMER);
		if (DTAPI_OK == theInput.GetStatistic(DTAPI_STAT_RFLVL_CHAN, nRFLevel))
			nLen += sprintf(szTemp + nLen, " Level %.1f dBmV", nRFLevel / 10.0);
	}
	EnterCriticalSection(&csSignal);
	lstrcpy(szLastSignalReport, szTemp);
	LeaveCriticalSection(&csSignal);
}
#endif DTU236_ASI

DWORD WINAPI DektecReceiveThread(LPVOID pPtr)
{
	int nFIFOCheckCounter = 0;
	int nSignalCheckCounter = 0;
	int nMaxFIFOSize = 0;
	DTAPI_RESULT dr;
	BYTE * pBuffer;

	DebugTrace("ReceiveThread+");

	theInput.GetMaxFifoSize(nMaxFIFOSize);
	pBuffer = (BYTE *)LocalAlloc(LPTR, nMaxFIFOSize);

	SourceHelper_StartSyncThread(ss, FALSE);

	dr = theInput.SetRxControl(DTAPI_RXCTRL_RCV);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetRxControl(RCV)", dr);
	}
	else
	{
		while (!ss->fTerminateReadThread)
		{
			int nFIFOLoad = 0;

#ifndef DTU236_ASI
			// Refresh lock/MER/level roughly once a second
			if (nSignalCheckCounter++ > 500)
			{
				UpdateSignalReport();
				nSignalCheckCounter = 0;
			}
#endif DTU236_ASI

			theInput.GetFifoLoad(nFIFOLoad);
			if (nFIFOLoad < 64 * 1024)
			{
				Sleep(1);
				continue;
			}
			nFIFOLoad &= ~3;	// ensure divisible by 4
			if (nFIFOLoad > nMaxFIFOSize)
				nFIFOLoad = nMaxFIFOSize & ~3;
			dr = theInput.Read((char *)pBuffer, nFIFOLoad);
			if (DTAPI_OK != dr)
			{
				DebugTraceResult("Read", dr);
				continue;
			}
			SourceHelper_SyncData(pBuffer, nFIFOLoad);

			// Check for FIFO overrun
			if (nFIFOCheckCounter++ > 256)
			{
				int nStatus = 0;
				int nLatched = 0;
				theInput.GetFlags(nStatus, nLatched);
				if (0 != (DTAPI_RX_FIFO_OVF & nLatched))
				{
					nFIFOOverrunCounter++;
					theInput.ClearFlags(DTAPI_RX_FIFO_OVF);
					DebugTrace("FIFO overrun has occured");
				}
				nFIFOCheckCounter = 0;
			}
		}
	}

	LocalFree(pBuffer);
	theInput.SetRxControl(DTAPI_RXCTRL_IDLE);
	SourceHelper_StopSyncThread();

	ss->fTerminateReadThread = FALSE;
	EnterCriticalSection(&ss->csTSBuffersInUse);
	ss->nTSBuffersInUse = -1000;
	LeaveCriticalSection(&ss->csTSBuffersInUse);

	ss->fReadThreadTerminated = TRUE;
	CloseHandle(ss->hReadDataThread);
	DebugTrace("ReceiveThread-");

	return 0;
}

extern "C" {

BOOL TSReader_Init(PSOURCESTRUCT pss)
{
	DTAPI_RESULT dr;

	if (SourceHelper_ValidateSourceContainer(pss) == FALSE)
		return FALSE;

	ss = pss;

	if (!fCSSignalInitDone)
	{
		InitializeCriticalSection(&csSignal);
		fCSSignalInitDone = TRUE;
	}

	// TSReader can unload a source without calling TSReader_DeInit (e.g. when the tune
	// dialog is cancelled) and later load and Init it again. Drop any stale attachment.
	if (theInput.IsAttached())
		theInput.Detach(DTAPI_INSTANT_DETACH);
	if (theCard.IsAttached())
		theCard.Detach();

	if (!AttachToBoard(theCard, theInput))
	{
		DebugTrace("AttachToBoard() failed");
		return FALSE;
	}

	// Unloading DTAPI while a device is attached deadlocks inside FreeLibrary (DTAPI's
	// global objects wait for their worker threads under the loader lock), and TSReader
	// does unload sources without DeInit. So once attached, keep this DLL loaded for the
	// rest of the process.
	{
		HMODULE hSelf;
		GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
						  (LPCSTR)&TSReader_Init, &hSelf);
	}

	dr = theInput.SetRxMode(DTAPI_RXMODE_ST188);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetRxMode", dr);
		return FALSE;
	}

	// Do not start receiving yet; the receive thread does that
	dr = theInput.SetRxControl(DTAPI_RXCTRL_IDLE);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetRxControl(IDLE)", dr);
		return FALSE;
	}

	dr = theInput.ClearFifo();
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("ClearFifo", dr);
		return FALSE;
	}

	return TRUE;
}

BOOL TSReader_DeInit()
{
	// Detach from input channel and then from the device
	theInput.Detach(DTAPI_INSTANT_DETACH);
	theCard.Detach();
	// csSignal is kept: the DLL stays loaded and Init may be called again
	return TRUE;
}

BOOL TSReader_Start()
{
	DWORD dwThreadID;

	nFIFOOverrunCounter = 0;

	ss->hReadDataThread = CreateThread(NULL, 0, DektecReceiveThread, (LPVOID)0, CREATE_SUSPENDED, &dwThreadID);
	SourceHelper_SetWorkerThreadPriorities(FALSE);
	ResumeThread(ss->hReadDataThread);
	return TRUE;
}

BOOL TSReader_Stop()
{
	DebugTrace("enter Stop()");

	ss->fTerminateReadThread = TRUE;
	while (ss->fReadThreadTerminated == FALSE)
		Sleep(50);

	DebugTrace("leave Stop()");
	return TRUE;
}

BOOL TSReader_Tune()
{
#ifndef DTU236_ASI
	static const char * szQAMNames[] = {"QAM-16", "QAM-32", "QAM-64", "QAM-128", "QAM-256"};
	int nQAMIndex = ss->nQAM;
	DTAPI_RESULT dr;
	DWORD dwTimeout;
	bool fLocked = false;

	if (ss->fDontTune)
		return TRUE;

	if (nQAMIndex < 0 || nQAMIndex > 4)
		nQAMIndex = 4;

	// ss->nFrequency is in KHz and ss->nSymbolRate in KS/s for DVB-C
	sprintf(szLastTune, "%.3f MHz %d KS/s %s", ss->nFrequency / 1000.0, ss->nSymbolRate, szQAMNames[nQAMIndex]);

	dr = theInput.SetDemodControl(DTAPI_MOD_QAM16 + nQAMIndex, DTAPI_MOD_J83_A, 0, ss->nSymbolRate * 1000);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetDemodControl", dr);
		if (ss->fQuietMode == FALSE)
		{
			char szTemp[256];
			wsprintf(szTemp, "Unable to set demodulator parameters:\n%s", DtapiResult2Str(dr));
			MessageBox(ss->hWndTSReader, szTemp, gszSourceName, MB_ICONWARNING);
		}
		return FALSE;
	}

	dr = theInput.SetTunerFrequency((__int64)ss->nFrequency * 1000);
	if (DTAPI_OK != dr)
	{
		DebugTraceResult("SetTunerFrequency", dr);
		if (ss->fQuietMode == FALSE)
		{
			char szTemp[256];
			wsprintf(szTemp, "Unable to tune to %d KHz:\n%s", ss->nFrequency, DtapiResult2Str(dr));
			MessageBox(ss->hWndTSReader, szTemp, gszSourceName, MB_ICONWARNING);
		}
		return FALSE;
	}

	// Spectrum inversion is detected automatically by most demodulators; only force it if asked
	if (ss->fSpectrumInversion)
		theInput.SetSpectrumInversion(true);

	dwTimeout = GetTickCount();
	while (GetTickCount() - dwTimeout < 3000)
	{
		if (DTAPI_OK == theInput.GetStatistic(DTAPI_STAT_LOCK, fLocked) && fLocked)
		{
			UpdateSignalReport();
			return TRUE;
		}
		Sleep(100);
	}
	UpdateSignalReport();
	if (ss->fQuietMode == FALSE)
		MessageBox(ss->hWndTSReader, "Failed to lock signal", gszSourceName, MB_ICONWARNING);
	return FALSE;
#else DTU236_ASI
	return TRUE;
#endif DTU236_ASI
}

BOOL TSReader_TuneDialog(HWND hWnd)
{
#ifndef DTU236_ASI
	DebugTrace("TSReader_TuneDialog");

	if (ss->fDontTune == TRUE)
		fNeedTuneDialog = FALSE;

	if (fNeedTuneDialog)
	{
		if (SourceHelper_DVBCTuneDialog(hWnd) == FALSE)
			return FALSE;
	}
	else
	{
		ss->nFrequency = nFrequency;
		ss->nSymbolRate = nSymbolRate;
		ss->nQAM = nQAM;
		ss->fSpectrumInversion = fSpectrumInversion;
		ss->nBandwidth = nBandwidth;
		fNeedTuneDialog = TRUE;
	}
#endif DTU236_ASI
	return TRUE;
}

BOOL TSReader_PIDManagement(BOOL fAdd, int nPID, BOOL fTemporary)
{
	return TRUE;
}

BOOL TSReader_GetDescription(char * szDescription, char * szCommandLineParameters, BOOL * fCanBeStopped, int * nMaxPIDs, DWORD * dwCapabilities)
{
	if (szDescription != NULL)
		lstrcpy(szDescription, gszSourceName);
	if (fCanBeStopped != NULL)
		*fCanBeStopped = FALSE;
	if (nMaxPIDs != NULL)
		*nMaxPIDs = 8192;
	if (szCommandLineParameters != NULL)
#ifndef DTU236_ASI
		lstrcpy(szCommandLineParameters, "freq sr QAM inversion bandwidth");
#else DTU236_ASI
		lstrcpy(szCommandLineParameters, "None");
#endif DTU236_ASI
	if (dwCapabilities != NULL)
		*dwCapabilities = CAPABILITIES_MULTICARD;

	return TRUE;
}

BOOL TSReader_ParseCommandLine(PSOURCESTRUCT ss, char * szCommandLine, BOOL fQuiet)
{
#ifndef DTU236_ASI
	if (lstrlen(szCommandLine))
	{
		int nConversionCount;

		nConversionCount = sscanf(szCommandLine,
								  "%d %d %d %d %d",
								  &nFrequency,
								  &nSymbolRate,
								  &nQAM,
								  &fSpectrumInversion,
								  &nBandwidth);
		if (nConversionCount < 5)
		{
			if (!fQuiet)
				MessageBox(NULL,
					   "Usage for this source: freq sr QAM inversion bandwidth\n"
					   "\n"
					   "freq = frequency to tune in KHz\n"
					   "sr = symbol rate in KS/s\n"
					   "QAM = QAM Mode (0=QAM-16 1=QAM-32 2=QAM-64 3=QAM-128 4=QAM-256)\n"
					   "inversion = inverted spectrum (0 or 1)\n"
					   "bandwidth = bandwidth of signal (0 = 6, 1 = 7, 2 = 8 MHz)",
					   gszSourceName,
					   MB_OK | MB_ICONSTOP);
			return FALSE;
		}
		fNeedTuneDialog = FALSE;
	}
	else
		fNeedTuneDialog = TRUE;
#endif DTU236_ASI
	return TRUE;
}

BOOL TSReader_IsPIDActive(int nPID)
{
	return TRUE;
}

BOOL TSReader_SetChannel(int nChannel)
{
	return TRUE;
}

BOOL TSReader_GetSignalString(char * szString)
{
#ifndef DTU236_ASI
	EnterCriticalSection(&csSignal);
	wsprintf(szString, "%s, FIFO Overruns: %d", szLastSignalReport, nFIFOOverrunCounter);
	LeaveCriticalSection(&csSignal);
#else DTU236_ASI
	wsprintf(szString, "FIFO Overruns: %d", nFIFOOverrunCounter);
#endif DTU236_ASI
	return TRUE;
}

BOOL TSReader_GetTunerString(char * szString)
{
	lstrcpy(szString, szLastTune);
	return TRUE;
}

BOOL TSReader_SendDiSEqC(BYTE * bCommand, int nLength)
{
	return TRUE;
}

int TSReader_GetSyncLossCount(BOOL fReset)
{
	return SourceHelper_GetSyncLossCount(fReset);
}

} // extern "C"

BOOL APIENTRY DllMain(HANDLE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
		hInstance = hModule;
	return TRUE;
}
