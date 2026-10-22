void CopyListControlToClipboard(HWND hListControl, BOOL fAddCR);
void UpdateMainStatusText(char * szText);
void UpdateSecondaryStatusText(char * szText);
BOOL ATSCPIDs();
__int64 DecodeMPEG2PCR(BYTE * bAB);
int GetTotalPMTChannels();
BOOL IsDuplicateDescriptor(BYTE * pDescriptor1, BYTE * pDescriptor2);
char * GetExtensionPtr(char * szInputString);
BOOL FillAddr(PSOCKADDR_IN psin, TCHAR * szHostName, unsigned short usPort);

BOOL IsMPEGAudioStream(int nPMTProgramIndex, int nESIndex);
BOOL IsAC3AudioStream(int nPMTProgramIndex, int nESIndex);
BOOL IsPCMAudioStream(int nPMTProgramIndex, int nESIndex);
BOOL IsDTSAudioStream(int nPMTProgramIndex, int nESIndex);
int GetDTSAudioDescriptor(int nPMTProgramIndex, int nESIndex, BYTE * pBuffer);
int GetDTSFrameSize(int nPMTProgramIndex, int nESIndex);
BOOL IsDataBroadcastStream(int nPMTProgramIndex, int nESIndex);
BOOL IsTeleTextOrVBIStream(int nPMTProgramIndex, int nESIndex);
BOOL IsSubtitleStream(int nPMTProgramIndex, int nESIndex);

void GetATSCMultipleString(int nBitBufferIndex, char * szOutputString, int nLength);
void GetExtendedChannelName(BYTE * pSectionPointer, char * szLongName);

void CursorNormal();
void CursorWait(HWND hWnd);
void ConvertDVBBCDTimeOffsets(DWORD dwInput, int * nHour, int * nMinute);
DWORD ConvertBCD(DWORD nInput);
void ConvertDVBDate(int nMJD, int * nYear, int * nMonth, int * nDay);
void ConvertDVBTime(int nBCDTime, int * nHour, int * nMinute, int * nSecond);
void ConvertATSCDateTime(DWORD dwGPSTime, SYSTEMTIME * st);
BOOL EventInPast(PEITEVENT pEvent, BOOL fAllowPastEITData);
BOOL GetEPGNow(DWORD64 * plnNow);
void SetEPGStreamTime(int nYear, int nMonth, int nDay, int nHour, int nMinute, int nSecond);
void ResetEPGStreamTime();
void LogDescriptor(int nDescriptorIndex, int nDescriptorTag);
void ExpireOldEITData(int nServiceID);

#ifndef LITE
void SaveEPGData(PEITEVENT pEITItem, int nChannelNumber);
void SaveExistingEPGData();
#endif LITE

BOOL myGetSaveFileName(LPOPENFILENAME lpofn);
void GetLanguageFromDescriptor(char * szLanguage, int nPMTIndex, int nESIndex);
void StripTrailingSpaces(char * szString);
char * TrueFalseString(BOOL fTrue);
void EscapeReplaceXML(char * szBuffer);
void WriteHTMLLine(HANDLE hFile, char * szString);
void WriteHTMLASCII(HANDLE hFile, char * szBuffer);

int GetVideoStreamCount();
int GetProgramCount();

void LoadVideoDecoderCrashThumbnail(int nESParsePMTIndex, int nESParseESIndex);
void YUVtoRGB(BYTE * pImage, BYTE * pY, BYTE * pU, BYTE * pV, int x, int y);
void GetNewThumbnailSize(int * nSourceHeight, int * nDestHeight, int * nDestWidth);
void GenerateSizedThumbnail(BYTE * pImage, int nDestWidth, int nDestHeight, int x, int nSourceHeight, int nESParsePMTIndex, int nESParseESIndex);
void GenerateAudioThumbnail(signed short * pSamples, int nAudioChannels, int nDestWidth, int nDestHeight, BYTE * pThumbnail, int nESParsePMTIndex, int nESParseESIndex);
void SetupScrambledChannelThumbnail(int nESParsePMTIndex, int nESParseESIndex);
#define CHARTX 220
#define CHARTTIMES 6
#define SAMPLES_REQUIRED CHARTX * CHARTTIMES * 2

void InvalidateThumbnails();
void GetVideoArea(int * xStart, int * yStart, int * xWidth, int * yHeight);
int ReadFromMPEG2ESPipe(BYTE * pBuffer, int nLength, int nES);
void SaveArchiveThumbnail(char * szStatus, int nES);
void DecoderThread_SaveThumbnail(char * szStatus, int nES, int width, int height, BYTE * picbuf);

#ifdef PRO
BYTE ReverseBits(BYTE bInput);
#endif PRO

int DetermineSignalType(char * szSignal);
void ExtractSignalData(int nSignalChartMode, float * fNewValues0, float * fNewValues1);

int GetLogicalChannelNumber(int nProgramNumber);
void GetBouquetName(int nBouquetIndex, char * szOutput);
BOOL GetPIDTooltipInfo(int nPID, char * szString);
void GetSourceInfoLine(int nLine, char * szOutput);
char * FormatTooltipPID(int nPID);
