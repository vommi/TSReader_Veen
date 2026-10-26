// Teletext subtitles for the embedded FFplay player
//
// The FFmpeg build has no teletext decoder (that needs libzvbi), but it has a DVB
// subtitle decoder. So for the embedded player TSReader decodes each teletext subtitle
// page (EN 300 706) of the streamed program itself and re-sends it as a DVB subtitle
// stream (EN 300 743) on a PID of its own, with the teletext PES's PTS. ffplay then shows
// teletext subtitles exactly like DVB subtitles: in sync, scaled with the video and in
// full screen, and the video window's Subtitles menu lists one track per page/language.
//
// The streamed PMT lists the converted pages instead of the teletext PID (ffplay could
// not decode that anyway), see SetupPriorToProgramRecording. Teletext packets are fed in
// from BufferRecordChannelData and the DVB subtitle packets go out through the same
// record buffer. All of it runs on the stream processing thread.

#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "TSReader.h"

extern PVARIABLES v;

#define TTXSUB_MAX_PAGES		8
#define TTXSUB_FIFO_PACKETS		1536
#define TTXSUB_CANVAS_WIDTH		1280		// subtitles are drawn on a 1280x720 display (DDS), which
#define TTXSUB_CANVAS_HEIGHT	720			// keeps a display set well inside one PES (64 KB)
#define TTXSUB_FONT_HEIGHT		34
#define TTXSUB_BOX_PADDING_X	10
#define TTXSUB_BOX_PADDING_Y	3
#define TTXSUB_MARGIN_X			14
#define TTXSUB_MARGIN_BOTTOM	20
#define TTXSUB_MAX_OBJECT_BYTES	56000
#define TTXSUB_FIRST_PID		0x1f00

typedef struct
{
	int nSourcePID;					// teletext PID
	int nPage;						// magazine << 8 | page, e.g. 0x699
	char szLanguage[4];
	int nTeletextType;				// 2 subtitle, 5 subtitle for the hard of hearing
	int nOutPID;					// DVB subtitle PID sent to ffplay
	int nContinuity;
	int nVersion;					// DVB page/region/CLUT/object version
	BYTE bRows[25][40];				// page as received (7-bit, parity removed)
	BOOL fRowUsed[25];
	int nNationalOption;
	DWORD64 lnPTS;
	char szShown[1024];				// text now on screen, to skip repeats
} TTXSUBPAGE;

typedef struct
{
	int nPID;
	BYTE bPES[8192];
	int nFill;
	TTXSUBPAGE * pReceiving[9];		// page being received per magazine (1..8)
} TTXSUBSOURCE;

static TTXSUBPAGE pages[TTXSUB_MAX_PAGES];
static int nPages;
static TTXSUBSOURCE sources[2];
static int nSources;
static BYTE * pFifo;				// TS packets waiting to go into the record buffer
static int nFifoRead, nFifoCount;
static BYTE bReverse[256];
static signed char cHamming84[256];

// ---------------------------------------------------------------- setup and PMT

static BYTE * FindDescriptor(PESLIST pES, int nTag, int * pnLength)
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

// TRUE when an ES is teletext with at least one subtitle page (teletext descriptor type 2 or 5)
BOOL TtxSubs_HasSubtitlePages(int nPMTIndex, int nESIndex)
{
	int nLength, i;
	BYTE * pDesc = FindDescriptor(&v->pat.pmt[nPMTIndex].es[nESIndex], 0x56, &nLength);

	if (pDesc == NULL)
		return FALSE;
	for (i = 0; i + 5 <= nLength; i += 5)
	{
		int nType = pDesc[i + 3] >> 3;
		if (nType == 2 || nType == 5)
			return TRUE;
	}
	return FALSE;
}

static BOOL PIDInUse(int nPID)
{
	int i;

	if (nPID == v->nRecordVideoPID || nPID == v->nRecordPCRPID || nPID == v->nRecordPMTPID)
		return TRUE;
	for (i = 0; i < MAX_AUDIO_STREAMS; i++)
		if (v->nRecordAudioPID[i] == nPID)
			return TRUE;
	for (i = 0; i < MAX_OTHER_STREAMS; i++)
		if (v->nRecordOtherPID[i] == nPID)
			return TRUE;
	for (i = 0; i < nPages; i++)
		if (pages[i].nOutPID == nPID)
			return TRUE;
	return FALSE;
}

static void InitTables()
{
	int i;

	for (i = 0; i < 256; i++)
	{
		int b, r = 0;
		for (b = 0; b < 8; b++)
			if (i & (1 << b))
				r |= 0x80 >> b;
		bReverse[i] = (BYTE)r;
		cHamming84[i] = -1;
	}
	// Hamming 8/4 (EN 300 706 8.2): bits P1 D1 P2 D2 P3 D3 P4 D4, LSB first
	for (i = 0; i < 16; i++)
	{
		int d1 = i & 1, d2 = (i >> 1) & 1, d3 = (i >> 2) & 1, d4 = (i >> 3) & 1;
		int p1 = 1 ^ d1 ^ d3 ^ d4;
		int p2 = 1 ^ d1 ^ d2 ^ d4;
		int p3 = 1 ^ d1 ^ d2 ^ d3;
		int p4 = 1 ^ p1 ^ d1 ^ p2 ^ d2 ^ p3 ^ d3 ^ d4;
		int nCode = p1 | (d1 << 1) | (p2 << 2) | (d2 << 3) | (p3 << 4) | (d3 << 5) | (p4 << 6) | (d4 << 7);
		int b;

		cHamming84[nCode] = (signed char)i;
		// single bit errors are corrected
		for (b = 0; b < 8; b++)
			if (cHamming84[nCode ^ (1 << b)] == -1)
				cHamming84[nCode ^ (1 << b)] = (signed char)i;
	}
}

// Finds the teletext subtitle pages of the streamed program and gives each its own PID.
// Called while the streamed PAT/PMT are set up; converts nothing unless the embedded
// ffplay player is the stream's destination.
void TtxSubs_Setup()
{
	int i, j;

	nPages = 0;
	nSources = 0;
	nFifoRead = nFifoCount = 0;
	if (!v->fFFplayEmbedded || v->nStreamTo != STREAM_TO_VLC || v->nSelectedProgram < 0)
		return;
	if (pFifo == NULL)
	{
		pFifo = LocalAlloc(LPTR, TTXSUB_FIFO_PACKETS * 188);
		InitTables();
	}
	if (pFifo == NULL)
		return;

	for (i = 0; i < MAX_OTHER_STREAMS; i++)
	{
		int nES = v->nRecordOtherESIndex[i];
		int nLength;
		BYTE * pDesc;

		if (nES == -1 || v->nRecordOtherPID[i] == 0x1fff)
			continue;
		pDesc = FindDescriptor(&v->pat.pmt[v->nSelectedProgram].es[nES], 0x56, &nLength);
		if (pDesc == NULL || !TtxSubs_HasSubtitlePages(v->nSelectedProgram, nES) || nSources >= 2)
			continue;
		memset(&sources[nSources], 0, sizeof(TTXSUBSOURCE));
		sources[nSources].nPID = v->nRecordOtherPID[i];
		nSources++;
		for (j = 0; j + 5 <= nLength && nPages < TTXSUB_MAX_PAGES; j += 5)
		{
			int nType = pDesc[j + 3] >> 3;
			int nMagazine = pDesc[j + 3] & 7;
			TTXSUBPAGE * pPage;
			int nPID;

			if (nType != 2 && nType != 5)
				continue;
			pPage = &pages[nPages];
			memset(pPage, 0, sizeof(TTXSUBPAGE));
			pPage->nSourcePID = v->nRecordOtherPID[i];
			pPage->nPage = ((nMagazine ? nMagazine : 8) << 8) | pDesc[j + 4];
			pPage->szLanguage[0] = (char)tolower(pDesc[j]);
			pPage->szLanguage[1] = (char)tolower(pDesc[j + 1]);
			pPage->szLanguage[2] = (char)tolower(pDesc[j + 2]);
			pPage->nTeletextType = nType;
			for (nPID = TTXSUB_FIRST_PID; nPID < 0x1ffe && PIDInUse(nPID); nPID++)
				;
			pPage->nOutPID = nPID;
			nPages++;
		}
	}
}

BOOL TtxSubs_IsSourcePID(int nPID)
{
	int i;

	for (i = 0; i < nSources; i++)
		if (sources[i].nPID == nPID)
			return TRUE;
	return FALSE;
}

// PMT ES entries (stream type 0x06 + subtitling descriptor) replacing teletext PID nPID;
// returns their length. pBuffer may be NULL to just get the length.
int TtxSubs_GetPMTEntries(int nPID, BYTE * pBuffer)
{
	int i, nLength = 0;

	for (i = 0; i < nPages; i++)
	{
		if (pages[i].nSourcePID != nPID)
			continue;
		if (pBuffer != NULL)
		{
			BYTE * p = &pBuffer[nLength];
			p[0] = 0x06;									// stream type: private PES
			p[1] = 0xe0 | (BYTE)(pages[i].nOutPID >> 8);
			p[2] = (BYTE)pages[i].nOutPID;
			p[3] = 0xf0;
			p[4] = 10;										// ES info length
			p[5] = 0x59;									// subtitling descriptor
			p[6] = 8;
			p[7] = pages[i].szLanguage[0];
			p[8] = pages[i].szLanguage[1];
			p[9] = pages[i].szLanguage[2];
			p[10] = pages[i].nTeletextType == 5 ? 0x20 : 0x10;	// (hard of hearing) DVB subtitles, no aspect ratio
			p[11] = 0x00;									// composition page 1
			p[12] = 0x01;
			p[13] = 0x00;									// ancillary page 1
			p[14] = 0x01;
		}
		nLength += 15;
	}
	return nLength;
}

// Track list for the video window: converted pages in PMT order
int TtxSubs_GetTracks(int nSourcePID, int * pnPIDs, char szLanguages[][4], int * pnPageNumbers, BOOL * pfHardOfHearing, int nMax)
{
	int i, nCount = 0;

	for (i = 0; i < nPages && nCount < nMax; i++)
	{
		if (pages[i].nSourcePID != nSourcePID)
			continue;
		pnPIDs[nCount] = pages[i].nOutPID;
		lstrcpy(szLanguages[nCount], pages[i].szLanguage);
		pnPageNumbers[nCount] = pages[i].nPage;
		pfHardOfHearing[nCount] = pages[i].nTeletextType == 5;
		nCount++;
	}
	return nCount;
}

// ---------------------------------------------------------------- DVB subtitle output

typedef struct
{
	BYTE * p;
	int nBytes;
	int nBits;						// bits used in the current byte
} BITWRITER;

static void PutBits(BITWRITER * bw, int nValue, int nCount)
{
	while (nCount-- > 0)
	{
		if (bw->nBits == 0)
			bw->p[bw->nBytes] = 0;
		if ((nValue >> nCount) & 1)
			bw->p[bw->nBytes] |= 0x80 >> bw->nBits;
		if (++bw->nBits == 8)
		{
			bw->nBits = 0;
			bw->nBytes++;
		}
	}
}

static void ByteAlign(BITWRITER * bw)
{
	if (bw->nBits)
	{
		bw->nBits = 0;
		bw->nBytes++;
	}
}

// 4-bit/pixel code string run (EN 300 743 7.2.5.2.2)
static void PutRun(BITWRITER * bw, int nColour, int nRun)
{
	while (nRun > 0)
	{
		if (nRun >= 25)
		{
			int n = nRun > 280 ? 280 : nRun;
			PutBits(bw, 0, 4); PutBits(bw, 3, 2); PutBits(bw, 3, 2); PutBits(bw, n - 25, 8); PutBits(bw, nColour, 4);
			nRun -= n;
		}
		else if (nRun >= 9)
		{
			PutBits(bw, 0, 4); PutBits(bw, 3, 2); PutBits(bw, 2, 2); PutBits(bw, nRun - 9, 4); PutBits(bw, nColour, 4);
			nRun = 0;
		}
		else if (nColour == 0)
		{
			if (nRun >= 3)
			{
				PutBits(bw, 0, 4); PutBits(bw, 0, 1); PutBits(bw, nRun - 2, 3);
			}
			else
			{
				PutBits(bw, 0, 4); PutBits(bw, 3, 2); PutBits(bw, nRun == 2 ? 1 : 0, 2);
			}
			nRun = 0;
		}
		else if (nRun >= 4)
		{
			int n = nRun > 7 ? 7 : nRun;		// this code takes runs of 4..7 (8 = 7 + 1)
			PutBits(bw, 0, 4); PutBits(bw, 2, 2); PutBits(bw, n - 4, 2); PutBits(bw, nColour, 4);
			nRun -= n;
		}
		else
		{
			PutBits(bw, nColour, 4);
			nRun--;
		}
	}
}

// One field of the object: every second line of the 4-bit pixel map
static int EncodeField(BYTE * pOut, const BYTE * pPixels, int nWidth, int nHeight, int nFirstLine)
{
	BITWRITER bw;
	int y;

	bw.p = pOut;
	bw.nBytes = bw.nBits = 0;
	for (y = nFirstLine; y < nHeight; y += 2)
	{
		const BYTE * pLine = &pPixels[y * nWidth];
		int x = 0;

		PutBits(&bw, 0x11, 8);						// 4-bit/pixel code string
		while (x < nWidth)
		{
			int nRun = 1;
			while (x + nRun < nWidth && pLine[x + nRun] == pLine[x])
				nRun++;
			PutRun(&bw, pLine[x], nRun);
			x += nRun;
		}
		PutBits(&bw, 0, 8);							// end of string
		ByteAlign(&bw);
		PutBits(&bw, 0xf0, 8);						// end of object line
	}
	return bw.nBytes;
}

static void QueuePacket(const BYTE * pPacket)
{
	if (nFifoCount >= TTXSUB_FIFO_PACKETS)
		return;			// player not reading: drop
	memcpy(&pFifo[((nFifoRead + nFifoCount) % TTXSUB_FIFO_PACKETS) * 188], pPacket, 188);
	nFifoCount++;
}

// Splits a PES into TS packets on the page's PID; the last one is padded with an
// adaptation field
static void PacketizePES(TTXSUBPAGE * pPage, const BYTE * pPES, int nLength)
{
	int nOffset = 0;
	BYTE bPacket[188];

	while (nOffset < nLength)
	{
		int nPayload = nLength - nOffset;
		int nHeader = 4;

		bPacket[0] = 0x47;
		bPacket[1] = (BYTE)((nOffset == 0 ? 0x40 : 0x00) | ((pPage->nOutPID >> 8) & 0x1f));
		bPacket[2] = (BYTE)pPage->nOutPID;
		if (nPayload >= 184)
		{
			nPayload = 184;
			bPacket[3] = (BYTE)(0x10 | pPage->nContinuity);
		}
		else
		{
			int nStuffing = 184 - nPayload;		// adaptation field: length byte + flags + stuffing

			bPacket[3] = (BYTE)(0x30 | pPage->nContinuity);
			bPacket[4] = (BYTE)(nStuffing - 1);
			if (nStuffing > 1)
			{
				bPacket[5] = 0x00;
				memset(&bPacket[6], 0xff, nStuffing - 2);
			}
			nHeader += nStuffing;
		}
		memcpy(&bPacket[nHeader], &pPES[nOffset], nPayload);
		nOffset += nPayload;
		pPage->nContinuity = (pPage->nContinuity + 1) & 0x0f;
		QueuePacket(bPacket);
	}
}

static int PutSegment(BYTE * p, int nType, const BYTE * pData, int nLength)
{
	p[0] = 0x0f;
	p[1] = (BYTE)nType;
	p[2] = 0x00;				// page id 1
	p[3] = 0x01;
	p[4] = (BYTE)(nLength >> 8);
	p[5] = (BYTE)nLength;
	if (nLength)
		memcpy(&p[6], pData, nLength);
	return 6 + nLength;
}

static void RGBToYCrCb(int r, int g, int b, BYTE * pY, BYTE * pCr, BYTE * pCb)
{
	// ITU-R BT.601, limited range (FFmpeg converts DVB subtitle CLUTs with the CCIR formulas)
	int y = 16 + (65 * r + 129 * g + 25 * b + 128) / 256;
	int cr = 128 + (112 * r - 94 * g - 18 * b + 128) / 256;
	int cb = 128 + (-38 * r - 74 * g + 112 * b + 128) / 256;
	*pY = (BYTE)(y < 16 ? 16 : y);
	*pCr = (BYTE)cr;
	*pCb = (BYTE)cb;
}

// Colour groups of the 16-entry CLUT: 0 transparent, 1 box black, then three anti-alias
// steps per text colour (white, yellow, cyan, green) and red and magenta at full only
static const int nGroupRGB[6][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0}, {255, 0, 0}, {255, 0, 255}};
static const int nGroupBase[6] = {2, 5, 8, 11, 14, 15};

static int TeletextColourGroup(int nTeletextColour)
{
	switch (nTeletextColour)
	{
	case 1: return 4;		// red
	case 2: return 3;		// green
	case 3: return 1;		// yellow
	case 4:					// blue: hard to read on black, use cyan
	case 6: return 2;		// cyan
	case 5: return 5;		// magenta
	default: return 0;		// white (and black text)
	}
}

typedef struct
{
	char szText[64];
	int nRow;
	int nColour;			// colour group
	int nCentreColumn;		// teletext column of the text's centre * 2
} SUBLINE;

// Builds and queues one DVB subtitle display set (an empty one clears the screen)
static void SendDisplaySet(TTXSUBPAGE * pPage, SUBLINE * pLines, int nLines)
{
	static BYTE bPES[60000];
	static BYTE bSegment[400000];
	BYTE bData[64];
	BYTE * pPixels = NULL;
	int nPES, nRegionX = 0, nRegionY = 0, nRegionW = 0, nRegionH = 0;
	int nSegmentLength, i;
	RECT rcBoxes[24];

	pPage->nVersion = (pPage->nVersion + 1) & 0x0f;

	if (nLines)
	{
		// Lay the lines out on the 1920x1080 display: teletext rows give the height,
		// the text centre the horizontal position
		HDC hDC = CreateCompatibleDC(NULL);
		HFONT hFont = CreateFont(-TTXSUB_FONT_HEIGHT, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, ANSI_CHARSET,
								 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, "Arial");
		HFONT hOldFont = SelectObject(hDC, hFont);
		int nLeft = TTXSUB_CANVAS_WIDTH, nTop = TTXSUB_CANVAS_HEIGHT, nRight = 0, nBottom = 0;
		int nLineHeight = TTXSUB_FONT_HEIGHT + 2 * TTXSUB_BOX_PADDING_Y + 4;
		BITMAPINFO bmi;
		DWORD * pDIB;
		HBITMAP hBitmap, hOldBitmap;
		int nPreviousBottom = 0;

		for (i = 0; i < nLines; i++)
		{
			SIZE size;
			int nCentre = pLines[i].nCentreColumn * TTXSUB_CANVAS_WIDTH / 80;
			int nY = (pLines[i].nRow * TTXSUB_CANVAS_HEIGHT) / 25 - 13;

			GetTextExtentPoint32(hDC, pLines[i].szText, lstrlen(pLines[i].szText), &size);
			if (nY < nPreviousBottom)
				nY = nPreviousBottom;			// keep lines apart whatever the row numbers say
			if (nY + nLineHeight > TTXSUB_CANVAS_HEIGHT - TTXSUB_MARGIN_BOTTOM)
				nY = TTXSUB_CANVAS_HEIGHT - TTXSUB_MARGIN_BOTTOM - nLineHeight;
			rcBoxes[i].left = nCentre - size.cx / 2 - TTXSUB_BOX_PADDING_X;
			rcBoxes[i].right = rcBoxes[i].left + size.cx + 2 * TTXSUB_BOX_PADDING_X;
			if (rcBoxes[i].left < TTXSUB_MARGIN_X)
				OffsetRect(&rcBoxes[i], TTXSUB_MARGIN_X - rcBoxes[i].left, 0);
			if (rcBoxes[i].right > TTXSUB_CANVAS_WIDTH - TTXSUB_MARGIN_X)
				OffsetRect(&rcBoxes[i], TTXSUB_CANVAS_WIDTH - TTXSUB_MARGIN_X - rcBoxes[i].right, 0);
			rcBoxes[i].top = nY;
			rcBoxes[i].bottom = nY + TTXSUB_FONT_HEIGHT + 2 * TTXSUB_BOX_PADDING_Y;
			nPreviousBottom = rcBoxes[i].bottom + 4;
			nLeft = min(nLeft, rcBoxes[i].left);
			nRight = max(nRight, rcBoxes[i].right);
			nTop = min(nTop, rcBoxes[i].top);
			nBottom = max(nBottom, rcBoxes[i].bottom);
		}
		// Moving lines up after the last one hit the bottom margin can make them overlap
		if (nBottom > TTXSUB_CANVAS_HEIGHT - TTXSUB_MARGIN_BOTTOM)
		{
			int nShift = nBottom - (TTXSUB_CANVAS_HEIGHT - TTXSUB_MARGIN_BOTTOM);
			for (i = 0; i < nLines; i++)
				OffsetRect(&rcBoxes[i], 0, -nShift);
			nTop -= nShift;
			nBottom -= nShift;
		}
		nRegionX = nLeft & ~1;
		nRegionY = nTop & ~1;
		nRegionW = ((nRight - nRegionX) + 1) & ~1;
		nRegionH = ((nBottom - nRegionY) + 1) & ~1;

		memset(&bmi, 0, sizeof(bmi));
		bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		bmi.bmiHeader.biWidth = nRegionW;
		bmi.bmiHeader.biHeight = -nRegionH;		// top-down
		bmi.bmiHeader.biPlanes = 1;
		bmi.bmiHeader.biBitCount = 32;
		bmi.bmiHeader.biCompression = BI_RGB;
		hBitmap = CreateDIBSection(hDC, &bmi, DIB_RGB_COLORS, (void **)&pDIB, NULL, 0);
		pPixels = LocalAlloc(LPTR, nRegionW * nRegionH);
		if (hBitmap != NULL && pPixels != NULL)
		{
			int x, y;

			hOldBitmap = SelectObject(hDC, hBitmap);
			memset(pDIB, 0, nRegionW * nRegionH * 4);
			SetBkMode(hDC, TRANSPARENT);
			SetTextColor(hDC, RGB(255, 255, 255));
			for (i = 0; i < nLines; i++)
			{
				RECT rc = rcBoxes[i];
				OffsetRect(&rc, -nRegionX, -nRegionY);
				DrawText(hDC, pLines[i].szText, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
			}
			GdiFlush();
			// Quantize: outside the boxes transparent, inside black plus 3 text levels
			for (i = 0; i < nLines; i++)
			{
				int nGroup = pLines[i].nColour;
				RECT rc = rcBoxes[i];
				OffsetRect(&rc, -nRegionX, -nRegionY);
				for (y = max(rc.top, 0); y < min(rc.bottom, nRegionH); y++)
				{
					for (x = max(rc.left, 0); x < min(rc.right, nRegionW); x++)
					{
						int nLevel = ((pDIB[y * nRegionW + x] >> 8) & 0xff) * 3 / 255;	// green channel 0..255
						BYTE bIndex = 1;
						if (nLevel > 0)
						{
							if (nGroup >= 4)
								bIndex = nLevel >= 2 ? (BYTE)nGroupBase[nGroup] : 1;
							else
								bIndex = (BYTE)(nGroupBase[nGroup] + nLevel - 1);
						}
						pPixels[y * nRegionW + x] = bIndex;
					}
				}
			}
			SelectObject(hDC, hOldBitmap);
		}
		if (hBitmap != NULL)
			DeleteObject(hBitmap);
		SelectObject(hDC, hOldFont);
		DeleteObject(hFont);
		DeleteDC(hDC);
		if (pPixels == NULL || hBitmap == NULL)
		{
			if (pPixels != NULL)
				LocalFree(pPixels);
			pPixels = NULL;
			nLines = 0;
		}
	}

	// PES header with the teletext page's PTS
	bPES[0] = 0x00; bPES[1] = 0x00; bPES[2] = 0x01; bPES[3] = 0xbd;
	bPES[6] = 0x81;				// '10', data alignment
	bPES[7] = 0x80;				// PTS only
	bPES[8] = 5;
	bPES[9] = (BYTE)(0x21 | ((pPage->lnPTS >> 29) & 0x0e));
	bPES[10] = (BYTE)(pPage->lnPTS >> 22);
	bPES[11] = (BYTE)(0x01 | ((pPage->lnPTS >> 14) & 0xfe));
	bPES[12] = (BYTE)(pPage->lnPTS >> 7);
	bPES[13] = (BYTE)(0x01 | ((pPage->lnPTS << 1) & 0xfe));
	nPES = 14;
	bPES[nPES++] = 0x20;		// data identifier: DVB subtitles
	bPES[nPES++] = 0x00;		// subtitle stream id

	// Display definition: 1920x1080
	bData[0] = (BYTE)(pPage->nVersion << 4) | 0x07;
	bData[1] = (TTXSUB_CANVAS_WIDTH - 1) >> 8;
	bData[2] = (BYTE)(TTXSUB_CANVAS_WIDTH - 1);
	bData[3] = (TTXSUB_CANVAS_HEIGHT - 1) >> 8;
	bData[4] = (BYTE)(TTXSUB_CANVAS_HEIGHT - 1);
	nPES += PutSegment(&bPES[nPES], 0x14, bData, 5);

	// Page composition: mode change, one region (or none to clear)
	bData[0] = 60;				// time-out (s); a newer display set replaces it sooner
	bData[1] = (BYTE)((pPage->nVersion << 4) | (2 << 2) | 0x03);
	nSegmentLength = 2;
	if (nLines)
	{
		bData[2] = 0;			// region 0
		bData[3] = 0xff;
		bData[4] = (BYTE)(nRegionX >> 8);
		bData[5] = (BYTE)nRegionX;
		bData[6] = (BYTE)(nRegionY >> 8);
		bData[7] = (BYTE)nRegionY;
		nSegmentLength = 8;
	}
	nPES += PutSegment(&bPES[nPES], 0x10, bData, nSegmentLength);

	if (nLines)
	{
		int nTop, nBottom;

		// Region composition: 4-bit, transparent fill, one object at 0,0
		bData[0] = 0;
		bData[1] = (BYTE)((pPage->nVersion << 4) | 0x08 | 0x07);
		bData[2] = (BYTE)(nRegionW >> 8);
		bData[3] = (BYTE)nRegionW;
		bData[4] = (BYTE)(nRegionH >> 8);
		bData[5] = (BYTE)nRegionH;
		bData[6] = (2 << 5) | (2 << 2) | 0x03;	// 4-bit compatibility and depth
		bData[7] = 0;							// CLUT 0
		bData[8] = 0;
		bData[9] = 0x03;						// 4-bit fill colour 0 (transparent)
		bData[10] = 0x00;						// object 0
		bData[11] = 0x00;
		bData[12] = 0x00;						// bitmap object, x 0
		bData[13] = 0x00;
		bData[14] = 0xf0;						// y 0
		bData[15] = 0x00;
		nPES += PutSegment(&bPES[nPES], 0x11, bData, 16);

		// CLUT: 16 4-bit entries
		{
			BYTE bCLUT[2 + 16 * 6];
			int nEntry, nCLUT = 2;

			bCLUT[0] = 0;
			bCLUT[1] = (BYTE)((pPage->nVersion << 4) | 0x0f);
			for (nEntry = 0; nEntry < 16; nEntry++)
			{
				int r = 0, g = 0, b = 0, t = 0;
				int nGroup;

				if (nEntry == 0)
					t = 0xff;						// transparent
				else if (nEntry == 1)
					t = 0x40;						// box: black, slightly see-through
				else
				{
					for (nGroup = 0; nGroup < 6; nGroup++)
					{
						int nSteps = nGroup >= 4 ? 1 : 3;
						if (nEntry >= nGroupBase[nGroup] && nEntry < nGroupBase[nGroup] + nSteps)
						{
							int nLevel = nEntry - nGroupBase[nGroup] + 1;	// 1..3 of 3
							if (nGroup >= 4)
								nLevel = 3;
							r = nGroupRGB[nGroup][0] * nLevel / 3;
							g = nGroupRGB[nGroup][1] * nLevel / 3;
							b = nGroupRGB[nGroup][2] * nLevel / 3;
						}
					}
				}
				bCLUT[nCLUT++] = (BYTE)nEntry;
				bCLUT[nCLUT++] = 0x5f;				// 4-bit entry, full range
				RGBToYCrCb(r, g, b, &bCLUT[nCLUT], &bCLUT[nCLUT + 1], &bCLUT[nCLUT + 2]);
				if (nEntry == 0)
					bCLUT[nCLUT] = 0;				// Y 0: fully transparent
				nCLUT += 3;
				bCLUT[nCLUT++] = (BYTE)t;
			}
			nPES += PutSegment(&bPES[nPES], 0x12, bCLUT, nCLUT);
		}

		// Object data: top field then bottom field, 4-bit run-length coded
		bSegment[0] = 0x00;					// object 0
		bSegment[1] = 0x00;
		bSegment[2] = (BYTE)((pPage->nVersion << 4) | 0x01);	// pixels, no non-modifying colour
		nTop = EncodeField(&bSegment[7], pPixels, nRegionW, nRegionH, 0);
		nBottom = EncodeField(&bSegment[7 + nTop], pPixels, nRegionW, nRegionH, 1);
		if (nTop + nBottom > TTXSUB_MAX_OBJECT_BYTES)
		{
			// Anti-aliased edges make many short runs: without them the object is much smaller
			int nPixel;
			for (nPixel = 0; nPixel < nRegionW * nRegionH; nPixel++)
			{
				int nGroup;
				for (nGroup = 0; nGroup < 4; nGroup++)
				{
					if (pPixels[nPixel] == nGroupBase[nGroup])
						pPixels[nPixel] = 1;
					else if (pPixels[nPixel] == nGroupBase[nGroup] + 1)
						pPixels[nPixel] = (BYTE)(nGroupBase[nGroup] + 2);
				}
			}
			nTop = EncodeField(&bSegment[7], pPixels, nRegionW, nRegionH, 0);
			nBottom = EncodeField(&bSegment[7 + nTop], pPixels, nRegionW, nRegionH, 1);
		}
		bSegment[3] = (BYTE)(nTop >> 8);
		bSegment[4] = (BYTE)nTop;
		bSegment[5] = (BYTE)(nBottom >> 8);
		bSegment[6] = (BYTE)nBottom;
		nSegmentLength = 7 + nTop + nBottom;
		if (nSegmentLength & 1)
			bSegment[nSegmentLength++] = 0x00;	// stuffing to an even length
		LocalFree(pPixels);
		if (nPES + 6 + nSegmentLength + 16 > sizeof(bPES))
			return;			// too big to send in one PES (cannot happen with 1-4 lines)
		nPES += PutSegment(&bPES[nPES], 0x13, bSegment, nSegmentLength);
	}

	nPES += PutSegment(&bPES[nPES], 0x80, NULL, 0);		// end of display set
	bPES[nPES++] = 0xff;								// end of PES data field marker
	bPES[4] = (BYTE)((nPES - 6) >> 8);
	bPES[5] = (BYTE)(nPES - 6);
	PacketizePES(pPage, bPES, nPES);
}

// ---------------------------------------------------------------- teletext decoding

// Latin G0 national option characters (EN 300 706 table 36) at 0x23 0x24 0x40 0x5b-0x60 0x7b-0x7e
static const char * szNationalPositions = "\x23\x24\x40\x5b\x5c\x5d\x5e\x5f\x60\x7b\x7c\x7d\x7e";
static const char * szNationalOptions[8] =
{
	"\xa3$@\xab\xbd\xbb^#-\xbc\xa6\xbe\xf7",		// English
	"#$\xa7\xc4\xd6\xdc^_\xb0\xe4\xf6\xfc\xdf",		// German
	"#\xa4\xc9\xc4\xd6\xc5\xdc_\xe9\xe4\xf6\xe5\xfc",	// Swedish / Finnish / Hungarian
	"\xa3$\xe9\xb0\xe7\xbb^#\xf9\xe0\xf2\xe8\xec",	// Italian
	"\xe9\xef\xe0\xeb\xea\xf9\xee#\xe8\xe2\xf4\xfb\xe7",	// French
	"\xe7$\xa1\xe1\xe9\xed\xf3\xfa\xbf\xfc\xf1\xe8\xe0",	// Portuguese / Spanish
	"#$@\xc1\xc9\xcd\xd3\xda_\xe1\xe9\xed\xf3",		// (Czech / Slovak: closest Latin-1)
	"#\xa4\xc9\xc4\xd6\xc5\xdc_\xe9\xe4\xf6\xe5\xfc"	// unknown: Nordic
};

static char TeletextChar(BYTE c, int nNationalOption)
{
	const char * p = strchr(szNationalPositions, c);

	if (c == 0x7f)
		return ' ';
	if (p != NULL)
		return szNationalOptions[nNationalOption & 7][p - szNationalPositions];
	return (char)c;
}

// Text lines of a received page: the boxed text of each row with its colour
static int PageLines(TTXSUBPAGE * pPage, SUBLINE * pLines, int nMax)
{
	int nRow, nLines = 0;

	for (nRow = 1; nRow <= 23 && nLines < nMax; nRow++)
	{
		BYTE * pRow = pPage->bRows[nRow];
		int nColour = 7, nTextColour = -1;
		BOOL fBoxed = FALSE, fAnyBox = FALSE;
		int nStart = -1, nEnd = -1, nCol, nOut = 0;
		char szText[64];

		if (!pPage->fRowUsed[nRow])
			continue;
		for (nCol = 0; nCol < 40; nCol++)
		{
			BYTE c = pRow[nCol];

			if (c < 0x20)
			{
				if (c <= 0x07)
					nColour = c;
				else if (c == 0x0b)
					fBoxed = fAnyBox = TRUE;
				else if (c == 0x0a)
					fBoxed = FALSE;
				if (nOut && szText[nOut - 1] != ' ' && nOut < sizeof(szText) - 1)
					szText[nOut++] = ' ';
				continue;
			}
			if (!fBoxed)
				continue;
			if (c != ' ' && nStart == -1)
			{
				nStart = nCol;
				nTextColour = nColour;
			}
			if (c != ' ')
				nEnd = nCol;
			if (nStart != -1 && nOut < sizeof(szText) - 1)
				szText[nOut++] = TeletextChar(c, pPage->nNationalOption);
		}
		if (!fAnyBox || nStart == -1)
			continue;
		while (nOut > 0 && szText[nOut - 1] == ' ')
			nOut--;
		szText[nOut] = '\0';
		lstrcpy(pLines[nLines].szText, szText);
		pLines[nLines].nRow = nRow;
		pLines[nLines].nColour = TeletextColourGroup(nTextColour);
		pLines[nLines].nCentreColumn = nStart + nEnd + 1;
		nLines++;
	}
	return nLines;
}

// A page has been received completely: send it if what is shown changes
static void PageComplete(TTXSUBPAGE * pPage)
{
	SUBLINE lines[24];
	char szAll[1024];
	int nLines = PageLines(pPage, lines, 24), i;

	szAll[0] = '\0';
	for (i = 0; i < nLines; i++)
	{
		if (lstrlen(szAll) + lstrlen(lines[i].szText) + 8 < sizeof(szAll))
		{
			char szRow[8];
			wsprintf(szRow, "%d:%d|", lines[i].nRow, lines[i].nColour);
			lstrcat(szAll, szRow);
			lstrcat(szAll, lines[i].szText);
			lstrcat(szAll, "\n");
		}
	}
	if (lstrcmp(szAll, pPage->szShown) == 0)
		return;
	lstrcpy(pPage->szShown, szAll);
	SendDisplaySet(pPage, lines, nLines);
}

static void TeletextPacket(TTXSUBSOURCE * pSource, BYTE * pData, DWORD64 lnPTS)
{
	int nAddress0 = cHamming84[pData[0]], nAddress1 = cHamming84[pData[1]];
	int nMagazine, nRow, i;

	if (nAddress0 < 0 || nAddress1 < 0)
		return;
	nMagazine = nAddress0 & 7;
	if (nMagazine == 0)
		nMagazine = 8;
	nRow = (nAddress0 >> 3) | (nAddress1 << 1);

	if (nRow == 0)
	{
		int nUnits = cHamming84[pData[2]], nTens = cHamming84[pData[3]];
		int nS2 = cHamming84[pData[5]], nS4 = cHamming84[pData[7]];
		int nC1 = cHamming84[pData[8]], nC2 = cHamming84[pData[9]];
		int nPage;
		BOOL fSerial;

		if (nUnits < 0 || nTens < 0 || nS2 < 0 || nS4 < 0 || nC1 < 0 || nC2 < 0)
			return;
		fSerial = nC2 & 1;							// C11
		// A header ends the page being received: in its magazine, or in every
		// magazine when pages are sent in serial mode
		for (i = 1; i <= 8; i++)
		{
			if ((fSerial || i == nMagazine) && pSource->pReceiving[i] != NULL)
			{
				PageComplete(pSource->pReceiving[i]);
				pSource->pReceiving[i] = NULL;
			}
		}
		nPage = (nMagazine << 8) | (nTens << 4) | nUnits;
		for (i = 0; i < nPages; i++)
		{
			TTXSUBPAGE * pPage = &pages[i];

			if (pPage->nSourcePID != pSource->nPID || pPage->nPage != nPage)
				continue;
			// C4 erases the page; a subtitle page (C6) is sent whole every time, and
			// broadcasters rarely set C4, so it starts empty on every header as well
			if ((nS2 & 0x08) || (nS4 & 0x08))
			{
				memset(pPage->fRowUsed, 0, sizeof(pPage->fRowUsed));
				memset(pPage->bRows, ' ', sizeof(pPage->bRows));
			}
			// C12-C14 select the national option set; C12 is the most significant bit
			pPage->nNationalOption = (((nC2 >> 1) & 1) << 2) | (((nC2 >> 2) & 1) << 1) | ((nC2 >> 3) & 1);
			pPage->lnPTS = lnPTS;
			pSource->pReceiving[nMagazine] = pPage;
		}
	}
	else if (nRow <= 23 && pSource->pReceiving[nMagazine] != NULL)
	{
		TTXSUBPAGE * pPage = pSource->pReceiving[nMagazine];

		for (i = 0; i < 40; i++)
			pPage->bRows[nRow][i] = pData[2 + i] & 0x7f;		// odd parity bit dropped
		pPage->fRowUsed[nRow] = TRUE;
	}
}

static void TeletextPES(TTXSUBSOURCE * pSource, BYTE * pPES, int nLength)
{
	DWORD64 lnPTS = 0;
	int nIndex;

	if (nLength < 10 || pPES[0] != 0 || pPES[1] != 0 || pPES[2] != 1)
		return;
	if (pPES[7] & 0x80)
		lnPTS = ((DWORD64)(pPES[9] & 0x0e) << 29) | ((DWORD64)pPES[10] << 22) | ((DWORD64)(pPES[11] & 0xfe) << 14)
			  | ((DWORD64)pPES[12] << 7) | (pPES[13] >> 1);
	nIndex = 9 + pPES[8];
	if (nIndex >= nLength || pPES[nIndex] < 0x10 || pPES[nIndex] > 0x1f)
		return;										// not EBU teletext
	nIndex++;
	while (nIndex + 2 <= nLength)
	{
		int nUnitID = pPES[nIndex], nUnitLength = pPES[nIndex + 1];

		if ((nUnitID == 0x02 || nUnitID == 0x03) && nUnitLength == 0x2c && nIndex + 2 + 0x2c <= nLength)
		{
			BYTE bData[42];
			int i;

			// skip field/line byte and framing code; bytes are sent LSB first
			for (i = 0; i < 42; i++)
				bData[i] = bReverse[pPES[nIndex + 4 + i]];
			TeletextPacket(pSource, bData, lnPTS);
		}
		nIndex += 2 + nUnitLength;
	}
}

// A TS packet of a converted teletext PID (from the stream processing thread)
void TtxSubs_InputPacket(const BYTE * pPacket)
{
	int nPID = ((pPacket[1] & 0x1f) << 8) | pPacket[2];
	int nAdaptation = (pPacket[3] >> 4) & 3;
	int nOffset = 4, i;
	TTXSUBSOURCE * pSource = NULL;

	for (i = 0; i < nSources; i++)
		if (sources[i].nPID == nPID)
			pSource = &sources[i];
	if (pSource == NULL || !(nAdaptation & 1))
		return;
	if (nAdaptation & 2)
		nOffset += 1 + pPacket[4];
	if (nOffset >= 188)
		return;
	if (pPacket[1] & 0x40)
	{
		if (pSource->nFill)
			TeletextPES(pSource, pSource->bPES, pSource->nFill);
		pSource->nFill = 0;
	}
	if (pSource->nFill + 188 - nOffset <= sizeof(pSource->bPES))
	{
		memcpy(&pSource->bPES[pSource->nFill], &pPacket[nOffset], 188 - nOffset);
		pSource->nFill += 188 - nOffset;
	}
	// Decode as soon as the PES is complete (its length is known for teletext)
	if (pSource->nFill >= 6)
	{
		int nPESLength = ((pSource->bPES[4] << 8) | pSource->bPES[5]) + 6;
		if (nPESLength > 6 && pSource->nFill >= nPESLength)
		{
			TeletextPES(pSource, pSource->bPES, nPESLength);
			pSource->nFill = 0;
		}
	}
}

// Moves queued DVB subtitle packets into the record buffer; returns the bytes written
int TtxSubs_Drain(BYTE * pDest, int nSpace)
{
	int nWritten = 0;

	while (nFifoCount > 0 && nSpace - nWritten >= 188)
	{
		memcpy(&pDest[nWritten], &pFifo[nFifoRead * 188], 188);
		nFifoRead = (nFifoRead + 1) % TTXSUB_FIFO_PACKETS;
		nFifoCount--;
		nWritten += 188;
	}
	return nWritten;
}
