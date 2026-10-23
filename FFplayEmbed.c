// Embedded FFplay video window
//
// Playback uses the same HTTP stream as VLC (TSReader_VLC.dll serves the selected
// program on http://127.0.0.1:<VLC port>). Instead of starting VLC we start ffplay
// borderless and re-parent its SDL window into a TSReader window, so the video sits
// inside TSReader. ffplay's own keys keep working when the video has focus
// (F = full screen, Space = pause).
//
// For the embedded player TSReader streams the video, every audio track and every DVB
// subtitle track. The window's Audio and Subtitles menus (labelled from the PMT
// language descriptors) switch tracks. TSReader writes the PMT only once at the start
// of the stream, so ffplay cannot simply be restarted with another track; instead the
// menus drive ffplay's own track cycling (A / T keys), which steps through the tracks
// in PMT order - the same order as our menus - with subtitles also passing "off".

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "TSReader.h"
#include "resource.h"

extern PVARIABLES v;
extern char gszAppName[];

#define WM_FFPLAY_ATTACH	(WM_USER + 1)	// wParam = ffplay HWND
#define WM_FFPLAY_EXITED	(WM_USER + 2)	// ffplay process ended by itself

static char gszFFplayHostClass[] = {"TSReaderFFplayHost"};
static HWND hWndHost;					// our video window
static HWND hWndFFplay;					// ffplay's SDL window once re-parented
static PROCESS_INFORMATION piFFplay;
static BOOL fStopping;
static HWND hWndNotify;					// main window; told when the user closes the video
static UINT uStopCommand;				// WM_COMMAND sent to hWndNotify to stop the stream

void FFplayEmbedStop();
BOOL IsSubtitleStream(int nPMTProgramIndex, int nESIndex);

#define ID_FFPLAY_AUDIO_BASE		41000	// + track index
#define ID_FFPLAY_SUBTITLE_OFF		42000
#define ID_FFPLAY_SUBTITLE_BASE		42001	// + track index

typedef struct
{
	int nPID;
	char szLabel[128];
} FFPLAYTRACK;

static FFPLAYTRACK audioTracks[MAX_AUDIO_STREAMS];
static FFPLAYTRACK subtitleTracks[MAX_OTHER_STREAMS];
static int nAudioTracks, nSubtitleTracks;
static int nCurrentAudio;				// index into audioTracks
static int nCurrentSubtitle;			// index into subtitleTracks, nSubtitleTracks = off

// Finds a descriptor in an ES descriptor loop; returns its payload and length
static BYTE * FindESDescriptor(PESLIST pES, int nTag, int * pnLength)
{
	int nIndex = 0;

	if (pES->pDescriptors == NULL)
		return NULL;
	while (nIndex + 2 <= pES->nDescriptorsLength)
	{
		int nLength = pES->pDescriptors[nIndex + 1];
		if (pES->pDescriptors[nIndex] == nTag && nIndex + 2 + nLength <= pES->nDescriptorsLength)
		{
			*pnLength = nLength;
			return &pES->pDescriptors[nIndex + 2];
		}
		nIndex += 2 + nLength;
	}
	return NULL;
}

static void LanguageName(const BYTE * pCode, char * szName)
{
	static const char * names[][2] = {
		{"fin", "Finnish"}, {"swe", "Swedish"}, {"eng", "English"}, {"sme", "Northern Sami"},
		{"smi", "Sami"}, {"nor", "Norwegian"}, {"dan", "Danish"}, {"deu", "German"},
		{"ger", "German"}, {"fra", "French"}, {"fre", "French"}, {"rus", "Russian"},
		{"est", "Estonian"}, {"spa", "Spanish"}, {"ita", "Italian"}, {"dut", "Dutch"},
		{"nld", "Dutch"}, {"qaa", "Original language"}, {"mul", "Multiple languages"},
		{"und", "Undefined"}, {"nar", "Audio description"}, {"qad", "Audio description"}};
	char szCode[4];
	int i;

	szCode[0] = (char)tolower(pCode[0]);
	szCode[1] = (char)tolower(pCode[1]);
	szCode[2] = (char)tolower(pCode[2]);
	szCode[3] = '\0';
	for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
	{
		if (lstrcmp(szCode, names[i][0]) == 0)
		{
			lstrcpy(szName, names[i][1]);
			return;
		}
	}
	lstrcpy(szName, szCode);
}

// Builds the track lists from what the stream dialog selected, in the order the
// streamed PMT lists them (video, audio tracks, other tracks)
static void BuildTrackLists()
{
	PPMT pPMT = &v->pat.pmt[v->nSelectedProgram];
	int i;

	nAudioTracks = 0;
	for (i = 0; i < MAX_AUDIO_STREAMS && v->nRecordAudioESIndex[i] != -1; i++)
	{
		PESLIST pES = &pPMT->es[v->nRecordAudioESIndex[i]];
		FFPLAYTRACK * pTrack = &audioTracks[nAudioTracks++];
		const char * szCodec = "audio";
		char szLanguage[64];
		BYTE * pDesc;
		int nLength;

		switch (pES->nStreamType)
		{
		case 0x03:
		case 0x04:
			szCodec = "MPEG";
			break;
		case 0x0f:
			szCodec = "AAC";
			break;
		case 0x11:
			szCodec = "HE-AAC";
			break;
		case 0x81:
			szCodec = "AC-3";
			break;
		case 0x06:
			if (FindESDescriptor(pES, 0x7a, &nLength) != NULL)
				szCodec = "E-AC-3";
			else if (FindESDescriptor(pES, 0x6a, &nLength) != NULL)
				szCodec = "AC-3";
			else if (FindESDescriptor(pES, 0x7c, &nLength) != NULL)
				szCodec = "AAC";
			break;
		}
		pTrack->nPID = pES->nESPID;
		pDesc = FindESDescriptor(pES, 0x0a, &nLength);		// ISO 639 language
		if (pDesc != NULL && nLength >= 4)
		{
			LanguageName(pDesc, szLanguage);
			wsprintf(pTrack->szLabel, "%s (%s)%s", szLanguage, szCodec,
					 pDesc[3] == 0x03 ? " - visual impaired commentary" : "");
		}
		else
			wsprintf(pTrack->szLabel, "Audio %d (%s)", nAudioTracks, szCodec);
	}

	nSubtitleTracks = 0;
	for (i = 0; i < MAX_OTHER_STREAMS && v->nRecordOtherESIndex[i] != -1; i++)
	{
		PESLIST pES;
		FFPLAYTRACK * pTrack;
		BYTE * pDesc;
		int nLength;

		if (!IsSubtitleStream(v->nSelectedProgram, v->nRecordOtherESIndex[i]))
			continue;
		pES = &pPMT->es[v->nRecordOtherESIndex[i]];
		pTrack = &subtitleTracks[nSubtitleTracks++];
		pTrack->nPID = pES->nESPID;
		pDesc = FindESDescriptor(pES, 0x59, &nLength);		// subtitling descriptor
		if (pDesc != NULL && nLength >= 8)
		{
			char szLanguage[64];
			LanguageName(pDesc, szLanguage);
			wsprintf(pTrack->szLabel, "%s%s", szLanguage,
					 (pDesc[3] >= 0x20 && pDesc[3] <= 0x24) ? " - hard of hearing" : "");
		}
		else
			wsprintf(pTrack->szLabel, "Subtitles %d", nSubtitleTracks);
	}
	nCurrentAudio = 0;
	nCurrentSubtitle = nSubtitleTracks ? 0 : nSubtitleTracks;	// first track on, if any
}

static void UpdateTrackMenus()
{
	HMENU hMenu = GetMenu(hWndHost);

	if (hMenu == NULL)
		return;
	if (nAudioTracks)
		CheckMenuRadioItem(hMenu, ID_FFPLAY_AUDIO_BASE, ID_FFPLAY_AUDIO_BASE + nAudioTracks - 1,
						   ID_FFPLAY_AUDIO_BASE + nCurrentAudio, MF_BYCOMMAND);
	CheckMenuRadioItem(hMenu, ID_FFPLAY_SUBTITLE_OFF, ID_FFPLAY_SUBTITLE_BASE + nSubtitleTracks,
					   nCurrentSubtitle == nSubtitleTracks ? ID_FFPLAY_SUBTITLE_OFF : ID_FFPLAY_SUBTITLE_BASE + nCurrentSubtitle,
					   MF_BYCOMMAND);
}

static void CreateTrackMenus()
{
	HMENU hMenuBar = CreateMenu();
	HMENU hMenuAudio = CreatePopupMenu();
	HMENU hMenuSubtitles = CreatePopupMenu();
	int i;

	for (i = 0; i < nAudioTracks; i++)
		AppendMenu(hMenuAudio, MF_STRING, ID_FFPLAY_AUDIO_BASE + i, audioTracks[i].szLabel);
	if (nAudioTracks == 0)
		AppendMenu(hMenuAudio, MF_STRING | MF_GRAYED, 0, "(no audio)");
	AppendMenu(hMenuSubtitles, MF_STRING, ID_FFPLAY_SUBTITLE_OFF, "Off");
	if (nSubtitleTracks)
		AppendMenu(hMenuSubtitles, MF_SEPARATOR, 0, NULL);
	for (i = 0; i < nSubtitleTracks; i++)
		AppendMenu(hMenuSubtitles, MF_STRING, ID_FFPLAY_SUBTITLE_BASE + i, subtitleTracks[i].szLabel);
	if (nSubtitleTracks == 0)
		AppendMenu(hMenuSubtitles, MF_STRING | MF_GRAYED, 0, "(no DVB subtitles in this program)");
	AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hMenuAudio, "&Audio");
	AppendMenu(hMenuBar, MF_POPUP, (UINT_PTR)hMenuSubtitles, "&Subtitles");
	SetMenu(hWndHost, hMenuBar);
	UpdateTrackMenus();
}

// Presses a key in ffplay nPresses times (ffplay steps one track per press)
static void SendFFplayKey(UINT uVirtualKey, int nPresses)
{
	UINT uScanCode = MapVirtualKey(uVirtualKey, MAPVK_VK_TO_VSC);
	LPARAM lDown = 1 | (uScanCode << 16);
	LPARAM lUp = lDown | (1 << 30) | (1u << 31);

	if (hWndFFplay == NULL || !IsWindow(hWndFFplay))
		return;
	SetFocus(hWndFFplay);
	while (nPresses-- > 0)
	{
		PostMessage(hWndFFplay, WM_KEYDOWN, uVirtualKey, lDown);
		PostMessage(hWndFFplay, WM_KEYUP, uVirtualKey, lUp);
	}
}

static void SelectAudioTrack(int nTrack)
{
	if (nTrack < 0 || nTrack >= nAudioTracks || nTrack == nCurrentAudio)
		return;
	// ffplay's A key: next audio track, wrapping round
	SendFFplayKey('A', (nTrack - nCurrentAudio + nAudioTracks) % nAudioTracks);
	nCurrentAudio = nTrack;
	UpdateTrackMenus();
}

static void SelectSubtitleTrack(int nTrack)		// nSubtitleTracks = off
{
	int nPositions = nSubtitleTracks + 1;			// every track plus "off"

	if (nTrack < 0 || nTrack > nSubtitleTracks || nTrack == nCurrentSubtitle)
		return;
	// ffplay's T key: next subtitle track, then off after the last one, then the first again
	SendFFplayKey('T', (nTrack - nCurrentSubtitle + nPositions) % nPositions);
	nCurrentSubtitle = nTrack;
	UpdateTrackMenus();
}

typedef struct
{
	DWORD dwProcessId;
	HWND hWndFound;
} FINDFFPLAY;

static BOOL CALLBACK FindFFplayWindowProc(HWND hWnd, LPARAM lParam)
{
	FINDFFPLAY * pFind = (FINDFFPLAY *)lParam;
	DWORD dwProcessId = 0;

	GetWindowThreadProcessId(hWnd, &dwProcessId);
	if (dwProcessId == pFind->dwProcessId && IsWindowVisible(hWnd) && GetWindow(hWnd, GW_OWNER) == NULL)
	{
		pFind->hWndFound = hWnd;
		return FALSE;
	}
	return TRUE;
}

// Waits for ffplay's window, hands it to the host window, then waits for ffplay to exit
static DWORD WINAPI FFplayWatchThread(LPVOID lpv)
{
	HANDLE hProcess = (HANDLE)lpv;
	FINDFFPLAY find;
	DWORD dwStart = GetTickCount();

	find.dwProcessId = GetProcessId(hProcess);
	find.hWndFound = NULL;
	while (find.hWndFound == NULL && GetTickCount() - dwStart < 20000)
	{
		if (WaitForSingleObject(hProcess, 100) == WAIT_OBJECT_0)
			break;
		EnumWindows(FindFFplayWindowProc, (LPARAM)&find);
	}
	if (find.hWndFound != NULL && IsWindow(hWndHost))
		PostMessage(hWndHost, WM_FFPLAY_ATTACH, (WPARAM)find.hWndFound, 0);

	WaitForSingleObject(hProcess, INFINITE);
	if (IsWindow(hWndHost))
		PostMessage(hWndHost, WM_FFPLAY_EXITED, 0, 0);
	return 0;
}

static void ResizeFFplay()
{
	RECT rc;

	if (hWndFFplay != NULL && IsWindow(hWndFFplay))
	{
		GetClientRect(hWndHost, &rc);
		MoveWindow(hWndFFplay, 0, 0, rc.right, rc.bottom, TRUE);
	}
}

static LRESULT CALLBACK FFplayHostWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg)
	{
	case WM_FFPLAY_ATTACH:
		{
			HWND hWndChild = (HWND)wParam;
			LONG lStyle;

			if (!IsWindow(hWndChild))
				break;
			lStyle = GetWindowLong(hWndChild, GWL_STYLE);
			lStyle &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
			lStyle |= WS_CHILD | WS_VISIBLE;
			SetWindowLong(hWndChild, GWL_STYLE, lStyle);
			SetParent(hWndChild, hWnd);
			hWndFFplay = hWndChild;
			ResizeFFplay();
			SetFocus(hWndFFplay);
		}
		break;
	case WM_SIZE:
		ResizeFFplay();
		break;
	case WM_COMMAND:
		{
			int nID = LOWORD(wParam);

			if (nID >= ID_FFPLAY_AUDIO_BASE && nID < ID_FFPLAY_AUDIO_BASE + nAudioTracks)
				SelectAudioTrack(nID - ID_FFPLAY_AUDIO_BASE);
			else if (nID == ID_FFPLAY_SUBTITLE_OFF)
				SelectSubtitleTrack(nSubtitleTracks);
			else if (nID >= ID_FFPLAY_SUBTITLE_BASE && nID < ID_FFPLAY_SUBTITLE_BASE + nSubtitleTracks)
				SelectSubtitleTrack(nID - ID_FFPLAY_SUBTITLE_BASE);
		}
		break;
	case WM_SETFOCUS:
	case WM_LBUTTONDOWN:
		if (hWndFFplay != NULL && IsWindow(hWndFFplay))
			SetFocus(hWndFFplay);
		break;
	case WM_FFPLAY_EXITED:
		// ffplay quit by itself (Q/Esc or a stream error): stop the stream as well
		if (!fStopping && hWndNotify != NULL)
			PostMessage(hWndNotify, WM_COMMAND, uStopCommand, 0);
		break;
	case WM_CLOSE:
		if (!fStopping && hWndNotify != NULL)
		{
			// Let TSReader stop the stream; that calls FFplayEmbedStop() which destroys us
			PostMessage(hWndNotify, WM_COMMAND, uStopCommand, 0);
			return 0;
		}
		DestroyWindow(hWnd);
		break;
	case WM_DESTROY:
		hWndHost = NULL;
		hWndFFplay = NULL;
		break;
	case WM_ERASEBKGND:
		{
			RECT rc;
			GetClientRect(hWnd, &rc);
			FillRect((HDC)wParam, &rc, GetStockObject(BLACK_BRUSH));
		}
		return 1;
	default:
		return DefWindowProc(hWnd, msg, wParam, lParam);
	}
	return 0;
}

// Returns the ffplay.exe to use: the configured one, or <TSReader folder>\ffplay\ffplay.exe
static void FFplayGetBundledExecutable(char * szExe, int nSize)
{
	char * pSlash;

	GetModuleFileName(NULL, szExe, nSize);
	pSlash = strrchr(szExe, '\\');
	if (pSlash != NULL)
		*pSlash = '\0';
	lstrcat(szExe, "\\ffplay\\ffplay.exe");
}

// Settings are shared by every TSReader copy on the PC (HKCU), so a configured path that
// no longer exists - e.g. another copy's folder - falls back to the bundled ffplay
void FFplayGetExecutable(char * szExe, int nSize)
{
	if (lstrlen(v->szFFplayLocation) && GetFileAttributes(v->szFFplayLocation) != INVALID_FILE_ATTRIBUTES)
		lstrcpyn(szExe, v->szFFplayLocation, nSize);
	else
		FFplayGetBundledExecutable(szExe, nSize);
}

BOOL FFplayEmbedStart(HWND hWndOwner, UINT uStopCmd, const char * szURL, const char * szTitle)
{
	static BOOL fClassRegistered = FALSE;
	char szExe[MAX_PATH];
	char szCommand[MAX_PATH + 1024];
	char szWindowTitle[256];
	char szTracks[64];
	STARTUPINFO si;
	RECT rc;
	HANDLE hThread;
	DWORD dwThreadID;

	FFplayEmbedStop();

	FFplayGetExecutable(szExe, sizeof(szExe));
	if (GetFileAttributes(szExe) == INVALID_FILE_ATTRIBUTES)
	{
		char szTemp[MAX_PATH + 128];
		wsprintf(szTemp, "ffplay was not found:\n%s\n\nSet its location in Playback/FFplay/Settings.", szExe);
		MessageBox(hWndOwner, szTemp, gszAppName, MB_ICONSTOP);
		return FALSE;
	}

	if (!fClassRegistered)
	{
		WNDCLASS wc;

		memset(&wc, 0, sizeof(wc));
		wc.style = CS_HREDRAW | CS_VREDRAW;
		wc.lpfnWndProc = FFplayHostWndProc;
		wc.hInstance = v->hInstance;
		wc.hIcon = LoadIcon(v->hInstance, MAKEINTRESOURCE(IDI_DVBSMALL_LOGO));
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = gszFFplayHostClass;
		RegisterClass(&wc);
		fClassRegistered = TRUE;
	}

	hWndNotify = hWndOwner;
	uStopCommand = uStopCmd;
	fStopping = FALSE;
	wsprintf(szWindowTitle, "TSReader Video - %s", szTitle);
	hWndHost = CreateWindow(gszFFplayHostClass, szWindowTitle,
							WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
							CW_USEDEFAULT, CW_USEDEFAULT, 960, 580,
							hWndOwner, NULL, v->hInstance, NULL);
	if (hWndHost == NULL)
		return FALSE;
	BuildTrackLists();
	CreateTrackMenus();
	ShowWindow(hWndHost, SW_SHOW);
	GetClientRect(hWndHost, &rc);

	// Start on the first audio track and the first subtitle track (selected by PID), so
	// the menus know where ffplay's track cycling starts from
	szTracks[0] = '\0';
	if (nAudioTracks)
		wsprintf(szTracks, "-ast i:%d ", audioTracks[nCurrentAudio].nPID);
	if (nCurrentSubtitle < nSubtitleTracks)
		wsprintf(szTracks + lstrlen(szTracks), "-sst i:%d ", subtitleTracks[nCurrentSubtitle].nPID);

	// Start ffplay off-screen and borderless; it is moved into hWndHost once its window exists
	_snprintf(szCommand, sizeof(szCommand) - 1,
			  "\"%s\" -hide_banner -loglevel error -noborder -left -32000 -top -32000 -x %d -y %d "
			  "-window_title \"TSReader embedded ffplay\" %s %s\"%s\"",
			  szExe, rc.right, rc.bottom, v->szFFplayOptions, szTracks, szURL);	// our track choice last so it wins over -ast/-sst in the options
	szCommand[sizeof(szCommand) - 1] = '\0';

	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&piFFplay, 0, sizeof(piFFplay));
	if (!CreateProcess(NULL, szCommand, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &piFFplay))
	{
		char szTemp[MAX_PATH + 128];
		wsprintf(szTemp, "Unable to start ffplay (error %d):\n%s", GetLastError(), szExe);
		MessageBox(hWndOwner, szTemp, gszAppName, MB_ICONSTOP);
		fStopping = TRUE;
		DestroyWindow(hWndHost);
		return FALSE;
	}
	CloseHandle(piFFplay.hThread);

	// The watch thread gets its own process handle so FFplayEmbedStop can close ours
	{
		HANDLE hWatch;
		DuplicateHandle(GetCurrentProcess(), piFFplay.hProcess, GetCurrentProcess(), &hWatch, 0, FALSE, DUPLICATE_SAME_ACCESS);
		hThread = CreateThread(NULL, 0, FFplayWatchThread, (LPVOID)hWatch, 0, &dwThreadID);
		if (hThread != NULL)
			CloseHandle(hThread);
	}
	return TRUE;
}

void FFplayEmbedStop()
{
	fStopping = TRUE;
	if (piFFplay.hProcess != NULL)
	{
		// Close ffplay's window first so it can exit tidily, then make sure it is gone
		if (hWndFFplay != NULL && IsWindow(hWndFFplay))
			PostMessage(hWndFFplay, WM_CLOSE, 0, 0);
		if (WaitForSingleObject(piFFplay.hProcess, 1500) != WAIT_OBJECT_0)
			TerminateProcess(piFFplay.hProcess, 0);
		CloseHandle(piFFplay.hProcess);
		piFFplay.hProcess = NULL;
	}
	if (hWndHost != NULL && IsWindow(hWndHost))
		DestroyWindow(hWndHost);
	hWndHost = NULL;
	hWndFFplay = NULL;
}

// Plays the selected program with the default player (double-click on a thumbnail).
// If the embedded player is already playing it switches to the newly selected program.
void PlayWithDefaultPlayer()
{
	UINT uCommand = ID_PLAYBACK_FFPLAY;

	if (v->nDefaultPlayer >= 1 && v->nDefaultPlayer <= MAX_VLC_CONFIGURATIONS)
		uCommand = ID_PLAYBACK_VLC_1 + v->nDefaultPlayer - 1;

	if (v->fRecording)
	{
		if (v->nStreamTo != STREAM_TO_VLC || !v->fFFplayEmbedded)
			return;		// recording or another player is busy; leave it alone
		PostMessage(v->hWndMainWindow, WM_COMMAND, ID_PLAYBACK_FFPLAY, 0);	// stop the current one
	}
	PostMessage(v->hWndMainWindow, WM_COMMAND, uCommand, 0);
}

BOOL CALLBACK DefaultPlayerDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			HWND hList = GetDlgItem(hDlg, IDC_DEFAULT_PLAYER_LIST);
			char szTemp[256];
			int i;

			SendMessage(hList, CB_ADDSTRING, 0, (LPARAM)"Embedded FFplay");
			for (i = 0; i < MAX_VLC_CONFIGURATIONS; i++)
			{
				if (lstrlen(v->szVLCConfigDescription[i]))
					wsprintf(szTemp, "VLC %d: %s", i + 1, v->szVLCConfigDescription[i]);
				else
					wsprintf(szTemp, "VLC %d", i + 1);
				SendMessage(hList, CB_ADDSTRING, 0, (LPARAM)szTemp);
			}
			SendMessage(hList, CB_SETCURSEL,
						(v->nDefaultPlayer >= 1 && v->nDefaultPlayer <= MAX_VLC_CONFIGURATIONS) ? v->nDefaultPlayer : 0, 0);
		}
		return TRUE;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDOK:
			v->nDefaultPlayer = (int)SendDlgItemMessage(hDlg, IDC_DEFAULT_PLAYER_LIST, CB_GETCURSEL, 0, 0);
			if (v->nDefaultPlayer < 0)
				v->nDefaultPlayer = 0;
			EndDialog(hDlg, TRUE);
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		}
		break;
	}
	return FALSE;
}

BOOL CALLBACK FFplaySettingsDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			char szExe[MAX_PATH];

			FFplayGetExecutable(szExe, sizeof(szExe));
			SetDlgItemText(hDlg, IDC_FFPLAY_LOCATION, szExe);
			SetDlgItemText(hDlg, IDC_FFPLAY_OPTIONS, v->szFFplayOptions);
		}
		return TRUE;
	case WM_CLOSE:
		EndDialog(hDlg, FALSE);
		break;
	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_FFPLAY_BROWSE:
			{
				OPENFILENAME ofn;
				char szExe[MAX_PATH];

				GetDlgItemText(hDlg, IDC_FFPLAY_LOCATION, szExe, sizeof(szExe));
				memset(&ofn, 0, sizeof(ofn));
				ofn.lStructSize = sizeof(ofn);
				ofn.hwndOwner = hDlg;
				ofn.lpstrFilter = "ffplay.exe\0ffplay.exe\0Programs (*.exe)\0*.exe\0";
				ofn.lpstrFile = szExe;
				ofn.nMaxFile = sizeof(szExe);
				ofn.lpstrTitle = "Locate ffplay.exe";
				ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
				if (GetOpenFileName(&ofn))
					SetDlgItemText(hDlg, IDC_FFPLAY_LOCATION, szExe);
			}
			break;
		case IDOK:
			{
				char szBundled[MAX_PATH];

				GetDlgItemText(hDlg, IDC_FFPLAY_LOCATION, v->szFFplayLocation, sizeof(v->szFFplayLocation));
				// Keep "use the bundled ffplay" as an empty setting so it follows this copy's folder
				FFplayGetBundledExecutable(szBundled, sizeof(szBundled));
				if (lstrcmpi(v->szFFplayLocation, szBundled) == 0)
					v->szFFplayLocation[0] = '\0';
			}
			GetDlgItemText(hDlg, IDC_FFPLAY_OPTIONS, v->szFFplayOptions, sizeof(v->szFFplayOptions));
			EndDialog(hDlg, TRUE);
			break;
		case IDCANCEL:
			EndDialog(hDlg, FALSE);
			break;
		}
		break;
	}
	return FALSE;
}
