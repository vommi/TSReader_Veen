// Descriptor hex tool and editor (View > Descriptor Hex / Editor)
//
// Lists every descriptor TSReader keeps from the PMT (program and elementary streams),
// SDT, NIT, BAT and TOT, and shows the selected one as hex in the format a device's
// "add descriptor" field wants (e.g. continuous 561473776511...), ready to copy.
// The descriptor can be edited as hex or, when DescriptorBuilder.c has a definition for
// it, field by field in a form (named values in a list, repeating entries added and
// removed); the hex and the length byte follow. "New" builds any defined descriptor
// from scratch.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "TSReader.h"
#include "resource.h"
#include "DescriptorBuilder.h"

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

	// EIT: descriptors kept with the first events of each service (short event text is
	// decoded by the parser and not kept as a descriptor)
	EnterCriticalSection(&v->csEIT);
	for (i = 0; i < MAX_EIT_CHANNEL_DATA; i++)
	{
		PEITEVENT pEvent = v->pEvents[i];
		int nEvent;

		for (nEvent = 0; pEvent != NULL && nEvent < 3; nEvent++, pEvent = (PEITEVENT)pEvent->dwNextEvent)
		{
			_snprintf(szOwner, sizeof(szOwner) - 1, "Service %d / event %d %s", i, pEvent->nEventID, pEvent->szEventName);
			szOwner[sizeof(szOwner) - 1] = '\0';
			for (j = 0; j < MAX_EIT_EXTRA_DESCRIPTORS && pEvent->pExtraDescriptors[j] != NULL; j++)
				AddItem("EIT", szOwner, pEvent->pExtraDescriptors[j], 2 + pEvent->pExtraDescriptors[j][1]);
		}
	}
	LeaveCriticalSection(&v->csEIT);
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

// ---------------------------------------------------------------- form editor

static DBINSTANCE instance;			// the current descriptor decoded by its definition
static BOOL fDefined;				// instance is valid (a definition decoded it)
#define DH_MAX_ROWS		(DB_MAX_FIELDS + DB_MAX_ENTRIES * DB_MAX_LOOP_FIELDS)
static int nRowField[DH_MAX_ROWS], nRowEntry[DH_MAX_ROWS], nRows;
static int nSelectedRow = -1;

static void ShowInfo(HWND hDlg, const char * szNote)
{
	char szName[128], szInfo[300];

	if (nCurrent < 2)
	{
		SetDlgItemText(hDlg, IDC_DH_INFO, szNote != NULL ? szNote : "");
		return;
	}
	szName[0] = '\0';
	if (fDefined)
		lstrcpyn(szName, instance.pDef->szName, sizeof(szName));
	else
		DecodeDescriptorNames(szName, bCurrent[0]);
	wsprintf(szInfo, "Tag 0x%02X %s - %d bytes (length byte %d)%s%s", bCurrent[0], szName, nCurrent, bCurrent[1],
			 szNote != NULL ? "  " : "", szNote != NULL ? szNote : "");
	SetDlgItemText(hDlg, IDC_DH_INFO, szInfo);
}

static void WriteHexBox(HWND hDlg)
{
	char szHex[2048];

	FormatHex(bCurrent, nCurrent, szHex, sizeof(szHex));
	fUpdating = TRUE;
	SetDlgItemText(hDlg, IDC_DH_HEX, szHex);
	fUpdating = FALSE;
}

// Label of a form row: field name, with the entry number inside a repeating entry
static void RowLabel(int nRow, char * szOut, int nSize)
{
	const DBFIELD * pField = &instance.pDef->pFields[nRowField[nRow]];

	if (DB_InLoop(instance.pDef, nRowField[nRow]))
		_snprintf(szOut, nSize - 1, "#%d %s", nRowEntry[nRow] + 1, pField->szName);
	else
		_snprintf(szOut, nSize - 1, "%s", pField->szName);
	szOut[nSize - 1] = '\0';
}

static BOOL RowShown(int nField, int nEntry)
{
	int nType = instance.pDef->pFields[nField].nType;

	if (nType == DF_RESERVED || nType == DF_LOOP || nType == DF_LOOPEND)
		return FALSE;
	return DB_FieldPresent(&instance, nField, nEntry);
}

// Shows the editor controls for the selected row
static void ShowFieldEditor(HWND hDlg)
{
	HWND hCombo = GetDlgItem(hDlg, IDC_DH_VALUECOMBO);
	const DBFIELD * pField;
	DBVALUE * pValue;
	char szText[600], szLabel[128];
	BOOL fCombo;

	SendMessage(hCombo, CB_RESETCONTENT, 0, 0);
	if (!fDefined || nSelectedRow < 0 || nSelectedRow >= nRows)
	{
		SetDlgItemText(hDlg, IDC_DH_FIELDNAME, fDefined ? "Select a field to change it" : "");
		SetDlgItemText(hDlg, IDC_DH_FIELDHINT, "");
		ShowWindow(hCombo, SW_HIDE);
		EnableWindow(GetDlgItem(hDlg, IDC_DH_VALUEEDIT), FALSE);
		fUpdating = TRUE;
		SetDlgItemText(hDlg, IDC_DH_VALUEEDIT, "");
		fUpdating = FALSE;
		return;
	}
	pField = &instance.pDef->pFields[nRowField[nSelectedRow]];
	pValue = DB_Value(&instance, nRowField[nSelectedRow], nRowEntry[nSelectedRow]);
	RowLabel(nSelectedRow, szLabel, sizeof(szLabel));
	SetDlgItemText(hDlg, IDC_DH_FIELDNAME, szLabel);

	fCombo = pField->nType == DF_ENUM || pField->nType == DF_FLAG || pField->nType == DF_PRESENT;
	if (fCombo)
	{
		int nSelect = -1, nItem;

		if (pField->nType == DF_ENUM)
		{
			const DBENUM * pEnum;
			for (pEnum = pField->pEnum; pEnum != NULL && pEnum->szName != NULL; pEnum++)
			{
				if (pField->nBits >= 8)
					wsprintf(szText, "0x%02X  %s", pEnum->nValue, pEnum->szName);
				else
					wsprintf(szText, "%d  %s", pEnum->nValue, pEnum->szName);
				nItem = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)szText);
				SendMessage(hCombo, CB_SETITEMDATA, nItem, pEnum->nValue);
				if ((DWORD)pEnum->nValue == pValue->dwValue)
					nSelect = nItem;
			}
		}
		else
		{
			nItem = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"0  no");
			SendMessage(hCombo, CB_SETITEMDATA, nItem, 0);
			nItem = (int)SendMessage(hCombo, CB_ADDSTRING, 0, (LPARAM)"1  yes");
			SendMessage(hCombo, CB_SETITEMDATA, nItem, 1);
			nSelect = pValue->dwValue ? 1 : 0;
		}
		SendMessage(hCombo, CB_SETCURSEL, nSelect, 0);
		ShowWindow(hCombo, SW_SHOW);
	}
	else
		ShowWindow(hCombo, SW_HIDE);

	EnableWindow(GetDlgItem(hDlg, IDC_DH_VALUEEDIT), pField->nType != DF_PRESENT && pField->nType != DF_FLAG);
	DB_FormatValue(pField, pValue, szText, sizeof(szText));
	fUpdating = TRUE;
	SetDlgItemText(hDlg, IDC_DH_VALUEEDIT, szText);
	fUpdating = FALSE;

	if (pField->szHint != NULL)
		lstrcpyn(szText, pField->szHint, sizeof(szText));
	else if (pField->nType == DF_ENUM)
		lstrcpy(szText, "Choose from the list, or type a value");
	else
		szText[0] = '\0';
	if (pField->nType == DF_UINT || pField->nType == DF_HEX || pField->nType == DF_ENUM)
		wsprintf(szText + lstrlen(szText), "%s(%d bits, decimal or 0x hex)", szText[0] ? "\r\n" : "", pField->nBits);
	SetDlgItemText(hDlg, IDC_DH_FIELDHINT, szText);
}

// Rebuilds the form rows from the instance; keeps the selected field/entry selected
static void FillFieldList(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_DH_FIELDLIST);
	int nKeepField = -1, nKeepEntry = -1, nNewSelection = -1;
	int i, e, nLoop;

	if (nSelectedRow >= 0 && nSelectedRow < nRows)
	{
		nKeepField = nRowField[nSelectedRow];
		nKeepEntry = nRowEntry[nSelectedRow];
	}
	nRows = 0;
	nSelectedRow = -1;
	SendMessage(hList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(hList);
	if (!fDefined)
	{
		LVITEM lvi;

		memset(&lvi, 0, sizeof(lvi));
		lvi.mask = LVIF_TEXT;
		lvi.pszText = nCurrent >= 2 && DB_FindDefinition(bCurrent, nCurrent) != NULL
					? "(does not match its definition - edit the hex)"
					: "(no field definition for this descriptor - edit the hex)";
		if (nCurrent >= 2)
			ListView_InsertItem(hList, &lvi);
	}
	else
	{
		nLoop = DB_LoopStart(instance.pDef);
		for (i = 0; i < instance.pDef->nFields && i != nLoop; i++)
		{
			if (RowShown(i, 0) && nRows < DH_MAX_ROWS)
			{
				nRowField[nRows] = i;
				nRowEntry[nRows++] = 0;
			}
		}
		if (nLoop >= 0)
			for (e = 0; e < instance.nEntries; e++)
				for (i = nLoop + 1; instance.pDef->pFields[i].nType != DF_LOOPEND; i++)
					if (RowShown(i, e) && nRows < DH_MAX_ROWS)
					{
						nRowField[nRows] = i;
						nRowEntry[nRows++] = e;
					}
		for (i = 0; i < nRows; i++)
		{
			LVITEM lvi;
			char szText[600];
			const DBFIELD * pField = &instance.pDef->pFields[nRowField[i]];
			DBVALUE * pValue = DB_Value(&instance, nRowField[i], nRowEntry[i]);

			RowLabel(i, szText, sizeof(szText));
			memset(&lvi, 0, sizeof(lvi));
			lvi.mask = LVIF_TEXT;
			lvi.iItem = i;
			lvi.pszText = szText;
			ListView_InsertItem(hList, &lvi);
			DB_FormatValue(pField, pValue, szText, sizeof(szText));
			ListView_SetItemText(hList, i, 1, szText);
			DB_FormatMeaning(pField, pValue, szText, sizeof(szText));
			ListView_SetItemText(hList, i, 2, szText);
			if (nRowField[i] == nKeepField && nRowEntry[i] == nKeepEntry)
				nNewSelection = i;
		}
	}
	SendMessage(hList, WM_SETREDRAW, TRUE, 0);
	if (nNewSelection >= 0)
	{
		fUpdating = TRUE;
		ListView_SetItemState(hList, nNewSelection, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
		ListView_EnsureVisible(hList, nNewSelection, FALSE);
		fUpdating = FALSE;
		nSelectedRow = nNewSelection;
	}
	EnableWindow(GetDlgItem(hDlg, IDC_DH_ADDENTRY), fDefined && DB_LoopStart(instance.pDef) >= 0);
	EnableWindow(GetDlgItem(hDlg, IDC_DH_REMOVEENTRY), fDefined && nSelectedRow >= 0 && DB_InLoop(instance.pDef, nRowField[nSelectedRow]));
	ShowFieldEditor(hDlg);
}

// Decodes bCurrent with its definition (if there is one)
static void DecodeCurrent()
{
	const DBDEF * pDef = DB_FindDefinition(bCurrent, nCurrent);

	fDefined = pDef != NULL && DB_Decode(pDef, bCurrent, nCurrent, &instance);
}

// The form changed: encode it into bCurrent and update the hex and the form
static void InstanceChanged(HWND hDlg)
{
	BYTE bData[257];
	int nLength = DB_Encode(&instance, bData);

	if (nLength == 0)
	{
		ShowInfo(hDlg, "- too long for one descriptor (255 bytes)");
		return;
	}
	memcpy(bCurrent, bData, nLength);
	nCurrent = nLength;
	WriteHexBox(hDlg);
	FillFieldList(hDlg);
	ShowInfo(hDlg, NULL);
}

static void LoadCurrent(HWND hDlg, const BYTE * pData, int nLength)
{
	memcpy(bCurrent, pData, nLength);
	nCurrent = nLength;
	DecodeCurrent();
	nSelectedRow = -1;
	WriteHexBox(hDlg);
	FillFieldList(hDlg);
	ShowInfo(hDlg, NULL);
}

static void FillList(HWND hDlg)
{
	HWND hList = GetDlgItem(hDlg, IDC_DH_LIST);
	int i, nDecoded = 0, nDiffer = 0, nNotDecoded = 0;

	CollectDescriptors();
	SendMessage(hList, WM_SETREDRAW, FALSE, 0);
	ListView_DeleteAllItems(hList);
	for (i = 0; i < nItems; i++)
	{
		LVITEM lvi;
		char szTemp[600], szName[128];
		int nSaveFormat = nFormat;
		const DBDEF * pDef = DB_FindDefinition(pItems[i].bData, pItems[i].nLength);

		// Check the builder decodes and re-encodes it to the same bytes
		if (pDef != NULL && DB_Decode(pDef, pItems[i].bData, pItems[i].nLength, &instance))
		{
			BYTE bData[257];
			int nLength = DB_Encode(&instance, bData);

			nDecoded++;
			if (nLength != pItems[i].nLength || memcmp(bData, pItems[i].bData, nLength) != 0)
				nDiffer++;
		}
		else if (pDef != NULL)
			nNotDecoded++;

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
		char szCount[160];
		wsprintf(szCount, "%d descriptors, %d with field editor%s%s", nItems, nDecoded,
				 nDiffer ? ", some re-encode differently!" : "",
				 nNotDecoded ? ", some do not match their definition" : "");
		SetDlgItemText(hDlg, IDC_DH_COUNT, szCount);
	}
	// FillList used instance for the check: reload the current descriptor
	if (nCurrent >= 2)
		DecodeCurrent();
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

// Value typed or chosen for the selected field
static void ApplyValue(HWND hDlg, const char * szText)
{
	const DBFIELD * pField;
	DBVALUE value;

	if (!fDefined || nSelectedRow < 0 || nSelectedRow >= nRows)
		return;
	pField = &instance.pDef->pFields[nRowField[nSelectedRow]];
	value = *DB_Value(&instance, nRowField[nSelectedRow], nRowEntry[nSelectedRow]);
	if (!DB_ParseValue(pField, szText, &value))
	{
		ShowInfo(hDlg, "- not a valid value for this field");
		return;
	}
	*DB_Value(&instance, nRowField[nSelectedRow], nRowEntry[nSelectedRow]) = value;
	{
		// The edit box keeps what is typed; only the hex and the form follow
		char szKeep[600];
		GetDlgItemText(hDlg, IDC_DH_VALUEEDIT, szKeep, sizeof(szKeep));
		InstanceChanged(hDlg);
		fUpdating = TRUE;
		SetDlgItemText(hDlg, IDC_DH_VALUEEDIT, szKeep);
		SendDlgItemMessage(hDlg, IDC_DH_VALUEEDIT, EM_SETSEL, lstrlen(szKeep), lstrlen(szKeep));
		fUpdating = FALSE;
	}
}

static void AddEntry(HWND hDlg)
{
	int nLoop = DB_LoopStart(instance.pDef), nAfter, e;

	if (!fDefined || nLoop < 0 || instance.nEntries >= DB_MAX_ENTRIES)
		return;
	// New entry after the selected one, as a copy of it (or of the defaults)
	nAfter = instance.nEntries - 1;
	if (nSelectedRow >= 0 && DB_InLoop(instance.pDef, nRowField[nSelectedRow]))
		nAfter = nRowEntry[nSelectedRow];
	for (e = instance.nEntries; e > nAfter + 1; e--)
		memcpy(instance.loop[e], instance.loop[e - 1], sizeof(instance.loop[e]));
	if (nAfter >= 0)
		memcpy(instance.loop[nAfter + 1], instance.loop[nAfter], sizeof(instance.loop[0]));
	else
	{
		DBINSTANCE * pDefaults = LocalAlloc(LPTR, sizeof(DBINSTANCE));
		if (pDefaults == NULL)
			return;
		DB_New(instance.pDef, pDefaults);
		memcpy(instance.loop[0], pDefaults->loop[0], sizeof(instance.loop[0]));
		LocalFree(pDefaults);
	}
	instance.nEntries++;
	nSelectedRow = -1;
	InstanceChanged(hDlg);
}

static void RemoveEntry(HWND hDlg)
{
	int nEntry, e;

	if (!fDefined || nSelectedRow < 0 || !DB_InLoop(instance.pDef, nRowField[nSelectedRow]))
		return;
	nEntry = nRowEntry[nSelectedRow];
	for (e = nEntry; e < instance.nEntries - 1; e++)
		memcpy(instance.loop[e], instance.loop[e + 1], sizeof(instance.loop[e]));
	instance.nEntries--;
	nSelectedRow = -1;
	InstanceChanged(hDlg);
}

BOOL CALLBACK DescriptorToolDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_INITDIALOG:
		{
			HWND hList = GetDlgItem(hDlg, IDC_DH_LIST);
			HWND hFields = GetDlgItem(hDlg, IDC_DH_FIELDLIST);
			static const char * szColumns[] = {"Table", "Owner", "Tag", "Name", "Bytes", "Hex"};
			static const int nWidths[] = {40, 250, 40, 130, 40, 330};
			static const char * szFieldColumns[] = {"Field", "Value", "Meaning"};
			static const int nFieldWidths[] = {200, 90, 220};
			static const char * szFormats[] = {"Continuous  (561473...)", "Spaced  (56 14 73 ...)", "0x prefix  (0x561473...)", "C array  (0x56, 0x14, ...)"};
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
			ListView_SetExtendedListViewStyle(hFields, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
			memset(&lvc, 0, sizeof(lvc));
			lvc.mask = LVCF_TEXT | LVCF_WIDTH;
			for (i = 0; i < 6; i++)
			{
				lvc.pszText = (char *)szColumns[i];
				lvc.cx = nWidths[i];
				ListView_InsertColumn(hList, i, &lvc);
			}
			for (i = 0; i < 3; i++)
			{
				lvc.pszText = (char *)szFieldColumns[i];
				lvc.cx = nFieldWidths[i];
				ListView_InsertColumn(hFields, i, &lvc);
			}
			for (i = 0; i < 4; i++)
				SendDlgItemMessage(hDlg, IDC_DH_FORMAT, CB_ADDSTRING, 0, (LPARAM)szFormats[i]);
			SendDlgItemMessage(hDlg, IDC_DH_FORMAT, CB_SETCURSEL, nFormat, 0);
			for (i = 0; i < DB_DefinitionCount(); i++)
				SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_ADDSTRING, 0, (LPARAM)DB_Definition(i)->szName);
			SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_SETCURSEL, 0, 0);
			SendDlgItemMessage(hDlg, IDC_DH_HEX, WM_SETFONT, (WPARAM)hFixed, FALSE);
			SetWindowLongPtr(hDlg, DWLP_USER, (LONG_PTR)hFixed);
			nCurrent = 0;
			fDefined = FALSE;
			nSelectedRow = -1;
			FillList(hDlg);
			FillFieldList(hDlg);
			ShowInfo(hDlg, "Select a descriptor, or choose one under New descriptor and press New");
		}
		return TRUE;

	case WM_NOTIFY:
		{
			NMHDR * pHdr = (NMHDR *)lParam;

			if (pHdr->code != LVN_ITEMCHANGED)
				break;
			if (pHdr->idFrom == IDC_DH_LIST)
			{
				NMLISTVIEW * pnm = (NMLISTVIEW *)lParam;

				if ((pnm->uNewState & LVIS_SELECTED) && !(pnm->uOldState & LVIS_SELECTED) && pnm->lParam >= 0 && pnm->lParam < nItems)
					LoadCurrent(hDlg, pItems[pnm->lParam].bData, pItems[pnm->lParam].nLength);
			}
			else if (pHdr->idFrom == IDC_DH_FIELDLIST && !fUpdating)
			{
				NMLISTVIEW * pnm = (NMLISTVIEW *)lParam;

				if ((pnm->uNewState & LVIS_SELECTED) && !(pnm->uOldState & LVIS_SELECTED) && fDefined && pnm->iItem < nRows)
				{
					nSelectedRow = pnm->iItem;
					EnableWindow(GetDlgItem(hDlg, IDC_DH_REMOVEENTRY), DB_InLoop(instance.pDef, nRowField[nSelectedRow]));
					ShowFieldEditor(hDlg);
				}
			}
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
					DecodeCurrent();
					FillFieldList(hDlg);
					ShowInfo(hDlg, fFixed ? "- length byte set to match" : NULL);
				}
				else
				{
					nCurrent = 0;
					fDefined = FALSE;
					FillFieldList(hDlg);
					ShowInfo(hDlg, "A descriptor needs at least 2 bytes (tag and length)");
				}
			}
			break;
		case IDC_DH_VALUEEDIT:
			if (HIWORD(wParam) == EN_CHANGE && !fUpdating)
			{
				char szValue[600];

				GetDlgItemText(hDlg, IDC_DH_VALUEEDIT, szValue, sizeof(szValue));
				ApplyValue(hDlg, szValue);
			}
			break;
		case IDC_DH_VALUECOMBO:
			if (HIWORD(wParam) == CBN_SELCHANGE && fDefined && nSelectedRow >= 0)
			{
				int nItem = (int)SendDlgItemMessage(hDlg, IDC_DH_VALUECOMBO, CB_GETCURSEL, 0, 0);

				if (nItem >= 0)
				{
					DWORD dwValue = (DWORD)SendDlgItemMessage(hDlg, IDC_DH_VALUECOMBO, CB_GETITEMDATA, nItem, 0);
					DB_Value(&instance, nRowField[nSelectedRow], nRowEntry[nSelectedRow])->dwValue = dwValue;
					InstanceChanged(hDlg);
				}
			}
			break;
		case IDC_DH_ADDENTRY:
			AddEntry(hDlg);
			break;
		case IDC_DH_REMOVEENTRY:
			RemoveEntry(hDlg);
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
				const DBDEF * pDef = DB_Definition((int)SendDlgItemMessage(hDlg, IDC_DH_NEWTAG, CB_GETCURSEL, 0, 0));

				if (pDef == NULL)
					break;
				DB_New(pDef, &instance);
				fDefined = TRUE;
				nSelectedRow = -1;
				InstanceChanged(hDlg);
				ShowInfo(hDlg, "- new: select a field to change it");
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
