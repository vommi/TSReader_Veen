// Descriptor hex tool and editor (View > Descriptor Hex / Editor)
//
// Lists every descriptor TSReader keeps from the PMT (program and elementary streams),
// SDT, NIT, BAT and TOT, and shows the selected one as hex in the format a device's
// "add descriptor" field wants (e.g. continuous 561473776511...), ready to copy.
// The descriptor can be edited, either as hex or - for teletext (0x56), subtitling
// (0x59), ISO 639 language (0x0A) and stream identifier (0x52) - as one entry per line;
// the other view and the length byte follow. "New" starts one from scratch.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "TSReader.h"
#include "resource.h"

extern PVARIABLES v;
extern char gszAppName[];

void DecodeDescriptorNames(char * szDescriptor, BYTE nDescriptorID);

#define DH_MAX_ITEMS		4000

typedef struct
{
	char szTable[8];
	char szOwner[128];
	int nLength;				// whole descriptor: tag + length + data
	BYTE bData[257];
} DHITEM;

static DHITEM * pItems;
static int nItems;
static BYTE bCurrent[257];		// descriptor being edited
static int nCurrent;
static BOOL fUpdating;			// set while one view rewrites the other
static int nFormat;				// 0 continuous, 1 spaced, 2 0x prefixed, 3 C array

// ---------------------------------------------------------------- collecting

static void AddItem(const char * szTable, const char * szOwner, const BYTE * pDescriptor, int nLength)
{
	DHITEM * pItem;

	if (nItems >= DH_MAX_ITEMS || nLength < 2 || nLength > 257)
		return;
	pItem = &pItems[nItems++];
	lstrcpyn(pItem->szTable, szTable, sizeof(pItem->szTable));
	lstrcpyn(pItem->szOwner, szOwner, sizeof(pItem->szOwner));
	pItem->nLength = nLength;
	memcpy(pItem->bData, pDescriptor, nLength);
}

// Splits a descriptor loop into descriptors
static void AddLoop(const char * szTable, const char * szOwner, const BYTE * pLoop, int nLoopLength)
{
	int nIndex = 0;

	if (pLoop == NULL)
		return;
	while (nIndex + 2 <= nLoopLength)
	{
		int nLength = 2 + pLoop[nIndex + 1];
		if (nIndex + nLength > nLoopLength)
			break;
		AddItem(szTable, szOwner, &pLoop[nIndex], nLength);
		nIndex += nLength;
	}
}

static void ServiceName(int nServiceID, char * szName, int nSize)
{
	szName[0] = '\0';
	if (nServiceID >= 0 && nServiceID < MAX_EIT_CHANNEL_DATA && v->pChannelData[nServiceID] != NULL)
		lstrcpyn(szName, v->pChannelData[nServiceID]->szShortName, nSize);
}

static void CollectDescriptors()
{
	int i, j;
	char szOwner[160], szName[128];

	nItems = 0;

	// PMT: program info and every elementary stream
	for (i = 0; i < MAX_PAT_ENTRIES; i++)
	{
		PPMT pPMT = &v->pat.pmt[i];

		if (pPMT->nPMTPID == 0)
			break;
		ServiceName(pPMT->nProgramNumber, szName, sizeof(szName));
		_snprintf(szOwner, sizeof(szOwner) - 1, "Service %d %s (program info)", pPMT->nProgramNumber, szName);
		szOwner[sizeof(szOwner) - 1] = '\0';
		AddLoop("PMT", szOwner, pPMT->pProgramInfo, pPMT->nProgramInfoLength);
		for (j = 0; j < MAX_ESLIST_ENTRIES; j++)
		{
			PESLIST pES = &pPMT->es[j];

			if (pES->nESPID == 0)
				break;
			_snprintf(szOwner, sizeof(szOwner) - 1, "Service %d %s / PID %d (0x%04x) type 0x%02x",
					  pPMT->nProgramNumber, szName, pES->nESPID, pES->nESPID, pES->nStreamType);
			szOwner[sizeof(szOwner) - 1] = '\0';
			AddLoop("PMT", szOwner, pES->pDescriptors, pES->nDescriptorsLength);
		}
	}

	// SDT: descriptors kept besides the service descriptor
	for (i = 0; i < MAX_EIT_CHANNEL_DATA; i++)
	{
		PEITCHANNELDATA pChannel = v->pChannelData[i];

		if (pChannel == NULL || pChannel->fATSC)
			continue;
		_snprintf(szOwner, sizeof(szOwner) - 1, "Service %d %s", i, pChannel->szShortName);
		szOwner[sizeof(szOwner) - 1] = '\0';
		for (j = 0; j < MAX_SDT_EXTRA_DESCRIPTORS && pChannel->pExtraDescriptors[j] != NULL; j++)
			AddItem("SDT", szOwner, pChannel->pExtraDescriptors[j], 2 + pChannel->pExtraDescriptors[j][1]);
	}

	// NIT: network descriptors once per network, then each transport stream's
	{
		int nNetworksDone[32], nDone = 0, k;

		for (i = 0; i < MAX_TS_ENTRIES; i++)
		{
			PNITENTRY pNIT = v->pNITData[i];
			BOOL fSeen = FALSE;

			if (pNIT == NULL)
				continue;
			for (k = 0; k < nDone; k++)
				if (nNetworksDone[k] == pNIT->nNetworkID)
					fSeen = TRUE;
			if (!fSeen && nDone < 32)
			{
				nNetworksDone[nDone++] = pNIT->nNetworkID;
				_snprintf(szOwner, sizeof(szOwner) - 1, "Network %d %s", pNIT->nNetworkID, pNIT->szNetworkName);
				szOwner[sizeof(szOwner) - 1] = '\0';
				AddLoop("NIT", szOwner, pNIT->pNetworkDescriptors, pNIT->nNetworkDescriptorsLength);
			}
			_snprintf(szOwner, sizeof(szOwner) - 1, "Network %d / TS %d (ONID %d)%s", pNIT->nNetworkID,
					  pNIT->nTransportStreamID, pNIT->nOriginalNetworkID, pNIT->fThisTS ? " this TS" : "");
			szOwner[sizeof(szOwner) - 1] = '\0';
			for (j = 0; j < MAX_NIT_EXTRA_DESCRIPTORS && pNIT->pExtraDescriptors[j] != NULL; j++)
				AddItem("NIT", szOwner, pNIT->pExtraDescriptors[j], 2 + pNIT->pExtraDescriptors[j][1]);
		}
	}

	// BAT: bouquet and transport stream descriptors
	for (i = 0; i < MAX_BAT_ENTRIES; i++)
	{
		PBAT pBAT = &v->bat[i];

		if (pBAT->bouquet_descriptors == NULL && pBAT->batts[0].transport_descriptors == NULL)
			continue;
		_snprintf(szOwner, sizeof(szOwner) - 1, "Bouquet %d", pBAT->bouquet_id);
		szOwner[sizeof(szOwner) - 1] = '\0';
		AddLoop("BAT", szOwner, pBAT->bouquet_descriptors, pBAT->bouquet_descriptors_length);
		for (j = 0; j < MAX_BAT_TRANSPORT_ITEMS; j++)
		{
			if (pBAT->batts[j].transport_descriptors == NULL)
				continue;
			_snprintf(szOwner, sizeof(szOwner) - 1, "Bouquet %d / TS %d (ONID %d)", pBAT->bouquet_id,
					  pBAT->batts[j].transport_stream_id, pBAT->batts[j].original_network_id);
			szOwner[sizeof(szOwner) - 1] = '\0';
			AddLoop("BAT", szOwner, pBAT->batts[j].transport_descriptors, pBAT->batts[j].transport_descriptors_length);
		}
	}

	// TOT
	AddLoop("TOT", "Time offset", v->dvbtot.pDescriptors, v->dvbtot.nDescriptorsLength);
}

// ---------------------------------------------------------------- hex text

static void FormatHex(const BYTE * pData, int nLength, char * szOut, int nSize)
{
	int i, nOut = 0;

	szOut[0] = '\0';
	if (nFormat == 2 && nSize > 2)
	{
		lstrcpy(szOut, "0x");
		nOut = 2;
	}
	for (i = 0; i < nLength && nOut + 8 < nSize; i++)
	{
		switch (nFormat)
		{
		case 1:
			nOut += wsprintf(&szOut[nOut], i ? " %02X" : "%02X", pData[i]);
			break;
		case 3:
			nOut += wsprintf(&szOut[nOut], i ? ", 0x%02X" : "0x%02X", pData[i]);
			break;
		default:
			nOut += wsprintf(&szOut[nOut], "%02X", pData[i]);
			break;
		}
	}
}

// Reads hex in any of the formats (0x prefixes, spaces, commas, braces); returns bytes
static int ParseHex(const char * szText, BYTE * pOut, int nMax)
{
	int nCount = 0, nNibble = -1;
	const char * p = szText;

	while (*p && nCount < nMax)
	{
		int nValue = -1;

		if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		{
			if (nNibble != -1)		// a lone digit before 0x: take it as a byte
			{
				pOut[nCount++] = (BYTE)nNibble;
				nNibble = -1;
			}
			p += 2;
			continue;
		}
		if (*p >= '0' && *p <= '9')
			nValue = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			nValue = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			nValue = *p - 'A' + 10;
		else if (nNibble != -1)		// separator after a single digit
		{
			pOut[nCount++] = (BYTE)nNibble;
			nNibble = -1;
		}
		if (nValue >= 0)
		{
			if (nNibble == -1)
				nNibble = nValue;
			else
			{
				pOut[nCount++] = (BYTE)((nNibble << 4) | nValue);
				nNibble = -1;
			}
		}
		p++;
	}
	if (nNibble != -1 && nCount < nMax)
		pOut[nCount++] = (BYTE)nNibble;
	return nCount;
}

// ---------------------------------------------------------------- field editors

static BOOL TagHasFields(int nTag)
{
	return nTag == 0x56 || nTag == 0x46 || nTag == 0x59 || nTag == 0x0a || nTag == 0x52;
}

static const char * FieldsHelp(int nTag)
{
	switch (nTag)
	{
	case 0x56:
	case 0x46:
		return "One page per line: language type page, e.g.  fin 2 699\r\n"
			   "type: 1 initial page, 2 subtitles, 3 additional info, 4 schedule, 5 subtitles for the hard of hearing";
	case 0x59:
		return "One per line: language type composition ancillary, e.g.  fin 0x10 1 1\r\n"
			   "type: 0x10 normal, 0x20 hard of hearing (0x11-0x14 / 0x21-0x24 with aspect ratio)";
	case 0x0a:
		return "One per line: language audio_type, e.g.  fin 0\r\n"
			   "audio_type: 0 undefined, 1 clean effects, 2 hearing impaired, 3 visual impaired commentary";
	case 0x52:
		return "component_tag in hex, e.g.  0x03";
	}
	return "No field editor for this descriptor: edit the hex";
}

static void LanguageText(const BYTE * p, char * sz)
{
	int i;
	for (i = 0; i < 3; i++)
		sz[i] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '?';
	sz[3] = '\0';
}

static void DescriptorToFields(const BYTE * pData, int nLength, char * szOut, int nSize)
{
	int nTag = pData[0], i, nOut = 0;
	char szLang[4];

	szOut[0] = '\0';
	switch (nTag)
	{
	case 0x56:
	case 0x46:
		for (i = 2; i + 5 <= nLength && nOut + 32 < nSize; i += 5)
		{
			int nMagazine = pData[i + 3] & 7;
			LanguageText(&pData[i], szLang);
			nOut += wsprintf(&szOut[nOut], "%s %d %d%02X\r\n", szLang, pData[i + 3] >> 3, nMagazine ? nMagazine : 8, pData[i + 4]);
		}
		break;
	case 0x59:
		for (i = 2; i + 8 <= nLength && nOut + 40 < nSize; i += 8)
		{
			LanguageText(&pData[i], szLang);
			nOut += wsprintf(&szOut[nOut], "%s 0x%02X %d %d\r\n", szLang, pData[i + 3],
							 (pData[i + 4] << 8) | pData[i + 5], (pData[i + 6] << 8) | pData[i + 7]);
		}
		break;
	case 0x0a:
		for (i = 2; i + 4 <= nLength && nOut + 32 < nSize; i += 4)
		{
			LanguageText(&pData[i], szLang);
			nOut += wsprintf(&szOut[nOut], "%s %d\r\n", szLang, pData[i + 3]);
		}
		break;
	case 0x52:
		if (nLength >= 3)
			wsprintf(szOut, "0x%02X\r\n", pData[2]);
		break;
	}
}

static int ParseNumber(const char ** pp)
{
	const char * p = *pp;
	int nValue = 0;

	while (*p == ' ' || *p == '\t')
		p++;
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
	{
		p += 2;
		while (isxdigit((BYTE)*p))
		{
			nValue = nValue * 16 + (isdigit((BYTE)*p) ? *p - '0' : (toupper(*p) - 'A' + 10));
			p++;
		}
	}
	else
	{
		while (isdigit((BYTE)*p))
			nValue = nValue * 10 + (*p++ - '0');
	}
	*pp = p;
	return nValue;
}

// Builds the descriptor from the field lines; returns its length (0 if nothing usable)
static int FieldsToDescriptor(int nTag, const char * szText, BYTE * pOut)
{
	int nLength = 2;
	const char * p = szText;

	pOut[0] = (BYTE)nTag;
	while (*p && nLength < 250)
	{
		char szLine[128];
		const char * q;
		int n = 0;

		while (*p && *p != '\r' && *p != '\n' && n < sizeof(szLine) - 1)
			szLine[n++] = *p++;
		szLine[n] = '\0';
		while (*p == '\r' || *p == '\n')
			p++;
		q = szLine;
		while (*q == ' ' || *q == '\t')
			q++;
		if (*q == '\0')
			continue;

		if (nTag == 0x52)
		{
			pOut[nLength++] = (BYTE)ParseNumber(&q);
			break;
		}
		// Every other editor starts with a 3-letter language code
		if (lstrlen(q) < 3)
			continue;
		pOut[nLength] = (BYTE)tolower(q[0]);
		pOut[nLength + 1] = (BYTE)tolower(q[1]);
		pOut[nLength + 2] = (BYTE)tolower(q[2]);
		q += 3;
		switch (nTag)
		{
		case 0x56:
		case 0x46:
			{
				int nType = ParseNumber(&q);
				int nPage;

				while (*q == ' ' || *q == '\t')
					q++;
				// page as written on screen, e.g. 699 or 888: magazine digit + two hex digits
				nPage = (int)strtoul(q, NULL, 16);
				pOut[nLength + 3] = (BYTE)(((nType & 0x1f) << 3) | ((nPage >> 8) & 7));
				pOut[nLength + 4] = (BYTE)nPage;
				nLength += 5;
			}
			break;
		case 0x59:
			{
				int nType = ParseNumber(&q);
				int nComposition = ParseNumber(&q);
				int nAncillary = ParseNumber(&q);

				pOut[nLength + 3] = (BYTE)nType;
				pOut[nLength + 4] = (BYTE)(nComposition >> 8);
				pOut[nLength + 5] = (BYTE)nComposition;
				pOut[nLength + 6] = (BYTE)(nAncillary >> 8);
				pOut[nLength + 7] = (BYTE)nAncillary;
				nLength += 8;
			}
			break;
		case 0x0a:
			pOut[nLength + 3] = (BYTE)ParseNumber(&q);
			nLength += 4;
			break;
		}
	}
	pOut[1] = (BYTE)(nLength - 2);
	return nLength;
}

// ---------------------------------------------------------------- dialog

static void ShowInfo(HWND hDlg, const char * szNote)
{
	char szName[128], szInfo[256];

	if (nCurrent < 2)
	{
		SetDlgItemText(hDlg, IDC_DH_INFO, szNote != NULL ? szNote : "");
		return;
	}
	szName[0] = '\0';
	DecodeDescriptorNames(szName, bCurrent[0]);
	wsprintf(szInfo, "Tag 0x%02X %s - %d bytes (length byte %d)%s%s", bCurrent[0], szName, nCurrent, bCurrent[1],
			 szNote != NULL ? "  " : "", szNote != NULL ? szNote : "");
	SetDlgItemText(hDlg, IDC_DH_INFO, szInfo);
	SetDlgItemText(hDlg, IDC_DH_FIELDS_HELP, FieldsHelp(bCurrent[0]));
	SendDlgItemMessage(hDlg, IDC_DH_FIELDS, EM_SETREADONLY, !TagHasFields(bCurrent[0]), 0);
}

static void WriteHexBox(HWND hDlg)
{
	char szHex[2048];

	FormatHex(bCurrent, nCurrent, szHex, sizeof(szHex));
	fUpdating = TRUE;
	SetDlgItemText(hDlg, IDC_DH_HEX, szHex);
	fUpdating = FALSE;
}

static void WriteFieldsBox(HWND hDlg)
{
	char szFields[2048];

	if (nCurrent >= 2 && TagHasFields(bCurrent[0]))
		DescriptorToFields(bCurrent, nCurrent, szFields, sizeof(szFields));
	else
		szFields[0] = '\0';
	fUpdating = TRUE;
	SetDlgItemText(hDlg, IDC_DH_FIELDS, szFields);
	fUpdating = FALSE;
}

static void LoadCurrent(HWND hDlg, const BYTE * pData, int nLength)
{
	memcpy(bCurrent, pData, nLength);
	nCurrent = nLength;
	WriteHexBox(hDlg);
	WriteFieldsBox(hDlg);
	ShowInfo(hDlg, NULL);
}

static void FillList(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_DH_LIST);
	int i;

	CollectDescriptors();
	SendMessage(hList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(hList);
	for (i = 0; i < nItems; i++)
	{
		LVITEM lvi;
		char szTemp[600], szName[128];
		int nSaveFormat = nFormat;

		memset(&lvi, 0, sizeof(lvi));
		lvi.mask = LVIF_TEXT | LVIF_PARAM;
		lvi.iItem = i;
		lvi.pszText = pItems[i].szTable;
		lvi.lParam = i;
		ListView_InsertItem(hList, &lvi);
		ListView_SetItemText(hList, i, 1, pItems[i].szOwner);
		wsprintf(szTemp, "0x%02X", pItems[i].bData[0]);
		ListView_SetItemText(hList, i, 2, szTemp);
		szName[0] = '\0';
		DecodeDescriptorNames(szName, pItems[i].bData[0]);
		ListView_SetItemText(hList, i, 3, szName);
		wsprintf(szTemp, "%d", pItems[i].nLength);
		ListView_SetItemText(hList, i, 4, szTemp);
		nFormat = 1;
		FormatHex(pItems[i].bData, pItems[i].nLength, szTemp, sizeof(szTemp));
		nFormat = nSaveFormat;
		ListView_SetItemText(hList, i, 5, szTemp);
	}
	SendMessage(hList, WM_SETREDRAW, TRUE, 0);
	{
		char szCount[64];
		wsprintf(szCount, "%d descriptors", nItems);
		SetDlgItemText(hDlg, IDC_DH_COUNT, szCount);
	}
}

static void CopyHex(HWND hDlg)
{
	char szHex[2048];
	HGLOBAL hMem;

	FormatHex(bCurrent, nCurrent, szHex, sizeof(szHex));
	if (!OpenClipboard(hDlg))
		return;
	EmptyClipboard();
	hMem = GlobalAlloc(GMEM_MOVEABLE, lstrlen(szHex) + 1);
	if (hMem != NULL)
	{
		lstrcpy(GlobalLock(hMem), szHex);
		GlobalUnlock(hMem);
		SetClipboardData(CF_TEXT, hMem);
	}
	CloseClipboard();
	ShowInfo(hDlg, "- copied");
}

BOOL CALLBACK DescriptorToolDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			HWND hList = GetDlgItem(hDlg, IDC_DH_LIST);
			static const char * szColumns[] = {"Table", "Owner", "Tag", "Name", "Bytes", "Hex"};
			static const int nWidths[] = {40, 250, 40, 130, 40, 330};
			static const char * szFormats[] = {"Continuous  (561473...)", "Spaced  (56 14 73 ...)", "0x prefix  (0x561473...)", "C array  (0x56, 0x14, ...)"};
			static const char * szNewTags[] = {"Teletext (0x56)", "Subtitling (0x59)", "ISO 639 language (0x0A)", "Stream identifier (0x52)"};
			LVCOLUMN lvc;
			int i;
			HFONT hFixed = CreateFont(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, 0, 0, 0, FIXED_PITCH, "Consolas");

			if (pItems == NULL)
				pItems = LocalAlloc(LPTR, sizeof(DHITEM) * DH_MAX_ITEMS);
			if (pItems == NULL)
			{
				EndDialog(hDlg, FALSE);
				return TRUE;
			}
			ListView_SetExtendedListViewStyle(hList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
			memset(&lvc, 0, sizeof(lvc));
			lvc.mask = LVCF_TEXT | LVCF_WIDTH;
			for (i = 0; i < 6; i++)
			{
				lvc.pszText = (char *)szColumns[i];
				lvc.cx = nWidths[i];
				ListView_InsertColumn(hList, i, &lvc);
			}
			for (i = 0; i < 4; i++)
				SendDlgItemMessage(hDlg, IDC_DH_FORMAT, CB_ADDSTRING, 0, (LPARAM)szFormats[i]);
			SendDlgItemMessage(hDlg, IDC_DH_FORMAT, CB_SETCURSEL, nFormat, 0);
			for (i = 0; i < 4; i++)
				SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_ADDSTRING, 0, (LPARAM)szNewTags[i]);
			SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_SETCURSEL, 0, 0);
			SendDlgItemMessage(hDlg, IDC_DH_HEX, WM_SETFONT, (WPARAM)hFixed, FALSE);
			SendDlgItemMessage(hDlg, IDC_DH_FIELDS, WM_SETFONT, (WPARAM)hFixed, FALSE);
			SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)hFixed);
			nCurrent = 0;
			FillList(hDlg);
			ShowInfo(hDlg, "Select a descriptor, or press New");
		}
		return TRUE;

	case WM_NOTIFY:
		if (((NMHDR *)lParam)->idFrom == IDC_DH_LIST && ((NMHDR *)lParam)->code == LVN_ITEMCHANGED)
		{
			NMLISTVIEW * pnm = (NMLISTVIEW *)lParam;

			if ((pnm->uNewState & LVIS_SELECTED) && !(pnm->uOldState & LVIS_SELECTED) && pnm->lParam >= 0 && pnm->lParam < nItems)
				LoadCurrent(hDlg, pItems[pnm->lParam].bData, pItems[pnm->lParam].nLength);
		}
		break;

	case WM_COMMAND:
		switch (LOWORD(wParam))
		{
		case IDC_DH_HEX:
			if (HIWORD(wParam) == EN_CHANGE && !fUpdating)
			{
				// Typed hex: take the bytes and fix the length byte to match
				char szHex[2048];
				BYTE bData[300];
				int nLength;

				GetDlgItemText(hDlg, IDC_DH_HEX, szHex, sizeof(szHex));
				nLength = ParseHex(szHex, bData, sizeof(bData));
				if (nLength > 257)
					nLength = 257;
				if (nLength >= 2)
				{
					BOOL fFixed = bData[1] != nLength - 2;
					bData[1] = (BYTE)(nLength - 2);
					memcpy(bCurrent, bData, nLength);
					nCurrent = nLength;
					WriteFieldsBox(hDlg);
					ShowInfo(hDlg, fFixed ? "- length byte set to match" : NULL);
				}
				else
				{
					nCurrent = 0;
					ShowInfo(hDlg, "A descriptor needs at least 2 bytes (tag and length)");
				}
			}
			break;
		case IDC_DH_FIELDS:
			if (HIWORD(wParam) == EN_CHANGE && !fUpdating && nCurrent >= 2 && TagHasFields(bCurrent[0]))
			{
				char szFields[2048];

				GetDlgItemText(hDlg, IDC_DH_FIELDS, szFields, sizeof(szFields));
				nCurrent = FieldsToDescriptor(bCurrent[0], szFields, bCurrent);
				WriteHexBox(hDlg);
				ShowInfo(hDlg, NULL);
			}
			break;
		case IDC_DH_FORMAT:
			if (HIWORD(wParam) == CBN_SELCHANGE)
			{
				nFormat = (int)SendDlgItemMessage(hDlg, IDC_DH_FORMAT, CB_GETCURSEL, 0, 0);
				if (nCurrent >= 2)
					WriteHexBox(hDlg);
			}
			break;
		case IDC_DH_NEW:
			{
				static const BYTE bTags[] = {0x56, 0x59, 0x0a, 0x52};
				static const char * szTemplates[] = {"fin 2 699\r\n", "fin 0x10 1 1\r\n", "fin 0\r\n", "0x01\r\n"};
				int nChoice = (int)SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_GETCURSEL, 0, 0);

				if (nChoice < 0 || nChoice > 3)
					nChoice = 0;
				nCurrent = FieldsToDescriptor(bTags[nChoice], szTemplates[nChoice], bCurrent);
				WriteHexBox(hDlg);
				WriteFieldsBox(hDlg);
				ShowInfo(hDlg, "- new: edit the fields");
				SetFocus(GetDlgItem(hDlg, IDC_DH_FIELDS));
			}
			break;
		case IDC_DH_COPY:
			if (nCurrent >= 2)
				CopyHex(hDlg);
			break;
		case IDC_DH_REFRESH:
			FillList(hDlg);
			break;
		case IDCANCEL:
		case IDOK:
			EndDialog(hDlg, TRUE);
			break;
		}
		break;

	case WM_DESTROY:
		{
			HFONT hFixed = (HFONT)GetWindowLongPtr(hDlg, DWLP_USER);
			if (hFixed != NULL)
				DeleteObject(hFixed);
		}
		break;
	}
	return FALSE;
}
