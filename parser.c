#include <windows.h>
#include <commctrl.h>
#include <time.h>
#include <winsock.h>
#include <shlobj.h>
#include "tsreader.h"
#include "bcdmux.h"
#include "util.h"
#include "dn_huffman.h"

extern PVARIABLES v;
extern BOOL (* PIDManagement) (BOOL fAdd, int nPID, BOOL fTemporary);

void MD__IPDataToFilters(int nPID, BYTE * pData, PMPEIPPACKET pmpeippacket);
void GetNextECMPID();
void UpdateSkyEPGMap(int nBATID);

// From StreamMonitor.c
double GetStreamMonitorTime();

void ParseATSCCETT(BYTE * pSectionPointer, int nPacketLength)
{
	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int private_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		
		if (section_length <= 0 || section_length > 4096)
			goto ParseATSCCETT_Windup;
		
		if (SourceHelper_CRC_Check(pSectionPointer, section_length + 3) != TRUE)
		{
			if (!v->fIgnoreTableCRCErrors)
				goto ParseATSCCETT_Windup;
		}
		{
			int table_id_extension = get_bits(BM_PARSER_THREAD, 16);
			int reserved2 = get_bits(BM_PARSER_THREAD, 2);
			int version_number = get_bits(BM_PARSER_THREAD, 5);
			int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
			int section_number = get_bits(BM_PARSER_THREAD, 8);
			int last_section_number = get_bits(BM_PARSER_THREAD, 8);
			int protocol_version = get_bits(BM_PARSER_THREAD, 8);
			int source_id = get_bits(BM_PARSER_THREAD, 16);
			int event_id = get_bits(BM_PARSER_THREAD, 14);
			int ETM_id_flag = get_bits(BM_PARSER_THREAD, 2);
			if (ETM_id_flag == 0)
			{
				int nServiceID;
				char szExtendedChannelString[4096] = {0};

				GetATSCMultipleString(BM_PARSER_THREAD, szExtendedChannelString, -1);
				for (nServiceID = 0; nServiceID < MAX_EIT_CHANNEL_DATA; nServiceID++)
				{
					if (v->pChannelData[nServiceID] != NULL)
					{
						if (v->pChannelData[nServiceID]->nSourceID == source_id)
						{
							if (lstrlen(v->pChannelData[nServiceID]->szATSCChannelETT) == 0)
							{
								if (lstrlen(szExtendedChannelString) >= sizeof(v->pChannelData[nServiceID]->szATSCChannelETT))
									szExtendedChannelString[sizeof(v->pChannelData[nServiceID]->szATSCChannelETT) - 1] = '\0';
								lstrcpy(v->pChannelData[nServiceID]->szATSCChannelETT, szExtendedChannelString);
							}
							break;
						}
					}
				}
			}
		}
	}
ParseATSCCETT_Windup:
	return;
}

void ParseATSCEITPacket(BYTE * pSectionPointer, int nPacketLength, int nEITNumber)
{
	int j;
	int nServiceID;
	BOOL fUsedThisEITEvent;
	EITEVENT thiseitevent;
	PEITEVENT pNewEIT;

	// Ignore all EITs until we have seen the STT and therefore know the GPS offset
	if (!v->dvbtdt.fSTTSeen)
		return;

#ifndef LITE
	if (v->fEPGSaveEnabled == FALSE)
	{
		if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(v->hEPGSaveHandle);
			v->hEPGSaveHandle = INVALID_HANDLE_VALUE;
		}
	}
	else
	{
		if (v->fEPGSaveFirstTime == TRUE)
		{
			SaveExistingEPGData();
			v->fEPGSaveFirstTime = FALSE;
		}
	}
#endif LITE

	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int private_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);

		if (section_length <= 0 || section_length > 4096)
			goto ParseATSCEITPacket_Windup;
		if (table_id != 0xcb)
			goto ParseATSCEITPacket_Windup;

		if (SourceHelper_CRC_Check(pSectionPointer, section_length + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_EIT]++;
			if (!v->fIgnoreTableCRCErrors)
				goto ParseATSCEITPacket_Windup;
		}
		v->nSIParserPackets[SI_PARSER_STATS_EIT]++;
		{
			int source_id = get_bits(BM_PARSER_THREAD, 16);
			int zero = get_bits(BM_PARSER_THREAD, 2);
			int version_number = get_bits(BM_PARSER_THREAD, 5);
			int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
			int section_number = get_bits(BM_PARSER_THREAD, 8);
			int last_section_number = get_bits(BM_PARSER_THREAD, 8);
			int protocol_version = get_bits(BM_PARSER_THREAD, 8);
			int num_events_in_section = get_bits(BM_PARSER_THREAD, 8);
			for (j = 0; j < num_events_in_section; j++)
			{
				int reserved2 = get_bits(BM_PARSER_THREAD, 2);
				int event_id = get_bits(BM_PARSER_THREAD, 14);
				DWORD start_time = get_bits(BM_PARSER_THREAD, 32);
				int reserved3 = get_bits(BM_PARSER_THREAD, 2);
				int ETM_location = get_bits(BM_PARSER_THREAD, 2);
				int length_in_seconds = get_bits(BM_PARSER_THREAD, 20);
				int title_length = get_bits(BM_PARSER_THREAD, 8);
				fUsedThisEITEvent = FALSE;
				memset(&thiseitevent, 0, sizeof(thiseitevent));
				GetATSCMultipleString(BM_PARSER_THREAD, thiseitevent.szEventName, title_length);
						
				// Got an EIT event - build the structure
				thiseitevent.nEventID = event_id;
				start_time -= v->dvbtdt.nGPSOffset;
				ConvertATSCDateTime(start_time, &thiseitevent.stStartTime);
				thiseitevent.stRunTime.wHour = length_in_seconds / 3600;
				length_in_seconds -= thiseitevent.stRunTime.wHour * 3600;
				thiseitevent.stRunTime.wMinute = length_in_seconds / 60;
				length_in_seconds -= thiseitevent.stRunTime.wMinute * 60;
				thiseitevent.stRunTime.wSecond = length_in_seconds;
				thiseitevent.nSource = nEITNumber;

				{
					int reserved4 = get_bits(BM_PARSER_THREAD, 4);
					int descriptors_length = get_bits(BM_PARSER_THREAD, 12);
					while (descriptors_length > 0)
					{
						int i;
						int descriptor_tag = get_bits(BM_PARSER_THREAD, 8);
						int descriptor_length = get_bits(BM_PARSER_THREAD, 8); 
						int k;
						BYTE bDescriptorBuffer[256];

						LogDescriptor(DESCRIPTOR_EIT, descriptor_tag);

						descriptors_length--; descriptors_length--;
						for (k = 0; k < descriptor_length; k++)
						{
							bDescriptorBuffer[k] = get_bits(BM_PARSER_THREAD, 8); descriptors_length--;
						}
						
						for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
						{
							if (thiseitevent.pExtraDescriptors[i] != NULL)
							{
								if (thiseitevent.pExtraDescriptors[i][0] == descriptor_tag)
									break;	// already got this descriptor
							}
							if (thiseitevent.pExtraDescriptors[i] == NULL)
							{
								int k;

								thiseitevent.pExtraDescriptors[i] = &v->tempDescriptorBuffer[i][0];
								thiseitevent.pExtraDescriptors[i][0] = descriptor_tag;
								thiseitevent.pExtraDescriptors[i][1] = descriptor_length;

								for (k = 0; k < descriptor_length; k++)
									thiseitevent.pExtraDescriptors[i][k + 2] = bDescriptorBuffer[k];
								break;
							}
						}
					}
				}

				// See about adding this item
				nServiceID = -1;
				for (nServiceID = 0; nServiceID < MAX_EIT_CHANNEL_DATA; nServiceID++)
				{
					if (v->pChannelData[nServiceID] != NULL)
					{
						if (v->pChannelData[nServiceID]->nSourceID == source_id)
							break;
					}
				}
				
				if ( (nServiceID < MAX_EIT_CHANNEL_DATA) && (nServiceID >= 0) )
				{
					EnterCriticalSection(&v->csEIT);

					// See if any items need to be expired
					ExpireOldEITData(nServiceID);
					
					// Now see about adding this item to the list
					if (v->pEvents[nServiceID] == NULL)
					{
						// No EIT data for this channel
						if (EventInPast(&thiseitevent, v->fKeepPastEITData) == FALSE)
						{
							v->pEvents[nServiceID] = LocalAlloc(LPTR, sizeof(EITEVENT) + 4);
							if (v->pEvents[nServiceID] != NULL)
							{
								int i;
								memcpy(v->pEvents[nServiceID], &thiseitevent, sizeof(EITEVENT));
								pNewEIT = v->pEvents[nServiceID];

								// Copy over the descriptors
								for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
								{
									if (thiseitevent.pExtraDescriptors[i] != NULL)
									{
										v->pEvents[nServiceID]->pExtraDescriptors[i] = LocalAlloc(LPTR, thiseitevent.pExtraDescriptors[i][1] + 2 + 4);
										memcpy(v->pEvents[nServiceID]->pExtraDescriptors[i],
												thiseitevent.pExtraDescriptors[i],
												thiseitevent.pExtraDescriptors[i][1] + 2);
										thiseitevent.pExtraDescriptors[i] = NULL;
									}
									else
										break;
								}
								fUsedThisEITEvent = TRUE;
								v->nEITEvents++;
#ifndef LITE
								if (v->fEPGSaveEnabled)
									SaveEPGData(v->pEvents[nServiceID], nServiceID);
#endif LITE
								PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_EIT, nServiceID);
							}
						}
					}
					else
					{
						PEITEVENT pCurrent;

						pCurrent = v->pEvents[nServiceID];
						do
						{
							if (pCurrent->nEventID == thiseitevent.nEventID)
							{
								// already got this event
								break;
							}
							if (pCurrent->dwNextEvent == 0)
							{
								// end of the list without a match on the service ID
								// so add this new item to the end of the list
								if (EventInPast(&thiseitevent, v->fKeepPastEITData) == FALSE)
								{
									PEITEVENT pNewEvent = LocalAlloc(LPTR, sizeof(EITEVENT) + 4);
									if (pNewEvent != NULL)
									{
										int i;

										memcpy(pNewEvent, &thiseitevent, sizeof(EITEVENT));
										pNewEIT = pNewEvent;
										fUsedThisEITEvent = TRUE;
										pCurrent->dwNextEvent = (DWORD)pNewEvent;

										// Copy over the descriptors
										for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
										{
											if (thiseitevent.pExtraDescriptors[i] != NULL)
											{
												pNewEvent->pExtraDescriptors[i] = LocalAlloc(LPTR, thiseitevent.pExtraDescriptors[i][1] + 2 + 4);
												memcpy(pNewEvent->pExtraDescriptors[i],
														thiseitevent.pExtraDescriptors[i],
														thiseitevent.pExtraDescriptors[i][1] + 2);
												thiseitevent.pExtraDescriptors[i] = NULL;
											}
											else
												break;
										}
										v->nEITEvents++;
#ifndef LITE
										if (v->fEPGSaveEnabled)
											SaveEPGData(pNewEvent, nServiceID);
#endif LITE
									}
								}
								break;
							}
							pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
						} while (TRUE);
					}
					LeaveCriticalSection(&v->csEIT);
				}
				
				if (fUsedThisEITEvent == FALSE)
				{
					int i;

					for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
					{
						if (thiseitevent.pExtraDescriptors[i] == NULL)
							break;
						thiseitevent.pExtraDescriptors[i] = NULL;
					}
				}
				else
				{
					if (v->hWndEPGGrid != NULL && v->fEPGDisplayActive && v->fEPGUpdateRealtime)
						PostMessage(v->hWndEPGGrid, WM_USER + 2, SI_PARSER_EIT + nServiceID, (LPARAM)pNewEIT);
				}
			}//for
		}
	}
ParseATSCEITPacket_Windup:
	return;
}

void ParseATSCETTPacket(BYTE * pSectionPointer, int nPacketLength)
{
#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader:ParseATSCETTPacket+\n");
#endif DEBUG_MESSAGES

#ifndef LITE
	if (v->fEPGSaveEnabled == FALSE)
	{
		if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(v->hEPGSaveHandle);
			v->hEPGSaveHandle = INVALID_HANDLE_VALUE;
		}
	}
	else
	{
		if (v->fEPGSaveFirstTime == TRUE)
		{
			SaveExistingEPGData();
			v->fEPGSaveFirstTime = FALSE;
		}
	}
#endif LITE

	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int private_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		
		if (section_length <= 0 || section_length > 4096)
			goto ParseATSCETTPacket_Windup;
		
		if (SourceHelper_CRC_Check(pSectionPointer, section_length + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_NIT]++;
			if (!v->fIgnoreTableCRCErrors)
			goto ParseATSCETTPacket_Windup;
		}
		v->nSIParserPackets[SI_PARSER_STATS_NIT]++;
		{
			int table_id_extension = get_bits(BM_PARSER_THREAD, 16);
			int reserved2 = get_bits(BM_PARSER_THREAD, 2);
			int version_number = get_bits(BM_PARSER_THREAD, 5);
			int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
			int section_number = get_bits(BM_PARSER_THREAD, 8);
			int last_section_number = get_bits(BM_PARSER_THREAD, 8);
			int protocol_version = get_bits(BM_PARSER_THREAD, 8);
			int source_id = get_bits(BM_PARSER_THREAD, 16);
			int event_id = get_bits(BM_PARSER_THREAD, 14);
			int ETM_id_flag = get_bits(BM_PARSER_THREAD, 2);
			if (ETM_id_flag == 2)
			{
				int nServiceID;
				char szExtendedEventString[4096] = {0};

				GetATSCMultipleString(BM_PARSER_THREAD, szExtendedEventString, -1);
				nServiceID = -1;
				for (nServiceID = 0; nServiceID < MAX_EIT_CHANNEL_DATA; nServiceID++)
				{
					if (v->pChannelData[nServiceID] != NULL)
					{
						if (v->pChannelData[nServiceID]->nSourceID == source_id)
							break;
					}
				}
				if ( (nServiceID < MAX_EIT_CHANNEL_DATA) && (nServiceID >= 0) )
				{
					PEITEVENT pCurrent;

					EnterCriticalSection(&v->csEIT);
					
					// Try to find a matching EIT entry
					pCurrent = v->pEvents[nServiceID];
					if (pCurrent != NULL)
					{
						do
						{
							if (pCurrent->nEventID == event_id)
							{
								// Found the event - add the description if we won't already have one
								if (pCurrent->szShortEventDescription == NULL)
								{
									if (lstrlen(szExtendedEventString) > 0)
									{
										pCurrent->szShortEventDescription = LocalAlloc(LPTR, lstrlen(szExtendedEventString) + 1 + 4);
										lstrcpy(pCurrent->szShortEventDescription, szExtendedEventString);
#ifndef LITE
										if (v->fEPGSaveEnabled)
											SaveEPGData(pCurrent, nServiceID);
#endif LITE
										if (v->hWndEPGGrid != NULL && v->fEPGDisplayActive && v->fEPGUpdateRealtime)
											PostMessage(v->hWndEPGGrid, WM_USER + 2, SI_PARSER_EIT + nServiceID, (LPARAM)pCurrent);
									}
								}
								break;
							}
							pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
						} while (pCurrent != NULL);
					}
					LeaveCriticalSection(&v->csEIT);
				}
			}
		}
	}
ParseATSCETTPacket_Windup:
#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader:ParseATSCETTPacket-\n");
#endif DEBUG_MESSAGES
	return;
}

// Rank of an EIT text language in v->szEITLanguages (lower is better); languages not
// listed, or any language when no preference is set, rank last and the first one wins
static int EITLanguageRank(const BYTE * pCode)
{
	char szCode[4];
	const char * pFound;
	int i;

	for (i = 0; i < 3; i++)
		szCode[i] = (char)tolower(pCode[i]);
	szCode[3] = '\0';
	pFound = strstr(v->szEITLanguages, szCode);
	if (szCode[0] == '\0' || pFound == NULL)
		return 100;
	return (int)(pFound - v->szEITLanguages);
}

// Remembers which languages the stream's EIT carries, for the language menus
static void NoteEITLanguage(const BYTE * pCode)
{
	int i;

	if (pCode[0] < 'A')
		return;
	for (i = 0; i < 16; i++)
	{
		if (v->szEITLanguagesSeen[i][0] == '\0')
		{
			v->szEITLanguagesSeen[i][0] = (char)tolower(pCode[0]);
			v->szEITLanguagesSeen[i][1] = (char)tolower(pCode[1]);
			v->szEITLanguagesSeen[i][2] = (char)tolower(pCode[2]);
			return;
		}
		if (strnicmp(v->szEITLanguagesSeen[i], (const char *)pCode, 3) == 0)
			return;
	}
}

void ParseDVBEITPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nSectionLength, nServiceID, nTableID;
	int nOriginalNetworkID;
	int nDescriptorsLoopLength;
	int nVersionNumber;
	EITEVENT thiseitevent;
	PEITEVENT pNewEIT;

#ifndef LITE
	if (v->fEPGSaveEnabled == FALSE)
	{
		if (v->hEPGSaveHandle != INVALID_HANDLE_VALUE)
		{
			CloseHandle(v->hEPGSaveHandle);
			v->hEPGSaveHandle = INVALID_HANDLE_VALUE;
		}
	}
	else
	{
		if (v->fEPGSaveFirstTime == TRUE)
		{
			SaveExistingEPGData();
			v->fEPGSaveFirstTime = FALSE;
		}
	}
#endif LITE

	do
	{
		BOOL fIgnoredViaNIT = FALSE;
		int i;

		// Check for stuffing
		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;
		
		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 65536) )
			break;

		// Get data from each section header		
		nTableID = pSectionPointer[0];
		if (v->nEITPID == 0x0012)
		{
			if ((nTableID < 0x4e || nTableID > 0x6f) && (nTableID != 0x72) )
			{
#ifdef PRO
				v->nSIParserTableErrors[SI_PARSER_STATS_EIT]++;
#endif PRO
				break;
			}
		}

		// Check for stuffing section
		if (nTableID == 0x72)
		{
			nPacketLength -= 3;		// table id and section length
			nPacketLength -= nSectionLength;
			pSectionPointer = &pSectionPointer[3] + nSectionLength;
			continue;
		}

#ifdef PRO
		if (nTableID == 0x4e)
		{
			double dCurrentTime = GetStreamMonitorTime();

			if (v->dSIParserTableTime[SI_PARSER_STATS_EIT] == 0.0)
				v->dSIParserTableTime[SI_PARSER_STATS_EIT] = dCurrentTime;
			if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_EIT] + 2.0)
			{
				v->nSIParserTimingErrors[SI_PARSER_STATS_EIT]++;			
			}
			v->dSIParserTableTime[SI_PARSER_STATS_EIT] = dCurrentTime;
		}
#endif PRO

		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_EIT]++;
			if (!v->fIgnoreTableCRCErrors)
				break;
		}

		nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
		if (v->nSIParserVersionNumbers[SI_PARSER_STATS_EIT] == -1)
			v->nSIParserVersionNumbers[SI_PARSER_STATS_EIT] = nVersionNumber;

		nServiceID = pSectionPointer[3] << 8 | pSectionPointer[4];
		nOriginalNetworkID = pSectionPointer[10] << 8 | pSectionPointer[11];
		nSectionLength -= 14; // detract size of the section header
		nPacketLength -= 4; // plus table id and section length
		nPacketLength -= nSectionLength;
		pSectionPointer = &pSectionPointer[14]; // point to the event loop

		// Check for an empty entry
		if (nSectionLength == 1)
		{
			nPacketLength -= 4;		// this byte + CRC
			pSectionPointer += 4;
			continue;
		}

		for (i = 0; i < MAX_IGNORED_NETWORKS; i++)
		{
			if (v->nIgnoredNetworks[i] == 0)
				break;
			if (v->nIgnoredNetworks[i] == nOriginalNetworkID)
			{
				fIgnoredViaNIT = TRUE;
				break;
			}
		}

		do
		{
			int nHour, nMinute, nSecond;
			int nHours, nMinutes, nSeconds;
			int nMJD, nStartTime, nDuration;
			int nYear, nMonth, nDay;
			char szShortEventDescription[4096] = {0};
			char szLongEventDescription[4096] = {0};
			int nShortRank = 1000, nLongRank = 1000;	// best EIT language so far (see EITLanguageRank)

			// Get data from each event
			memset(&thiseitevent, 0, sizeof(thiseitevent));
			thiseitevent.nEventID = pSectionPointer[0] << 8 | pSectionPointer[1];
			nMJD = pSectionPointer[2] << 8 | pSectionPointer[3];
			nStartTime = pSectionPointer[4] << 16 | pSectionPointer[5] << 8 | pSectionPointer[6];
			nDuration = pSectionPointer[7] << 16 | pSectionPointer[8] << 8 | pSectionPointer[9];

			ConvertDVBDate(nMJD, &nYear, &nMonth, &nDay);
			ConvertDVBTime(nStartTime, &nHour, &nMinute, &nSecond);
			thiseitevent.stStartTime.wYear = nYear;
			thiseitevent.stStartTime.wMonth = nMonth;
			thiseitevent.stStartTime.wDay = nDay;
			thiseitevent.stStartTime.wHour = nHour;
			thiseitevent.stStartTime.wMinute = nMinute;
			thiseitevent.stStartTime.wSecond = nSecond;
			ConvertDVBTime(nDuration, &nHours, &nMinutes, &nSeconds);
			thiseitevent.stRunTime.wHour = nHours;
			thiseitevent.stRunTime.wMinute = nMinutes;
			thiseitevent.stRunTime.wSecond = nSeconds;

			thiseitevent.fFreeCAMode = (pSectionPointer[10] >> 4) & 1;
			thiseitevent.nRunningStatus = (pSectionPointer[10] >> 5) & 7;
			thiseitevent.nSource = nTableID;
			thiseitevent.nFlags = (v->nEITLanguageGeneration & 0xff) << 8;	// which language choice the text was taken with

			nDescriptorsLoopLength = ((pSectionPointer[10] << 8 ) | pSectionPointer[11]) & 0xfff;
			nSectionLength -= 12;
			pSectionPointer = &pSectionPointer[12];
			if (nDescriptorsLoopLength > 0)
			{
				BOOL fUsedThisEITEvent = FALSE;
				do
				{
					// Get data from each descriptor
					int nDescriptorLength, nDescriptorTag;

					nDescriptorTag = pSectionPointer[0];
					nDescriptorLength = pSectionPointer[1] + 2;
					LogDescriptor(DESCRIPTOR_EIT, nDescriptorTag);

					switch(nDescriptorTag)
					{
					case 0x4d:			// short event descriptor
						{
							unsigned int nLength = pSectionPointer[5];
							unsigned char * pText = &pSectionPointer[6];
							unsigned int k;

							if (v->fEITLanguageFilterEnabled)
							{
								if (memcmp(v->szEITLanguageFilterLanguage, &pSectionPointer[2], 3) != 0)
									goto LogDefaultEITDescriptor;
							}

							// One short event per language: keep the most preferred one
							NoteEITLanguage(&pSectionPointer[2]);
							{
								int nRank = EITLanguageRank(&pSectionPointer[2]);
								if (nRank >= nShortRank)
									break;
								nShortRank = nRank;
							}
							thiseitevent.szEventName[0] = '\0';
							szShortEventDescription[0] = '\0';

							thiseitevent.nFlags |= EIT_FLAG_SHORT_EVENT;
							if (nLength > 0)
							{
								while ( ((*pText & 0x7f) < 0x20) && (nLength > 0) )
								{
									nLength--;
									pText++;
								}
								for (k = 0; k < nLength; k++)
								{
									thiseitevent.szEventName[k] = *pText++;
									if ((BYTE)thiseitevent.szEventName[k] == 0x80 || (BYTE)thiseitevent.szEventName[k] == 0x87)
										thiseitevent.szEventName[k] = ' ';
								}
								thiseitevent.szEventName[k] = '\0';
								nLength = *pText++;		// text_length
								if (nLength > 0)
								{
									int o = 0;
									for (k = 0; k < nLength; k++)
									{
										if (*pText < ' ' || *pText == 0x80 || *pText == 0x87)
										{
											pText++;
										}
										else
											szShortEventDescription[o++] = *pText++;
									}
									szShortEventDescription[o] = 0;
								}
							}
						}
						break;
					case 0x4e:			// extended event descriptor
						{
							int nTextLength;
							int nLengthOfItems = pSectionPointer[6];
							unsigned char * pText = &pSectionPointer[7];
							int nRank = EITLanguageRank(&pSectionPointer[3]);

							// Long texts are split over several extended event descriptors (per
							// language): join the parts of the most preferred language
							if (nRank > nLongRank)
								break;
							if (nRank < nLongRank)
							{
								nLongRank = nRank;
								szLongEventDescription[0] = '\0';
							}
							thiseitevent.nFlags |= EIT_FLAG_LONG_EVENT;
							pText += nLengthOfItems;		// skip to text
							nTextLength = (*pText++) & 0xff;	// get length of text
							if (nTextLength > 0)
							{
								int k;
								int o = lstrlen(szLongEventDescription);

								for (k = 0; k < nTextLength && o < 3500; k++)	// joined text stays within the 4 KB description buffers used by the exports
								{
									if (*pText < ' ' || *pText == 0x80 || *pText == 0x87)
									{
										pText++;
									}
									else
										szLongEventDescription[o++] = *pText++;
								}
								szLongEventDescription[o] = '\0';
							}
						}
						break;
					case 0x91:			// Dish Network compressed short event descriptor
						{
							int nIndex;
							HUFFMANTAB * pTable = NULL;

							thiseitevent.nFlags |= EIT_FLAG_COMPRESSED_SHORT_EVENT;
							if (v->nEITPID == 0x0300)
								pTable = &ExtHufTab;
							
							memset(thiseitevent.szEventName, 0, sizeof(thiseitevent.szEventName));
							DcpString(thiseitevent.szEventName,
								      &pSectionPointer[2],
									  sizeof(thiseitevent.szEventName),
									  pTable);
							for (nIndex = 0; nIndex < lstrlen(thiseitevent.szEventName); nIndex++)
							{
								if (thiseitevent.szEventName[nIndex] < ' ')
									thiseitevent.szEventName[nIndex] = ' ';
							}
						}				
						break;
					case 0x92:			// Dish Network compressed long event descriptor
						{	
							int nIndex;
							HUFFMANTAB * pTable = NULL;

							thiseitevent.nFlags |= EIT_FLAG_COMPRESSED_LONG_EVENT;
							if (v->nEITPID == 0x0300)
								pTable = &ExtHufTab;

							memset(szLongEventDescription, 0, sizeof(szLongEventDescription));
							DcpString(szLongEventDescription,
								      &pSectionPointer[2],
									  sizeof(szLongEventDescription),
									  pTable);
							for (nIndex = 0; nIndex < lstrlen(szLongEventDescription); nIndex++)
							{
								if (szLongEventDescription[nIndex] < ' ')
									szLongEventDescription[nIndex] = ' ';
							}
						}				
						break;
					case 0:
						break;
					default:
LogDefaultEITDescriptor:
						{
							int i;

							for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
							{
								if (thiseitevent.pExtraDescriptors[i] != NULL)
								{
									if (thiseitevent.pExtraDescriptors[i][0] == nDescriptorTag)
										break;	// already got this descriptor
								}
								if (thiseitevent.pExtraDescriptors[i] == NULL)
								{
									thiseitevent.pExtraDescriptors[i] = &v->tempDescriptorBuffer[i][0];
									memcpy(thiseitevent.pExtraDescriptors[i], &pSectionPointer[0], nDescriptorLength);
									break;
								}
							}
						}
						break;
					}

					nSectionLength -= nDescriptorLength;
					pSectionPointer += nDescriptorLength;
					nDescriptorsLoopLength -= nDescriptorLength;
				} while (nDescriptorsLoopLength > 0);

				if ( (nServiceID < MAX_EIT_CHANNEL_DATA) && (nServiceID >= 0) && !fIgnoredViaNIT)
				{
					EnterCriticalSection(&v->csEIT);

					// See if any items need to be expired
					ExpireOldEITData(nServiceID);
					
					// Now see about adding this item to the list
					if (v->pEvents[nServiceID] == NULL)
					{
						// No EIT data for this channel
						if (EventInPast(&thiseitevent, v->fKeepPastEITData) == FALSE)
						{
							v->pEvents[nServiceID] = LocalAlloc(LPTR, sizeof(EITEVENT) + 4);
							if (v->pEvents[nServiceID] != NULL)
							{
								int i;
								PEITEVENT pNewItem = v->pEvents[nServiceID];

								memcpy(v->pEvents[nServiceID], &thiseitevent, sizeof(EITEVENT));
								pNewEIT = v->pEvents[nServiceID];
								if (lstrlen(szShortEventDescription))
								{
									pNewItem->szShortEventDescription = LocalAlloc(LPTR, lstrlen(szShortEventDescription) + 1 + 4);
									lstrcpy(pNewItem->szShortEventDescription, szShortEventDescription);
								}
								if (lstrlen(szLongEventDescription))
								{
									pNewItem->szLongEventDescription = LocalAlloc(LPTR, lstrlen(szLongEventDescription) + 1 + 4);
									lstrcpy(pNewItem->szLongEventDescription, szLongEventDescription);
								}

								// Copy over the descriptors
								for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
								{
									if (thiseitevent.pExtraDescriptors[i] != NULL)
									{
										pNewItem->pExtraDescriptors[i] = LocalAlloc(LPTR, thiseitevent.pExtraDescriptors[i][1] + 2 + 4);
										memcpy(pNewItem->pExtraDescriptors[i],
												thiseitevent.pExtraDescriptors[i],
												thiseitevent.pExtraDescriptors[i][1] + 2);
									}
									else
										break;
								}
								fUsedThisEITEvent = TRUE;
								v->nEITEvents++;
								PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_EIT, nServiceID);
#ifndef LITE
								if (v->fEPGSaveEnabled)
									SaveEPGData(pNewItem, nServiceID);
#endif LITE
							}
						}
					}
					else
					{
						PEITEVENT pCurrent;

						pCurrent = v->pEvents[nServiceID];
						do
						{
							if (pCurrent->nEventID == thiseitevent.nEventID)
							{
								// The EIT language choice changed since this event was stored: take
								// its text again (in the newly preferred language)
								if (((pCurrent->nFlags >> 8) & 0xff) != ((thiseitevent.nFlags >> 8) & 0xff) &&
									(thiseitevent.nFlags & EIT_FLAG_SHORT_EVENT))
								{
									lstrcpy(pCurrent->szEventName, thiseitevent.szEventName);
									if (pCurrent->szShortEventDescription != NULL)
										LocalFree(pCurrent->szShortEventDescription);
									pCurrent->szShortEventDescription = NULL;
									if (lstrlen(szShortEventDescription))
									{
										pCurrent->szShortEventDescription = LocalAlloc(LPTR, lstrlen(szShortEventDescription) + 1 + 4);
										lstrcpy(pCurrent->szShortEventDescription, szShortEventDescription);
									}
									if (lstrlen(szLongEventDescription) && pCurrent->szLongEventDescription != NULL)
									{
										LocalFree(pCurrent->szLongEventDescription);
										pCurrent->szLongEventDescription = NULL;
									}
									pCurrent->nFlags = (pCurrent->nFlags & 0xff) | (thiseitevent.nFlags & 0xff00);
								}
								// already got this event, but check to see if there's no
								// extended description and we have an extended description
								if (pCurrent->szLongEventDescription == NULL)
								{
									if (lstrlen(szLongEventDescription) > 0)
									{
										pCurrent->szLongEventDescription = LocalAlloc(LPTR, lstrlen(szLongEventDescription) + 1 + 4);
										lstrcpy(pCurrent->szLongEventDescription, szLongEventDescription);
									}
								}
								break;
							}
							if (pCurrent->dwNextEvent == 0)
							{
								// end of the list without a match on the service ID
								// so add this new item to the end of the list
								if (EventInPast(&thiseitevent, v->fKeepPastEITData) == FALSE)
								{
									PEITEVENT pNewEvent = LocalAlloc(LPTR, sizeof(EITEVENT) + 4);
									if (pNewEvent != NULL)
									{
										int i;

										memcpy(pNewEvent, &thiseitevent, sizeof(EITEVENT));
										pNewEIT = pNewEvent;
										if (lstrlen(szShortEventDescription))
										{
											pNewEvent->szShortEventDescription = LocalAlloc(LPTR, lstrlen(szShortEventDescription) + 1 + 4);
											lstrcpy(pNewEvent->szShortEventDescription, szShortEventDescription);
										}
										if (lstrlen(szLongEventDescription))
										{
											pNewEvent->szLongEventDescription = LocalAlloc(LPTR, lstrlen(szLongEventDescription) + 1 + 4);
											lstrcpy(pNewEvent->szLongEventDescription, szLongEventDescription);
										}

										// Copy over the descriptors
										for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
										{
											if (thiseitevent.pExtraDescriptors[i] != NULL)
											{
												pNewEvent->pExtraDescriptors[i] = LocalAlloc(LPTR, thiseitevent.pExtraDescriptors[i][1] + 2 + 4);
												memcpy(pNewEvent->pExtraDescriptors[i],
														thiseitevent.pExtraDescriptors[i],
														thiseitevent.pExtraDescriptors[i][1] + 2);
												//thiseitevent.pExtraDescriptors[i] = NULL;
											}
											else
												break;
										}
										fUsedThisEITEvent = TRUE;
										pCurrent->dwNextEvent = (DWORD)pNewEvent;
										v->nEITEvents++;
#ifndef LITE
										if (v->fEPGSaveEnabled)
											SaveEPGData(pNewEvent, nServiceID);
#endif LITE
									}
								}
								break;
							}
							pCurrent = (PEITEVENT)pCurrent->dwNextEvent;
						} while (TRUE);
					}
					LeaveCriticalSection(&v->csEIT);
				}
				
				if (fUsedThisEITEvent == FALSE)
				{
					int i;

					for (i = 0; i < MAX_EIT_EXTRA_DESCRIPTORS; i++)
					{
						if (thiseitevent.pExtraDescriptors[i] == NULL)
							break;
						thiseitevent.pExtraDescriptors[i] = NULL;
					}
				}
				else
				{
					if (v->hWndEPGGrid != NULL && v->fEPGDisplayActive && v->fEPGUpdateRealtime)
						PostMessage(v->hWndEPGGrid, WM_USER + 2, SI_PARSER_EIT + nServiceID, (LPARAM)pNewEIT);
				}
			}
		} while (nSectionLength > 4);
		pSectionPointer += 4; // skip CRC
	} while (nPacketLength > 20);
}

int ParseDVBBAT(BYTE * pSectionPointer, int nSectionLength)
{
	int nCurrentLength = nSectionLength;
	int nBATIndex;
	BOOL fAlreadyGotBAT;

	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int i;
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved_future_use1 = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		int bouquet_id = get_bits(BM_PARSER_THREAD, 16);
		int reserved2 = get_bits(BM_PARSER_THREAD, 2);
		int version_number = get_bits(BM_PARSER_THREAD, 5);
		int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
		int section_number = get_bits(BM_PARSER_THREAD, 8);
		int last_section_number = get_bits(BM_PARSER_THREAD, 8);
		int reserved_future_use2 = get_bits(BM_PARSER_THREAD, 4);
		int bouquet_descriptors_length = get_bits(BM_PARSER_THREAD, 12);

		/*if (v->nSIParserVersionNumbers[SI_PARSER_STATS_BAT] == -1)
			v->nSIParserVersionNumbers[SI_PARSER_STATS_BAT] = version_number;
		else
		{
			if (v->nSIParserVersionNumbers[SI_PARSER_STATS_BAT] != version_number)
			{
				int i;

				return nSectionLength + 4;

				// Version number changed on us
				PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_BAT, -1);	// remove
				for (i = 0; i < MAX_BAT_ENTRIES; i++)
				{
					int j;

					if (v->bat[i].bouquet_descriptors != NULL)
						LocalFree(v->bat[i].bouquet_descriptors);
					for (j = 0; j < MAX_BAT_TRANSPORT_ITEMS; j++)
					{
						if (v->bat[i].batts[j].transport_descriptors != NULL)
							LocalFree(v->bat[i].batts[j].transport_descriptors);
					}
					memset(&v->bat[i], 0, sizeof(BAT));
				}
				v->nSIParserVersionNumbers[SI_PARSER_STATS_BAT] = version_number;
			}
		}*/
		
		// See if we have this BAT already
		fAlreadyGotBAT = FALSE;
		for (nBATIndex = 0; nBATIndex < MAX_BAT_ENTRIES; nBATIndex++)
		{
			if (v->bat[nBATIndex].bouquet_id == 0)
				break;		// empty spot
			if (v->bat[nBATIndex].bouquet_id == bouquet_id && v->bat[nBATIndex].version_number == version_number)
			{
				fAlreadyGotBAT = TRUE;
				break;
			}
		}
		if (nBATIndex == MAX_BAT_ENTRIES)
		{
			OutputDebugString("TSReader: Out of BAT space\n");
			return nSectionLength + 4;
		}
		if (!fAlreadyGotBAT)
		{
			v->bat[nBATIndex].bouquet_id = bouquet_id;
			v->bat[nBATIndex].version_number = version_number;
			v->bat[nBATIndex].bouquet_descriptors_length = bouquet_descriptors_length;
			v->bat[nBATIndex].bouquet_descriptors = LocalAlloc(LPTR, bouquet_descriptors_length + 4);
			for (i = 0; i < bouquet_descriptors_length; i++)
				v->bat[nBATIndex].bouquet_descriptors[i] = get_bits(BM_PARSER_THREAD, 8);
			i = 0;
			while (i < bouquet_descriptors_length)
			{
				LogDescriptor(DESCRIPTOR_BAT, v->bat[nBATIndex].bouquet_descriptors[i + 0]);
				i += v->bat[nBATIndex].bouquet_descriptors[i + 1] + 2;
			}
		}
		else
		{
			for (i = 0; i < bouquet_descriptors_length; i++)
				get_bits(BM_PARSER_THREAD, 8);
		}

		// Now the transport stream loop
		{
			int reserved_future_use3 = get_bits(BM_PARSER_THREAD, 4);
			int transport_stream_loop_length = get_bits(BM_PARSER_THREAD, 12);
			int nTransportIndex;
			while (transport_stream_loop_length)
			{
				int j;
				int transport_stream_id = get_bits(BM_PARSER_THREAD, 16);
				int original_network_id = get_bits(BM_PARSER_THREAD, 16);
				int reserved_future_use4 = get_bits(BM_PARSER_THREAD, 4);
				int transport_descriptors_length = get_bits(BM_PARSER_THREAD, 12);
				BOOL fAlreadyGotTransportBAT = FALSE;
				transport_stream_loop_length -= 6;

				for (nTransportIndex = 0; nTransportIndex < MAX_BAT_TRANSPORT_ITEMS; nTransportIndex++)
				{
					if (v->bat[nBATIndex].batts[nTransportIndex].transport_stream_id == 0
						&& v->bat[nBATIndex].batts[nTransportIndex].original_network_id == 0)
						break;		// end of list - need to add

					if (v->bat[nBATIndex].batts[nTransportIndex].transport_stream_id == transport_stream_id
						&& v->bat[nBATIndex].batts[nTransportIndex].original_network_id == original_network_id)
					{
						fAlreadyGotTransportBAT = TRUE;
						break;
					}
				}
				if (nTransportIndex == MAX_BAT_TRANSPORT_ITEMS)
				{
					OutputDebugString("TSReader: Out of BAT TS space\n");
					break;
				}

				if (!fAlreadyGotTransportBAT)
				{
					v->bat[nBATIndex].batts[nTransportIndex].transport_stream_id = transport_stream_id;
					v->bat[nBATIndex].batts[nTransportIndex].original_network_id = original_network_id;
					v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors_length = transport_descriptors_length;
					v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors = LocalAlloc(LPTR, v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors_length + 4);
					for (j = 0; j < transport_descriptors_length; j++)
						v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors[j] = get_bits(BM_PARSER_THREAD, 8);
					j = 0;
					while (j < transport_descriptors_length)
					{
						LogDescriptor(DESCRIPTOR_BAT, v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors[j + 0]);
						j += v->bat[nBATIndex].batts[nTransportIndex].transport_descriptors[j + 1] + 2;
					}
#ifndef LITE
					if (v->fSkyEPG && bouquet_id == v->nCurrentBATID)
						UpdateSkyEPGMap(v->nCurrentBATID);
#endif LITE
				}
				else
				{
					for (j = 0; j < transport_descriptors_length; j++)
						get_bits(BM_PARSER_THREAD, 8);
				}
				transport_stream_loop_length -= transport_descriptors_length;
			}
		}
		if (!fAlreadyGotBAT)
			PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_BAT, nBATIndex);
	}
	return nSectionLength + 4;

}

void ParseDVBSDTPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nSectionLength, nTableID;
	int nDescriptorsLoopLength;
	int nTransportStreamID, nOriginalNetworkID;
	int nVersionNumber;
	BOOL fAlreadyGotThisChannel;
	EITCHANNELDATA thischanneldata;

	do
	{
		int i;
		BOOL fIgnoredViaNIT = FALSE;

		// Check for stuffing
		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;

		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 65536) || (nSectionLength > nPacketLength) )
		{
#ifdef DEBUG_MESSAGES
			char szTemp[128];
			wsprintf(szTemp, "ParseSDT: nSection out of range = %d nPacketLength = %d\n", nSectionLength, nPacketLength);
			OutputDebugString(szTemp);
#endif DEBUG_MESSAGES
			return;
		}

		// Get data from each section header		
		nTableID = pSectionPointer[0];
		if (!(nTableID == 0x42 || nTableID == 0x46 || nTableID == 0x4a || nTableID == 0x72))
		{
#ifdef PRO
			v->nSIParserTableErrors[SI_PARSER_STATS_SDT]++;
#endif PRO
			return;
		}

		// Check for stuffing section
		if (nTableID == 0x72)
		{
			nPacketLength -= 3;		// table id and section length
			nPacketLength -= nSectionLength;
			pSectionPointer = &pSectionPointer[3] + nSectionLength;
			continue;
		}

		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
#ifdef DEBUG_MESSAGES
			OutputDebugString("ParseSDT: CRC failure\n");
#endif DEBUG_MESSAGES
			v->nSIParserCRCs[SI_PARSER_STATS_SDT]++;
			if (!v->fIgnoreTableCRCErrors)
				return;
		}

		if (nTableID == 0x4a)
		{
			int nProcessedLength = ParseDVBBAT(pSectionPointer, nSectionLength);
			pSectionPointer += nProcessedLength;
			nPacketLength -= nProcessedLength;
			continue;
		}

#ifdef PRO
		if (nTableID == 0x42)
		{
			double dCurrentTime = GetStreamMonitorTime();

			if (v->dSIParserTableTime[SI_PARSER_STATS_SDT] == 0.0)
				v->dSIParserTableTime[SI_PARSER_STATS_SDT] = dCurrentTime;
			if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_SDT] + 2.0)
			{
				v->nSIParserTimingErrors[SI_PARSER_STATS_SDT]++;			
			}
			v->dSIParserTableTime[SI_PARSER_STATS_SDT] = dCurrentTime;
		}
#endif PRO

		if (v->fSDTOnlyForCurrentMux == TRUE && nTableID != 0x42)
			return;

		nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
		if (v->nSIParserVersionNumbers[SI_PARSER_STATS_SDT] == -1)
			v->nSIParserVersionNumbers[SI_PARSER_STATS_SDT] = nVersionNumber;

		nTransportStreamID = pSectionPointer[3] << 8 | pSectionPointer[4];
		nOriginalNetworkID = pSectionPointer[8] << 8 | pSectionPointer[9];
		nSectionLength -= 11; // detract size of the section header
		nPacketLength -= 4; // plus table id and section length
		nPacketLength -= nSectionLength;
		pSectionPointer = &pSectionPointer[11]; // point to the service loop

		for (i = 0; i < MAX_IGNORED_NETWORKS; i++)
		{
			if (v->nIgnoredNetworks[i] == 0)
				break;
			if (v->nIgnoredNetworks[i] == nOriginalNetworkID)
			{
				fIgnoredViaNIT = TRUE;
				break;
			}
		}

		if (nSectionLength >= 6)
		{
			do
			{
				memset(&thischanneldata, 0, sizeof(thischanneldata));

				fAlreadyGotThisChannel = FALSE;
				thischanneldata.nChannelNumber = pSectionPointer[0] << 8 | pSectionPointer[1];
				thischanneldata.fEIT_schedule_flag = (pSectionPointer[2] >> 1) & 1;
				thischanneldata.fEIT_present_following_flag = pSectionPointer[2] & 1;
				thischanneldata.nRunningStatus = (pSectionPointer[3] >> 5) & 7;
				thischanneldata.fFreeCAMode = (pSectionPointer[3] >> 4) & 1;
				if (v->pChannelData[thischanneldata.nChannelNumber] != NULL)
					fAlreadyGotThisChannel = TRUE;
				thischanneldata.nFromTable = nTableID;
				if (thischanneldata.nChannelNumber > MAX_EIT_CHANNEL_DATA)
					return;
				thischanneldata.nTransportStreamID = nTransportStreamID;
				thischanneldata.nOriginalNetworkID = nOriginalNetworkID;
				nDescriptorsLoopLength = ((pSectionPointer[3] << 8 ) | pSectionPointer[4]) & 0xfff;
				nSectionLength -= 5;
				pSectionPointer = &pSectionPointer[5];
				if (nDescriptorsLoopLength > 0)
				{
					do
					{
						// Get data from each descriptor
						int nDescriptorLength, nDescriptorTag;

						nDescriptorTag = pSectionPointer[0];
						nDescriptorLength = pSectionPointer[1] + 2;
						LogDescriptor(DESCRIPTOR_SDT, nDescriptorTag);

						if (fAlreadyGotThisChannel == FALSE)
						{
							switch(nDescriptorTag)
							{
							case 0:
								break;
	#ifdef SKYSTUFF
							case 0xc0:
								memcpy(thischanneldata.szShortName, &pSectionPointer[2], nDescriptorLength - 2);
								thischanneldata.szShortName[nDescriptorLength - 2] = '\0';
								break;
	#endif SKYSTUFF
							case 0x48:
								{
									int nLength = pSectionPointer[3];
									unsigned char * pText;
									int k;
									int nOutput = 0;

									pText = &pSectionPointer[4];
									for (k = 0; k < nLength; k++)
									{
										if (*pText == 0x86 || *pText == 0x87 || *pText < 0x06)
											pText++;
										else
											thischanneldata.szLongName[nOutput++] = *pText++;
									}
									thischanneldata.szLongName[nOutput] = '\0';
									
									nLength = *pText++;								
									nOutput = 0;
									for (k = 0; k < nLength; k++)
									{
										if (*pText == 0x86 || *pText == 0x87 || *pText < 0x06)
											pText++;
										else
											thischanneldata.szShortName[nOutput++] = *pText++;
									}
									thischanneldata.szShortName[nOutput] = '\0';
								}
								//break;		// DON'T BREAK - we want to save this descriptor too
							default:
								{
									int i;

									for (i = 0; i < MAX_SDT_EXTRA_DESCRIPTORS; i++)
									{
										if (thischanneldata.pExtraDescriptors[i] != NULL)
										{
											if (IsDuplicateDescriptor(thischanneldata.pExtraDescriptors[i], pSectionPointer) == TRUE)
												break;
											//if (thischanneldata.pExtraDescriptors[i][0] == nDescriptorTag)
											//	break;	// already got this descriptor
										}
										if (thischanneldata.pExtraDescriptors[i] == NULL)
										{
											thischanneldata.pExtraDescriptors[i] = LocalAlloc(LPTR, nDescriptorLength + 4);
											if (thischanneldata.pExtraDescriptors[i] != NULL)
												memcpy(thischanneldata.pExtraDescriptors[i], &pSectionPointer[0], nDescriptorLength);
											break;
										}
									}
								}
								break;
							}
						}

						nSectionLength -= nDescriptorLength;
						pSectionPointer += nDescriptorLength;
						nDescriptorsLoopLength -= nDescriptorLength;
					} while (nDescriptorsLoopLength > 0);
				}

				// We now have a completed EITCHANNELDATA - check if this channel
				// needs to be added
				if (v->pChannelData[thischanneldata.nChannelNumber] == NULL && !fIgnoredViaNIT)
				{
					v->pChannelData[thischanneldata.nChannelNumber] = LocalAlloc(LPTR, sizeof(EITCHANNELDATA) + 4);
					if (v->pChannelData[thischanneldata.nChannelNumber] != NULL)
					{
						memcpy(v->pChannelData[thischanneldata.nChannelNumber], &thischanneldata, sizeof(EITCHANNELDATA));
						v->nEITChannels++;
						if (v->nNetworkPID == -1)
							v->nNetworkPID = 0x0010;	// this is a DVB feed
						PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_SDT, thischanneldata.nChannelNumber);
					}
				}
				else
				{
					for (i = 0; i < MAX_SDT_EXTRA_DESCRIPTORS; i++)
					{
						if (thischanneldata.pExtraDescriptors[i] != NULL)
						{
							LocalFree(thischanneldata.pExtraDescriptors[i]);
							thischanneldata.pExtraDescriptors[i] = NULL;
						}
					}
				}
			} while (nSectionLength > 4);
		}
		pSectionPointer += 4; // skip CRC
	} while (nPacketLength > 20);
}

BOOL ParseDCIIPMTTypeThing(BYTE * pSectionPointer, int nPacketLength, int nCurrentProgramNumber)
{
	int i;
	int nPMTIndex;
	int nProgramInfoLength = 0;
	BOOL fRetVal = FALSE;
	BYTE pProgramInfo[256];

	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int private_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		int frames_extension_flag = get_bits(BM_PARSER_THREAD, 1);
		int segmentation_overlay_included = get_bits(BM_PARSER_THREAD, 1);
		int message_preamble_included = get_bits(BM_PARSER_THREAD, 1);
		int message_type_version = get_bits(BM_PARSER_THREAD, 5);
		int unknown = get_bits(BM_PARSER_THREAD, 8);
		int service_number = get_bits(BM_PARSER_THREAD, 16);
		if (service_number != nCurrentProgramNumber)
			return fRetVal;
		
		for (i = 0; i < 7; i++)
		{
			int unknown = get_bits(BM_PARSER_THREAD, 8);
		}
		for (i = 0; i < 6; i++)
		{
			int CABytes = get_bits(BM_PARSER_THREAD, 8);
		}
		for (i = 0; i < 8; i++)
		{
			int unknown = get_bits(BM_PARSER_THREAD, 8);
		}
		{
			int unknown1 = get_bits(BM_PARSER_THREAD, 3);
			int PCR_PID = get_bits(BM_PARSER_THREAD, 13);
			int unknown2 = get_bits(BM_PARSER_THREAD, 8);
			int descriptor_length = get_bits(BM_PARSER_THREAD, 8);
			nProgramInfoLength = descriptor_length;
			for (i = 0; i < descriptor_length; i++)
			{
				pProgramInfo[i] = get_bits(BM_PARSER_THREAD, 8);
			}
			do
			{
				int stream_type = get_bits(BM_PARSER_THREAD, 8);
				if (   stream_type != 0x02 && stream_type != 0x80
					&& stream_type != 0x81 && stream_type != 0x86)
					break;
				{
					int unknown1 = get_bits(BM_PARSER_THREAD, 3);
					int stream_PID = get_bits(BM_PARSER_THREAD, 13);
					int unknown2 = get_bits(BM_PARSER_THREAD, 8);
					int descriptors_loop_length = get_bits(BM_PARSER_THREAD, 8);

					// find out offset in the list of channels in the pat structure
					nPMTIndex = -1;
					for (i = 0; i < MAX_PAT_ENTRIES; i++)
					{
						if (nCurrentProgramNumber == v->pat.pmt[i].nProgramNumber)
						{
							nPMTIndex = i;
							v->pat.pmt[nPMTIndex].nPCRPID = PCR_PID;
							break;
						}
					}
					if (nPMTIndex != -1) // should never happen!
					{
						if (v->pat.pmt[nPMTIndex].fCompleted)
							return FALSE;	// already done

						for (i = 0; i < MAX_ESLIST_ENTRIES; i++)
						{
							if (v->pat.pmt[nPMTIndex].es[i].nESPID == stream_PID) // already got this one?
								break;
							if (v->pat.pmt[nPMTIndex].es[i].nESPID == 0) // end of table without finding this?
							{
								v->pat.pmt[nPMTIndex].es[i].nESPID = stream_PID;
								PIDManagement(TRUE, stream_PID, TRUE);
								v->pat.pmt[nPMTIndex].es[i].nStreamType = stream_type;
								v->pat.pmt[nPMTIndex].es[i].pDescriptors = NULL;
								if (descriptors_loop_length)
								{
									v->pat.pmt[nPMTIndex].es[i].nDescriptorsLength = descriptors_loop_length;
									v->pat.pmt[nPMTIndex].es[i].pDescriptors = LocalAlloc(LPTR, descriptors_loop_length + 4);
									if (v->pat.pmt[nPMTIndex].es[i].pDescriptors != NULL)
									{
										int j;

										for (j = 0; j < descriptors_loop_length; j++)
											v->pat.pmt[nPMTIndex].es[i].pDescriptors[j] = get_bits(BM_PARSER_THREAD, 8);
									}
								}
								break;
							}
						}
						fRetVal = TRUE; // after this packet, we've got the ES stream info for the current program
					}
				}
			} while (TRUE);
			if (fRetVal == TRUE)
			{
				v->pat.pmt[nPMTIndex].fCompleted = TRUE;
				if (nProgramInfoLength)
				{
					v->pat.pmt[nPMTIndex].nProgramInfoLength = nProgramInfoLength;
					v->pat.pmt[nPMTIndex].pProgramInfo = LocalAlloc(LPTR, nProgramInfoLength + 4);
					memcpy(v->pat.pmt[nPMTIndex].pProgramInfo, pProgramInfo, nProgramInfoLength);
				}
			}
		}
	}

	return fRetVal;
}

BOOL ParseDCIIProgramNameMessage(BYTE * pSectionPointer, int nPacketLength, int nCurrentProgramNumber)
{
	int i;
	char program_name[256];
	char alternate_program_name[256];

	memset(program_name, 0, sizeof(program_name));
	memset(alternate_program_name, 0, sizeof(alternate_program_name));
	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int private_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved1 = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		DWORD ISO_639_language_code = get_bits(BM_PARSER_THREAD, 24);
		int service_number = get_bits(BM_PARSER_THREAD, 16);
		int reserved2 = get_bits(BM_PARSER_THREAD, 8);
		int sequence = get_bits(BM_PARSER_THREAD, 8);
		int program_epoch_number = get_bits(BM_PARSER_THREAD, 8);
		int program_name_control_byte = get_bits(BM_PARSER_THREAD, 8);
		int program_name_length = get_bits(BM_PARSER_THREAD, 8);

		for (i = 0; i < program_name_length; i++)
			program_name[i] = get_bits(BM_PARSER_THREAD, 8);
		{
			int alternate_program_name_length = get_bits(BM_PARSER_THREAD, 8);
			for (i = 0; i < alternate_program_name_length; i++)
				alternate_program_name[i] = get_bits(BM_PARSER_THREAD, 8);
		}
	}

	return FALSE;
}

BOOL ParsePMTPacket(BYTE * pSectionPointer, int nPacketLength, int nCurrentProgramNumber, int nPMTListenIndex)
{
	int nTableID, nSectionLength;
	int nProgramNumber;
	int nPCRPID;
	BOOL fRetVal = FALSE;
	BYTE * pProgramInfo;
	BYTE * pCIBase;
	int nCILength;

#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader:ParsePMTPacket+\n");
#endif DEBUG_MESSAGES

	if (nCurrentProgramNumber == 0)
	{
		fRetVal = FALSE;
		goto ParsePMTPacket_Windup;
	}

	// Check for stuffing
	do
	{
		if (*pSectionPointer != 0xff)
			break;
		pSectionPointer++;
		nPacketLength--;
	} while (nPacketLength > 0);
	if (nPacketLength == 0)
	{
		fRetVal = FALSE;
		goto ParsePMTPacket_Windup;
	}

	nTableID = pSectionPointer[0];

	//if (nTableID == 0xc1)		// DCII Program Name Message
	//	return (ParseDCIIProgramNameMessage(pSectionPointer, nPacketLength, nCurrentProgramNumber));
	if (nTableID == 0x40)	// DCII "PMT type thing"
		return (ParseDCIIPMTTypeThing(pSectionPointer, nPacketLength, nCurrentProgramNumber));

#ifdef PRO
	{
		double dCurrentTime = GetStreamMonitorTime();

		if (v->dSIParserTableTime[SI_PARSER_STATS_PMT] == 0.0)
			v->dSIParserTableTime[SI_PARSER_STATS_PMT] = dCurrentTime;
		if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_PMT] + 0.5)
		{
			v->nSIParserTimingErrors[SI_PARSER_STATS_PMT]++;			
		}
		v->dSIParserTableTime[SI_PARSER_STATS_PMT] = dCurrentTime;
	}
#endif PRO

	if (nTableID != 2)			// Standard MPEG-2 PMT
	{
#ifndef LITE
		if (v->fFastPMTParserDisabled == FALSE && nPMTListenIndex != -1)
		{
			// Let's see if this is a screwy ONN like feed with an extra
			// byte after the pointer
			if (   (pSectionPointer[0] == 0x00)
				&& (pSectionPointer[1] == 0x02) 
				&& ((pSectionPointer[2] & 0x80) == 0x80) 
				&& (v->pmtlisten[nPMTListenIndex].nPMTPointerKludge == 0) )
			{
				v->pmtlisten[nPMTListenIndex].nPMTPointerKludge = 1;
			}
			fRetVal = FALSE;
			goto ParsePMTPacket_Windup;
		}
#endif LITE

		// Let's see if this is a screwy ONN like feed with an extra
		// byte after the pointer
		if (   (pSectionPointer[0] == 0x00)
			&& (pSectionPointer[1] == 0x02) 
			&& ((pSectionPointer[2] & 0x80) == 0x80) 
			&& (v->nPMTPointerKludge == 0) )
		{
			v->nPMTPointerKludge = 1;
		}
#ifdef PRO
		else
			v->nSIParserTableErrors[SI_PARSER_STATS_PMT]++;
#endif PRO
		fRetVal = FALSE;
		goto ParsePMTPacket_Windup;
	}
	do
	{
		int nProgramInfoLength;
		int nVersionNumber;
		int nPMTIndex = -1;

		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
			goto ParsePMTPacket_Windup;

		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_PMT]++;
			if (!v->fIgnoreTableCRCErrors)
				goto ParsePMTPacket_Windup;
		}

		nProgramNumber = (pSectionPointer[3] << 8) + pSectionPointer[4];		
		nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
		nPCRPID = ((pSectionPointer[8] << 8) + pSectionPointer[9]) & 0x1fff;
		nProgramInfoLength = ((pSectionPointer[10] << 8) + pSectionPointer[11]) & 0xfff;
		
		pCIBase = &pSectionPointer[3];
		nCILength = nPacketLength;
		
		nSectionLength -= 12;
		nPacketLength -= 12;
		pSectionPointer += 12;

		// skip the program info descriptors
		if (nProgramInfoLength)
		{
			pProgramInfo = pSectionPointer;
			nSectionLength -= nProgramInfoLength;
			nPacketLength -= nProgramInfoLength;
			pSectionPointer += nProgramInfoLength;
		}

		// we can now process the ES stream pointers
		do
		{
			int nDescriptorTag;
			int nDescriptorTagLength;
			int nStreamType = pSectionPointer[0];
			int nESPID = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0x1fff;
			int nESInfoLength = ((pSectionPointer[3] << 8) + pSectionPointer[4]) & 0xff;

			nSectionLength -= 5;
			nPacketLength -= 5;
			pSectionPointer += 5;

			nDescriptorTag = pSectionPointer[0];
			nDescriptorTagLength = pSectionPointer[1];
			LogDescriptor(DESCRIPTOR_PMT, nDescriptorTag);

			if (nProgramNumber == nCurrentProgramNumber)
			{
				int i;
				nPMTIndex = -1;

				// find out offset in the list of channels in the pat structure
				for (i = 0; i < MAX_PAT_ENTRIES; i++)
				{
					if (nProgramNumber == v->pat.pmt[i].nProgramNumber)
					{
						nPMTIndex = i;
						break;
					}
				}
				
				if (nPMTIndex != -1) // should never happen!
				{
					if (v->pat.pmt[nPMTIndex].fCompleted)
					{
						if (v->pat.pmt[nPMTIndex].nVersionNumber != nVersionNumber)
						{
							// PMT change!
							int nESIndex;
							char szTemp[128];

							wsprintf(szTemp, "TSReader: PMT version change from %d to %d for program %d\n", 
								     v->pat.pmt[nPMTIndex].nVersionNumber, nVersionNumber, nProgramNumber);
							OutputDebugString(szTemp);

							for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
							{
								if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
									continue;
								if (v->pat.pmt[nPMTIndex].es[nESIndex].hESTreeItem != NULL)
									SendMessage(v->hDlgSIParser, WM_USER + 6, 0, (LPARAM)v->pat.pmt[nPMTIndex].es[nESIndex].hESTreeItem);	// removes the ES tree item
								EnterCriticalSection(&v->csThumbnails);
								if (v->pat.pmt[nPMTIndex].es[nESIndex].nDescriptorsLength)
									LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors);
								if (v->pat.pmt[nPMTIndex].es[nESIndex].pParsedData != NULL)
									LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pParsedData);
								if (v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame != NULL)
									LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame);
								memset(&v->pat.pmt[nPMTIndex].es[nESIndex], 0, sizeof(v->pat.pmt[nPMTIndex].es[nESIndex]));
								LeaveCriticalSection(&v->csThumbnails);
							}
							if (v->pat.pmt[nPMTIndex].hPCRTreeItem != NULL)
							{
								SendMessage(v->hDlgSIParser, WM_USER + 6, 0, (LPARAM)v->pat.pmt[nPMTIndex].hPCRTreeItem);	// removes the ES tree item
								v->pat.pmt[nPMTIndex].hPCRTreeItem = NULL;
							}
							v->pat.pmt[nPMTIndex].fCompleted = FALSE;
							if (nPMTIndex == v->nSelectedProgram)
								v->pat.pmt[nPMTIndex].fPostTreeAddSelect = TRUE;
							// todo - what about recordings in process?
						}
						else
							goto ParsePMTPacket_SkipES;	// process the rest
					}

					v->pat.pmt[nPMTIndex].nPCRPID = nPCRPID;
					v->pat.pmt[nPMTIndex].nVersionNumber = nVersionNumber;
					PIDManagement(TRUE, nPCRPID, TRUE);

					/*
					Analyse_PMT(pCIBase, nCILength,
								dwVPid, dwAPid, 
								v->start_CI[nPMTIndex], &v->nStartCILength[nPMTIndex],
								v->stop_CI[nPMTIndex], &v->nStopCILength[nPMTIndex]);*/
					if (nProgramInfoLength)
					{
						v->pat.pmt[nPMTIndex].nProgramInfoLength = nProgramInfoLength;
						v->pat.pmt[nPMTIndex].pProgramInfo = LocalAlloc(LPTR, nProgramInfoLength + 4);
						memcpy(v->pat.pmt[nPMTIndex].pProgramInfo, pProgramInfo, nProgramInfoLength);
						nProgramInfoLength = 0;	// stop from happening again on this PMT
					}

					if (nSectionLength > 0)
					{
						for (i = 0; i < MAX_ESLIST_ENTRIES; i++)
						{
							if (v->pat.pmt[nPMTIndex].es[i].nESPID == nESPID) // already got this one?
								break;
							if (v->pat.pmt[nPMTIndex].es[i].nESPID == 0) // end of table without finding this?
							{
								v->pat.pmt[nPMTIndex].es[i].nESPID = nESPID;
								PIDManagement(TRUE, nESPID, TRUE);
								v->pat.pmt[nPMTIndex].es[i].nStreamType = nStreamType;
								v->pat.pmt[nPMTIndex].es[i].pDescriptors = NULL;
								if (nESInfoLength)
								{
									v->pat.pmt[nPMTIndex].es[i].nDescriptorsLength = nESInfoLength;
									v->pat.pmt[nPMTIndex].es[i].pDescriptors = LocalAlloc(LPTR, nESInfoLength + 4);
									if (v->pat.pmt[nPMTIndex].es[i].pDescriptors != NULL)
									{
										int j = 0;
										memcpy(v->pat.pmt[nPMTIndex].es[i].pDescriptors, pSectionPointer, nESInfoLength);
										while (j < nESInfoLength)
										{
											LogDescriptor(DESCRIPTOR_PMT, v->pat.pmt[nPMTIndex].es[i].pDescriptors[j + 0]);
											j += v->pat.pmt[nPMTIndex].es[i].pDescriptors[j + 1] + 2;
										}
									}
								}
								break;
							}
						}
					}
					fRetVal = TRUE; // after this packet, we've got the ES stream info for the current program
				}
				else
				{
					OutputDebugString("Should never get here in PMT parsing\n");
				}
			}
ParsePMTPacket_SkipES:
			// skip the ES Info descriptors
			nSectionLength -= nESInfoLength;
			nPacketLength -= nESInfoLength;
			pSectionPointer += nESInfoLength;
		} while (nSectionLength > 4);
		if (nPMTIndex != -1)
		{
			v->pat.pmt[nPMTIndex].fCompleted = TRUE;
		}
		pSectionPointer += 4; // skip CRC
	} while (nPacketLength > 20);

ParsePMTPacket_Windup:
#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader:ParsePMTPacket-\n");
#endif DEBUG_MESSAGES
	return fRetVal;
}


// MPE section header - 12 bytes
// IP packet header - 20 bytes
// UDP packet header - 8 bytes

BOOL fSyncedUDPPacket = FALSE;

void ParseIPPacket(BYTE * pSectionPointer, int nPacketLength, int nPID, int nBufferNumber)
{
	int i;
	int llc_snap_offset = 0;
	int reserved;
	BOOL fAddit = TRUE;
	PIPMACENTRY pCurrentMac;
	PIPENTRY pCurrentIP;
	MPEIPPACKET mpe;


	
	// MPEG-2 section header - 3 bytes
	while (nPacketLength)
	{
		memset(&mpe, 0, sizeof(mpe));
		set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
		mpe.table_id = get_bits(BM_PARSER_THREAD, 8);
		if (mpe.table_id != 0x3e && mpe.table_id != 0x3f)	// DVB is 3e, ATSC 3f
			return;
		mpe.section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		mpe.private_indicator = get_bits(BM_PARSER_THREAD, 1);
		reserved = get_bits(BM_PARSER_THREAD, 2);
		mpe.section_length = get_bits(BM_PARSER_THREAD, 12);
		if ( (mpe.section_length <= 0) || (mpe.section_length > 65536) )
			return;

		// MPE header - 9 or 17 bytes
		mpe.MAC_Address[6 - 1] = get_bits(BM_PARSER_THREAD, 8);
		mpe.MAC_Address[5 - 1] = get_bits(BM_PARSER_THREAD, 8);
		reserved = get_bits(BM_PARSER_THREAD, 2);
		mpe.payload_scrambling_control = get_bits(BM_PARSER_THREAD, 2);
		mpe.address_scrambling_control = get_bits(BM_PARSER_THREAD, 2);
		mpe.LLC_SNAP_flag = get_bits(BM_PARSER_THREAD, 1);
		mpe.current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
		mpe.section_number = get_bits(BM_PARSER_THREAD, 8);
		mpe.last_section_number = get_bits(BM_PARSER_THREAD, 8);
		mpe.MAC_Address[4 - 1] = get_bits(BM_PARSER_THREAD, 8);
		mpe.MAC_Address[3 - 1] = get_bits(BM_PARSER_THREAD, 8);
		mpe.MAC_Address[2 - 1] = get_bits(BM_PARSER_THREAD, 8);
		mpe.MAC_Address[1 - 1] = get_bits(BM_PARSER_THREAD, 8);
		if (mpe.LLC_SNAP_flag == 1)
		{
			llc_snap_offset = 8;
			for (i = 0; i < llc_snap_offset; i++)
				reserved = get_bits(BM_PARSER_THREAD, 8);			
		}
		if (v->fDVBHMACs)
		{
			mpe.MAC_Address[0] = 0x01;
			mpe.MAC_Address[1] = 0x00;
			mpe.MAC_Address[2] = 0x5e;
			mpe.MAC_Address[3] = 0x00;
		}

		// IP header - 20 bytes
		mpe.IPHeader_Version = get_bits(BM_PARSER_THREAD, 4);
		switch(mpe.IPHeader_Version)
		{
		case 4:		// IPv4
			mpe.IPHeader_IHL = get_bits(BM_PARSER_THREAD, 4);
			mpe.IPHeader_TOS = get_bits(BM_PARSER_THREAD, 8);
			mpe.IPHeader_TotalLength = get_bits(BM_PARSER_THREAD, 16);
			mpe.IPHeader_Identification = get_bits(BM_PARSER_THREAD, 16);
			mpe.IPHeader_Flags = get_bits(BM_PARSER_THREAD, 3);
			mpe.IPHeader_FragmentOffset = get_bits(BM_PARSER_THREAD, 13);
			mpe.IPHeader_TTL = get_bits(BM_PARSER_THREAD, 8);
			mpe.IPHeader_Protocol = get_bits(BM_PARSER_THREAD, 8);
			mpe.IPHeader_HeaderChecksum = get_bits(BM_PARSER_THREAD, 16);
			mpe.IPHeader_SourceAddress = get_bits(BM_PARSER_THREAD, 32);
			mpe.IPHeader_DestinationAddress = get_bits(BM_PARSER_THREAD, 32);
			break;
		case 6:		// IPv6
			mpe.IPv6Header_TrafficClass = get_bits(BM_PARSER_THREAD, 8);
			mpe.IPv6Header_FlowLabel = get_bits(BM_PARSER_THREAD, 20);
			mpe.IPv6Header_PayloadLength = get_bits(BM_PARSER_THREAD, 16);
			mpe.IPv6Header_NextHeader = get_bits(BM_PARSER_THREAD, 8);
			mpe.IPv6Header_HopLimit= get_bits(BM_PARSER_THREAD, 8);
			for (i = 0; i < 16; i++)
				mpe.IPv6Header_SourceAddress[i] = get_bits(BM_PARSER_THREAD, 8);
			for (i = 0; i < 16; i++)
				mpe.IPv6Header_DestinationAddress[i] = get_bits(BM_PARSER_THREAD, 8);
			if (mpe.IPv6Header_PayloadLength == 0)
				goto ParseIPPacket_NextSection;		// can't do much with that now
			break;
		}

#ifdef _DEBUGx
		/* 
		if (mpe.IPHeader_FragmentOffset == 0)
		{
			int i;
			DWORD dwWritten;
			BYTE * szInData = &pSectionPointer[32 + llc_snap_offset];
			char szDebug[512] = {0};
			char szHex[256] = {0};
			char szASCII[256] = {0};

			for (i = 0; i < 48; i++)
			{
				char szTemp2[8];
				wsprintf(szTemp2, "%02x ", *szInData);
				lstrcat(szHex, szTemp2);
				if (*szInData < ' ')
					lstrcpy(szTemp2, ".");
				else
					wsprintf(szTemp2, "%c", *szInData);
				lstrcat(szASCII, szTemp2);
				szInData++;
			}
			wsprintf(szDebug, "Len = %04d %s %s\r\n",
					 mpe.IPHeader_TotalLength,
					 szHex,
					 szASCII);
			OutputDebugString(szDebug);
			WriteFile(v->hDebugFile, szDebug, lstrlen(szDebug), &dwWritten, NULL);
		}
		*/
#endif _DEBUGx

		// Send over to any user filters
		if (mpe.IPHeader_Version == 4)
			MD__IPDataToFilters(nPID, pSectionPointer, &mpe);
		
		// Setup the root (PID) item if the first time
		if (v->ippid[nBufferNumber].nPID == 0)
		{
			v->ippid[nBufferNumber].nPID = nPID;
			PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_IP_PID, nBufferNumber);
		}

		v->ippid[nBufferNumber].nPacketCount++;
		v->ippid[nBufferNumber].nByteCount += nPacketLength;

		if (v->ippid[nBufferNumber].hIPPIDRootItem == NULL)
		{
			// Can't do much with this until we have the PID item setup
			goto ParseIPPacket_NextSection;
		}

		// See if this MAC address already exists in the list of MACs on this PID
		if (v->ippid[nBufferNumber].pMACEntries == NULL)
		{
			// First time - allocate this item
			v->ippid[nBufferNumber].pMACEntries = LocalAlloc(LPTR, sizeof(IPMACENTRY) + 4);
			pCurrentMac = v->ippid[nBufferNumber].pMACEntries;
		}
		else
		{
			PIPMACENTRY pPreviousMac;
			
			pCurrentMac = v->ippid[nBufferNumber].pMACEntries;
			pPreviousMac = pCurrentMac;

			do
			{
				if (memcmp(pCurrentMac->bMAC, mpe.MAC_Address, sizeof(mpe.MAC_Address)) == 0)
				{
					// found it!
					pCurrentMac->nPacketCount++;
					pCurrentMac->nByteCount += mpe.section_length;
					fAddit = FALSE;
					break;
				}
				pPreviousMac = pCurrentMac;
				pCurrentMac = (PIPMACENTRY)pCurrentMac->dwNext;
			} while (pCurrentMac != NULL);

			if (fAddit)
			{
				// Didn't find it - so allocate a new one and link it to the previous
				pCurrentMac = LocalAlloc(LPTR, sizeof(IPMACENTRY) + 4);
				pPreviousMac->dwNext = (DWORD)pCurrentMac;
			}
		}

		if (fAddit)
		{
			// Add the new item at pCurrentMac
			for (i = 0; i < 6; i++)
				pCurrentMac->bMAC[i] = mpe.MAC_Address[i];
			pCurrentMac->nPacketCount = 0;
			pCurrentMac->hIPPIDRootItem = v->ippid[nBufferNumber].hIPPIDRootItem;
			PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_IP_MAC, (LPARAM)pCurrentMac);
		}

		if (pCurrentMac == NULL || pCurrentMac->hIPMacItem == NULL)
		{
			// No point doing this until the item is created
			goto ParseIPPacket_NextSection;
		}

		fAddit = TRUE;
		if (pCurrentMac->pIPEntries == NULL)
		{
			pCurrentMac->pIPEntries = LocalAlloc(LPTR, sizeof(IPENTRY) + 4);
			pCurrentIP = pCurrentMac->pIPEntries;
		}
		else
		{
			PIPENTRY pPreviousIP;

			pCurrentIP = pCurrentMac->pIPEntries;
			pPreviousIP = pCurrentIP;
			do
			{
				BOOL fMatched = FALSE;

				if (mpe.IPHeader_Version == 4)
				{
					if ( (mpe.IPHeader_DestinationAddress == pCurrentIP->dwDestinationAddress)
					  && ((DWORD)mpe.IPHeader_Protocol == pCurrentIP->dwProtocol) )
					  fMatched = TRUE;
				}
				else if (mpe.IPHeader_Version == 6)
				{
					if (memcmp(mpe.IPv6Header_DestinationAddress, pCurrentIP->bDestinationAddressIPv6, 16) == 0)
					{
						if ((DWORD)mpe.IPv6Header_NextHeader == pCurrentIP->dwProtocol)
							fMatched = TRUE;
					}
				}
				if (fMatched)
				{
					DWORD dwWritten = 0;

					fAddit = FALSE;
					pCurrentIP->nPacketCount++;
					switch(pCurrentIP->dwProtocol)
					{
					case IP_UDP_ID: // UDP
						if (mpe.IPHeader_Version == 4)
							pCurrentIP->nByteCount += mpe.IPHeader_TotalLength - 8;
						else if (mpe.IPHeader_Version == 6)
							pCurrentIP->nByteCount += mpe.IPv6Header_PayloadLength;
#ifndef LITE
						if (pCurrentIP->hSaveFile != NULL)
						{
							switch(pCurrentIP->nSaveMode)
							{
							case IP_SAVE_PAYLOAD:
								if (mpe.IPHeader_Version == 4)
								{
									if (mpe.IPHeader_FragmentOffset == 0)
									{
										WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[32 + llc_snap_offset + 8], mpe.IPHeader_TotalLength - 28, &dwWritten, NULL);
										pCurrentIP->fGotFirstFragment = TRUE;
									}
									else
									{
										if (pCurrentIP->fGotFirstFragment == TRUE)
											WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[32 + llc_snap_offset], mpe.IPHeader_TotalLength - 20, &dwWritten, NULL);
									}
								}
								else if (mpe.IPHeader_Version == 6)
								{
								}
								break;
							case IP_SAVE_PAYLOAD_IP:
								if (mpe.IPHeader_Version == 4)
									WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[12 + llc_snap_offset], mpe.IPHeader_TotalLength, &dwWritten, NULL);
								else if (mpe.IPHeader_Version == 6)
									WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[12 + llc_snap_offset], 40 + mpe.IPv6Header_PayloadLength, &dwWritten, NULL);
								break;
							case IP_SAVE_PAYLOAD_IP_MPE:
								WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[3], mpe.section_length, &dwWritten, NULL);
								break;
							}
						}

						if (pCurrentIP->fTransmitting == TRUE)
						{
							int nEthernetLength = 0;
							int i;
							BYTE pEthernetBuffer[1514];

							// Destination Ethernet address
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[0];
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[1];
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[2];
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[3];
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[4];
							pEthernetBuffer[nEthernetLength++] = mpe.MAC_Address[5];

							// Souce Ethernet address
							pEthernetBuffer[nEthernetLength++] = 0x00;
							pEthernetBuffer[nEthernetLength++] = 0x09;
							pEthernetBuffer[nEthernetLength++] = 0x6b;
							pEthernetBuffer[nEthernetLength++] = 0x06;
							pEthernetBuffer[nEthernetLength++] = 0x17;
							pEthernetBuffer[nEthernetLength++] = 0xe3;

							// Packet type (Ethernet 2 network) IP
							pEthernetBuffer[nEthernetLength++] = 0x08;
							pEthernetBuffer[nEthernetLength++] = 0x00;

							// copy over the IP/UDP packet
							for (i = 0; i < mpe.IPHeader_TotalLength; i++)
							{
								if (nEthernetLength == sizeof(pEthernetBuffer))
								{
									OutputDebugString("TSReader: Ethernet packet buffer too small\n");
									break;
								}
								pEthernetBuffer[nEthernetLength++] = pSectionPointer[i + 12 + llc_snap_offset];
							}
							UDPSender_SendPacket(pEthernetBuffer, nEthernetLength);
						}
#endif LITE
						break;
					case 6:	// TCP
						pCurrentIP->nByteCount += mpe.IPHeader_TotalLength - 20;
#ifndef LITE
						if (pCurrentIP->hSaveFile != NULL)
						{
							if (mpe.IPHeader_Version == 4)
							{
								switch(pCurrentIP->nSaveMode)
								{
								case IP_SAVE_PAYLOAD:
									WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[32 + llc_snap_offset + 20], mpe.IPHeader_TotalLength - 40, &dwWritten, NULL);
									break;
								case IP_SAVE_PAYLOAD_IP:
									WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[12 + llc_snap_offset], mpe.IPHeader_TotalLength, &dwWritten, NULL);
									break;
								case IP_SAVE_PAYLOAD_IP_MPE:
									WriteFile(pCurrentIP->hSaveFile, &pSectionPointer[3], mpe.section_length, &dwWritten, NULL);
									break;
								}
							}
						}
#endif LITE
						break;
					}
					break;
				}
				pPreviousIP = pCurrentIP;
				pCurrentIP = (PIPENTRY)pCurrentIP->dwNext;
			} while (pCurrentIP != NULL);

			if (fAddit)
			{
				pCurrentIP = LocalAlloc(LPTR, sizeof(IPENTRY) + 4);
				pPreviousIP->dwNext = (DWORD)pCurrentIP;
			}
		}

		if (fAddit)
		{
			if (mpe.IPHeader_Version == 4)
			{
				pCurrentIP->dwDestinationAddress = mpe.IPHeader_DestinationAddress;
				pCurrentIP->dwProtocol = mpe.IPHeader_Protocol;
			}
			else if (mpe.IPHeader_Version == 6)
			{
				memcpy(pCurrentIP->bDestinationAddressIPv6, mpe.IPv6Header_DestinationAddress, 16);
				pCurrentIP->dwProtocol = mpe.IPv6Header_NextHeader;
			}
			pCurrentIP->nIPVersion = mpe.IPHeader_Version;
			pCurrentIP->hIPMacItem = pCurrentMac->hIPMacItem;
			PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_IP_IP, (LPARAM)pCurrentIP);
		}

		// end of do loop - bump pointers
ParseIPPacket_NextSection:
		nPacketLength -= mpe.section_length + 3;
		pSectionPointer += mpe.section_length + 3;
	}
}

BOOL ParsePATPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nTableID, nSectionLength;
	int nSectionNumber, nLastSectionNumber;
	int nVersionNumber;
	int nTransportStreamID;
	BOOL fRetVal = TRUE;

#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader: ParsePATPacket+\n");
#endif DEBUG_MESSAGES

	// Check for stuffing
	do
	{
		if (*pSectionPointer != 0xff)
			break;
		pSectionPointer++;
		nPacketLength--;
	} while (nPacketLength > 0);
	if (nPacketLength == 0)
	{
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}

	nTableID = pSectionPointer[0];
	if (nTableID != 0)
	{
#ifdef PRO
		v->nSIParserTableErrors[SI_PARSER_STATS_PAT]++;
#endif PRO
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}

	nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
	if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
	{
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}

	if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
	{
		// Let's see if this is a screwy ONN like feed with an extra
		// byte after the pointer
		if (   (pSectionPointer[0] == 0x00)
			&& (pSectionPointer[1] == 0x00) 
			&& ((pSectionPointer[2] & 0x80) == 0x80) 
			&& (v->nPATPointerKludge == 0) )
		{
			v->nPATPointerKludge = 1;
			fRetVal = FALSE;
			goto ParsePATPacket_Windup;
		}
		v->nSIParserCRCs[SI_PARSER_STATS_PAT]++;
		if (!v->fIgnoreTableCRCErrors)
		{
			fRetVal = FALSE;
			goto ParsePATPacket_Windup;
		}
	}

	if ((pSectionPointer[5] & 1) == 0)	// not current_next
	{
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}

#ifdef PRO
	{
		double dCurrentTime = GetStreamMonitorTime();

		if (v->dSIParserTableTime[SI_PARSER_STATS_PAT] == 0.0)
			v->dSIParserTableTime[SI_PARSER_STATS_PAT] = dCurrentTime;
		if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_PAT] + 0.5)
		{
			v->nSIParserTimingErrors[SI_PARSER_STATS_PAT]++;			
		}
		v->dSIParserTableTime[SI_PARSER_STATS_PAT] = dCurrentTime;
	}
#endif PRO

	nTransportStreamID = (pSectionPointer[3] << 8) + pSectionPointer[4];
	nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
	nSectionNumber = pSectionPointer[6];
	nLastSectionNumber = pSectionPointer[7];

	if (nVersionNumber > v->nLastPATHighestVersionNumber)
	{
		v->nLastPATHighestVersionNumber = nVersionNumber;
		v->nCheckPATCounter = 0;
	}

	if (v->nCheckPATCounter++ < v->nMinimumPATs)
	{
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}
	if (nVersionNumber != v->nLastPATHighestVersionNumber)
	{
		fRetVal = FALSE;
		goto ParsePATPacket_Windup;
	}

	// If it's a different version, let's re-generate the PMT in memory
	if (nVersionNumber != v->pat.nVersionNumber)
	{
		//HTREEITEM hSave = v->pat.hPATTreeItem;
		//memset(&v->pat, 0, sizeof(v->pat));
		//v->pat.hPATTreeItem = hSave;
		v->pat.nVersionNumber = nVersionNumber;
		v->pat.nTransportStreamID = nTransportStreamID;
	}

#ifndef LITE
	v->pat.pRawPAT = LocalAlloc(LMEM_FIXED, nPacketLength + 4);
	memcpy(v->pat.pRawPAT, pSectionPointer, nPacketLength);
#endif LITE

	nSectionLength -= 5;
	pSectionPointer += 8;
	do
	{
		int nProgramNumber, nPMTPID;
		int i;

		nProgramNumber = (pSectionPointer[0] << 8) + pSectionPointer[1];
		nPMTPID = ((pSectionPointer[2] << 8) + pSectionPointer[3]) & 0x1fff;
		for (i = 0; i < MAX_PAT_ENTRIES; i++)
		{
			if (v->pat.pmt[i].nPMTPID == 0)
			{
				{
					char szTemp[128];
					wsprintf(szTemp, "TSReader: Program = %d PMTPID = %x Item #%d\n", nProgramNumber, nPMTPID, i);
					OutputDebugString(szTemp);
				}
				v->pat.pmt[i].nPMTPID = nPMTPID;
				v->pat.pmt[i].nProgramNumber = nProgramNumber;
				if (nProgramNumber == 0)
				{
					if (v->nNetworkPID == -1)
						v->nNetworkPID = nPMTPID;
				}
				break;
			}
		}

		pSectionPointer += 4;
		nSectionLength -= 4;
	} while (nSectionLength > 4);
	fRetVal = (nSectionNumber == nLastSectionNumber);

ParsePATPacket_Windup:
#ifdef DEBUG_MESSAGES
	OutputDebugString("TSReader: ParsePATPacket-\n");
#endif DEBUG_MESSAGES
	return fRetVal;
}

BOOL ParsePSIPPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nTableID, nSectionLength;
	BYTE * pOriginalSectionPointer = pSectionPointer;

	while (nPacketLength)
	{
		pSectionPointer = pOriginalSectionPointer;

		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;

		
		nTableID = pSectionPointer[0];
		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 4096) )
			return FALSE;
		nPacketLength -= nSectionLength + 3;
		pOriginalSectionPointer += nSectionLength + 3;

		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_SDT]++;
			if (!v->fIgnoreTableCRCErrors)
				return FALSE;
		}

		v->nSIParserPackets[SI_PARSER_STATS_SDT]++;
		switch(nTableID)
		{
		case 0xc7:		// MGT
			{
				int tables_defined = pSectionPointer[9] << 8 | pSectionPointer[10];

				if (tables_defined)
				{
					int table;
					int i;
					int descriptors_length;

					pSectionPointer += 11;		// point to first table entry
					//nPacketLength -= 11;
					for (table = 0; table < tables_defined; table++)
					{
						BYTE * pDescriptorsPtr;
						int table_type = pSectionPointer[0] << 8 | pSectionPointer[1];
						int table_type_PID = (pSectionPointer[2] << 8 | pSectionPointer[3]) & 0x1fff;
						int table_type_descriptors_length = (pSectionPointer[9] << 8 | pSectionPointer[10]) & 0xfff;
						pSectionPointer += 11;		// point to next table entry or descriptor
						//nPacketLength -= 11;
						if (table_type_descriptors_length)
						{
							pDescriptorsPtr = pSectionPointer;
							pSectionPointer += table_type_descriptors_length;
							//nPacketLength -= table_type_descriptors_length;
						}

						// See if we've seen this entry already
						for (i = 0; i < MAX_MGT_ENTRIES; i++)
						{
							if ((v->mgt[i].nTableType == table_type) && (v->mgt[i].nTablePID == table_type_PID))
								break;
							if (v->mgt[i].nTableType == -1 && v->mgt[i].nTableType != 0)
							{
								v->mgt[i].nTableType = table_type;
								v->mgt[i].nTablePID = table_type_PID;
								if (table_type_descriptors_length)
								{
									int j = 0;

									v->mgt[i].nDescriptorsLength = table_type_descriptors_length;
									v->mgt[i].pDescriptors = LocalAlloc(LPTR, table_type_descriptors_length + 4);
									memcpy(v->mgt[i].pDescriptors, pDescriptorsPtr, table_type_descriptors_length);
									while (j < table_type_descriptors_length)
									{
										LogDescriptor(DESCRIPTOR_MGT, v->mgt[i].pDescriptors[j + 0]);
										j += v->mgt[i].pDescriptors[j + 0] + 2;
									}
								}
								PIDManagement(TRUE, v->mgt[i].nTablePID, TRUE);
								if (i == 0)
									PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_MGT, 0);
								if (table_type >= 0x100 && table_type <= 0x13f)
									v->nATSCEITPID[table_type - 0x100] = table_type_PID;
								else if (table_type >= 0x200 && table_type <= 0x23f)
									v->nATSCETTPID[table_type - 0x200] = table_type_PID;
								else if (table_type == 0x004)
									v->nATSCCETTPID = table_type_PID;
								break;
							}
						}
					}
					descriptors_length = (pSectionPointer[0] << 8 | pSectionPointer[1]) & 0x0fff;
					if (descriptors_length)
					{
						if (v->pMGTDescriptors == NULL)
						{
							int j = 0;

							v->nMGTDescriptorsLength = descriptors_length;
							v->pMGTDescriptors = LocalAlloc(LPTR, descriptors_length + 4);
							memcpy(v->pMGTDescriptors, &pSectionPointer[2], descriptors_length);
							while (j < descriptors_length)
							{
								LogDescriptor(DESCRIPTOR_MGT, v->pMGTDescriptors[j + 0]);
								j += v->pMGTDescriptors[j + 0] + 2;
							}
						}
					}				
				}
			}
			break;
		case 0xc8:		// TVCT
			{
				int transport_stream_id = pSectionPointer[3] << 8 | pSectionPointer[4];
				int num_channels_in_section = pSectionPointer[9];
				if (num_channels_in_section)
				{
					int i;

					pSectionPointer += 10;		// skip to the first channel's data
					//nPacketLength -= 10;

					for (i = 0; i < num_channels_in_section; i++)
					{
						int descriptors_length = (pSectionPointer[30] << 8 | pSectionPointer[31]) & 0x3ff;
						int major_channel_number = ((pSectionPointer[14] << 16 | pSectionPointer[15] << 8 | pSectionPointer[16]) >> 10) & 0x3ff;
						int minor_channel_number = (pSectionPointer[14] << 16 | pSectionPointer[15] << 8 | pSectionPointer[16]) & 0x3ff;
						int carrier_frequency = pSectionPointer[18] << 24 | pSectionPointer[19] << 16 | pSectionPointer[20] << 8 | pSectionPointer[21];
						int channel_TSID = pSectionPointer[22] << 8 | pSectionPointer[23];
						int program_number = pSectionPointer[24] << 8 | pSectionPointer[25];
						int source_id = pSectionPointer[28] << 8 | pSectionPointer[29];
						int modulation_mode = pSectionPointer[17];
						int j;
						EITCHANNELDATA thischanneldata;

						memset(&thischanneldata, 0, sizeof(thischanneldata));
						thischanneldata.nTransportStreamID = channel_TSID;
						thischanneldata.nChannelNumber = program_number;
						thischanneldata.nMajorChannelNumber = major_channel_number;
						thischanneldata.nMinorChannelNumber = minor_channel_number;
						thischanneldata.nCarrierFrequency = carrier_frequency;
						thischanneldata.nModulationMode = modulation_mode;
						thischanneldata.fATSC = TRUE;
						thischanneldata.nSourceID = source_id;

						for (j = 0; j < 7; j++)
							thischanneldata.szShortName[j] = pSectionPointer[j * 2 + 1];
						thischanneldata.szShortName[7] = 0;

						pSectionPointer += 32;		// skip to the next channel's data or the descriptors
						//nPacketLength -= 32;

						if (descriptors_length)
						{
							do
							{
								// Get data from each descriptor
								int nDescriptorLength, nDescriptorTag;
								int k;

								nDescriptorTag = pSectionPointer[0];
								nDescriptorLength = pSectionPointer[1] + 2;
								LogDescriptor(DESCRIPTOR_VCT, nDescriptorTag);

								for (k = 0; k < MAX_SDT_EXTRA_DESCRIPTORS; k++)
								{
									if (thischanneldata.pExtraDescriptors[k] != NULL)
									{
										if (IsDuplicateDescriptor(thischanneldata.pExtraDescriptors[k], pSectionPointer) == TRUE)
											break;
									}
									if (thischanneldata.pExtraDescriptors[k] == NULL)
									{
										thischanneldata.pExtraDescriptors[k] = LocalAlloc(LPTR, nDescriptorLength + 4);
										memcpy(thischanneldata.pExtraDescriptors[k], &pSectionPointer[0], nDescriptorLength);
										if (pSectionPointer[0] == 0xa0)
											GetExtendedChannelName(pSectionPointer, thischanneldata.szLongName);
										break;
									}
								}

								pSectionPointer += nDescriptorLength;
								//nPacketLength -= nDescriptorLength;
								descriptors_length -= nDescriptorLength;

							} while (descriptors_length > 0);
						}

						// We now have a completed EITCHANNELDATA - check if this channel
						// needs to be added
						if (v->pChannelData[thischanneldata.nChannelNumber] == NULL)
						{
							v->pChannelData[thischanneldata.nChannelNumber] = LocalAlloc(LPTR, sizeof(EITCHANNELDATA) + 4);
							memcpy(v->pChannelData[thischanneldata.nChannelNumber], &thischanneldata, sizeof(EITCHANNELDATA));
							v->nEITChannels++;
							PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_VCT, thischanneldata.nChannelNumber);
						}
						else
						{
							int k;

							for (k = 0; k < MAX_SDT_EXTRA_DESCRIPTORS; k++)
							{
								if (thischanneldata.pExtraDescriptors[k] == NULL)
									break;
								LocalFree(thischanneldata.pExtraDescriptors[k]);
								thischanneldata.pExtraDescriptors[k] = NULL;
							}
						}
					}
				}
			}
			break;
		case 0xc9: // CVCT
			set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
			{
				int i;
				int nCVCTIndex;
				BOOL fNewTransportID = TRUE;

				int table_id = get_bits(BM_PARSER_THREAD, 8);
				int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
				int private_indicator = get_bits(BM_PARSER_THREAD, 1);
				int reserved1 = get_bits(BM_PARSER_THREAD, 2);
				int section_length = get_bits(BM_PARSER_THREAD, 12);

				int transport_stream_id = get_bits(BM_PARSER_THREAD, 16);
				int reserved2 = get_bits(BM_PARSER_THREAD, 2);
				int version_number = get_bits(BM_PARSER_THREAD, 5);
				int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
				int section_number = get_bits(BM_PARSER_THREAD, 8);
				int last_section_number = get_bits(BM_PARSER_THREAD, 8);
				int protocol_version = get_bits(BM_PARSER_THREAD, 8);
				int num_channels_in_section = get_bits(BM_PARSER_THREAD, 8);
				if (num_channels_in_section == 0)
					return FALSE;
				
				// See if we have this CVCT already
				for (nCVCTIndex = 0; nCVCTIndex < MAX_CVCT_ENTRIES; nCVCTIndex++)
				{
					if (v->cvct[nCVCTIndex].transport_stream_id == transport_stream_id)
					{
						fNewTransportID = FALSE;
						break;
					}
					if (v->cvct[nCVCTIndex].transport_stream_id == -1)
						break;				
				}
				if (nCVCTIndex == MAX_CVCT_ENTRIES)
				{
					OutputDebugString("TSReader: No space available in CVCT\n");
					return FALSE;	// no more room
				}
				v->cvct[nCVCTIndex].transport_stream_id = transport_stream_id;
				v->cvct[nCVCTIndex].version_number = version_number;
				
				for (i = 0; i < num_channels_in_section; i++)
				{
					int j;
					char short_name[8];
					for (j = 0; j < 7; j++)
					{
						short_name[j] = get_bits(BM_PARSER_THREAD, 16) & 0xff; // unicode
					}
					short_name[j] = '\0';
					{
						BOOL fAlreadyGotThisOne = FALSE;

						int reserved3 = get_bits(BM_PARSER_THREAD, 4);
						int major_channel_number = get_bits(BM_PARSER_THREAD, 10);
						int minor_channel_number = get_bits(BM_PARSER_THREAD, 10);
						int modulation_mode = get_bits(BM_PARSER_THREAD, 8);
						int carrier_frequency = get_bits(BM_PARSER_THREAD, 32);
						int channel_TSID = get_bits(BM_PARSER_THREAD, 16);
						int program_number = get_bits(BM_PARSER_THREAD, 16);
						int ETM_location = get_bits(BM_PARSER_THREAD, 2);
						int access_controlled = get_bits(BM_PARSER_THREAD, 1);
						int hidden = get_bits(BM_PARSER_THREAD, 1);
						int path_select = get_bits(BM_PARSER_THREAD, 1);
						int out_of_band = get_bits(BM_PARSER_THREAD, 1);
						int hide_guide = get_bits(BM_PARSER_THREAD, 1);
						int reserved4 = get_bits(BM_PARSER_THREAD, 3);
						int service_type = get_bits(BM_PARSER_THREAD, 6);
						int source_id = get_bits(BM_PARSER_THREAD, 16);
						int reserved5 = get_bits(BM_PARSER_THREAD, 6);
						int descriptors_length = get_bits(BM_PARSER_THREAD, 10);

						for (j = 0; j < MAX_CVCT_CHANNEL_ENTRIES; j++)
						{
							if (v->cvct[nCVCTIndex].CVCTEntry[j].major_channel_number == major_channel_number &&
								v->cvct[nCVCTIndex].CVCTEntry[j].minor_channel_number == minor_channel_number)
							{
								fAlreadyGotThisOne = TRUE;
								break;
							}
							if (v->cvct[nCVCTIndex].CVCTEntry[j].major_channel_number == -1 &&
								v->cvct[nCVCTIndex].CVCTEntry[j].minor_channel_number == -1)
								break;
						}
						if (j == MAX_CVCT_CHANNEL_ENTRIES)
						{
							OutputDebugString("TSReader: Out of room for channel CVCT\n");
							return FALSE;
						}

						if (!fAlreadyGotThisOne)
						{
							EITCHANNELDATA thischanneldata;

							v->cvct[nCVCTIndex].num_channels_in_section++;
							lstrcpy(v->cvct[nCVCTIndex].CVCTEntry[j].szShortName, short_name);
							v->cvct[nCVCTIndex].CVCTEntry[j].major_channel_number = major_channel_number;
							v->cvct[nCVCTIndex].CVCTEntry[j].minor_channel_number = minor_channel_number;
							v->cvct[nCVCTIndex].CVCTEntry[j].modulation_mode = modulation_mode;
							v->cvct[nCVCTIndex].CVCTEntry[j].carrier_frequency = carrier_frequency;
							v->cvct[nCVCTIndex].CVCTEntry[j].channel_TSID = channel_TSID;
							v->cvct[nCVCTIndex].CVCTEntry[j].program_number = program_number;
							v->cvct[nCVCTIndex].CVCTEntry[j].ETM_location = ETM_location;
							v->cvct[nCVCTIndex].CVCTEntry[j].access_controlled = access_controlled;
							v->cvct[nCVCTIndex].CVCTEntry[j].hidden = hidden;
							v->cvct[nCVCTIndex].CVCTEntry[j].path_select = path_select;
							v->cvct[nCVCTIndex].CVCTEntry[j].out_of_band = out_of_band;
							v->cvct[nCVCTIndex].CVCTEntry[j].hide_guide = hide_guide;
							v->cvct[nCVCTIndex].CVCTEntry[j].service_type = service_type;
							v->cvct[nCVCTIndex].CVCTEntry[j].source_id = source_id;
							v->cvct[nCVCTIndex].CVCTEntry[j].descriptors_length = descriptors_length;

							memset(&thischanneldata, 0, sizeof(thischanneldata));
							thischanneldata.nTransportStreamID = channel_TSID;
							thischanneldata.nChannelNumber = program_number;
							thischanneldata.nMajorChannelNumber = major_channel_number;
							thischanneldata.nMinorChannelNumber = minor_channel_number;
							thischanneldata.nCarrierFrequency = carrier_frequency;
							thischanneldata.nModulationMode = modulation_mode;
							thischanneldata.fATSC = TRUE;
							thischanneldata.nSourceID = source_id;
							lstrcpy(thischanneldata.szShortName, short_name);
							if (v->pChannelData[thischanneldata.nChannelNumber] == NULL)
							{
								v->pChannelData[thischanneldata.nChannelNumber] = LocalAlloc(LPTR, sizeof(EITCHANNELDATA) + 4);
								memcpy(v->pChannelData[thischanneldata.nChannelNumber], &thischanneldata, sizeof(EITCHANNELDATA));
								v->nEITChannels++;
							}
						}

						if (descriptors_length)
						{
							int k;

							if (!fAlreadyGotThisOne)
								v->cvct[nCVCTIndex].CVCTEntry[j].pDescriptors = LocalAlloc(LPTR, descriptors_length + 4);
							for (k = 0; k < descriptors_length; k++)
							{
								if (!fAlreadyGotThisOne)
									v->cvct[nCVCTIndex].CVCTEntry[j].pDescriptors[k] = get_bits(BM_PARSER_THREAD, 8);
								else
									get_bits(BM_PARSER_THREAD, 8);
							}
							k = 0;
							while (k < descriptors_length)
							{
								LogDescriptor(DESCRIPTOR_VCT, v->cvct[nCVCTIndex].CVCTEntry[j].pDescriptors[k + 0]);
								k += v->cvct[nCVCTIndex].CVCTEntry[j].pDescriptors[k + 1] + 2;
							}
						}
					}
				}
				{
					int j;
					int reserved = get_bits(BM_PARSER_THREAD, 6);
					int additional_descriptors_length = get_bits(BM_PARSER_THREAD, 10);
					if (additional_descriptors_length)
					{
						v->cvct[nCVCTIndex].additional_descriptors_length = additional_descriptors_length;
						v->cvct[nCVCTIndex].pAdditionalDescriptors = LocalAlloc(LPTR, additional_descriptors_length + 4);
						for(j = 0; j < additional_descriptors_length; j++)
							v->cvct[nCVCTIndex].pAdditionalDescriptors[j] = get_bits(BM_PARSER_THREAD, 8);
						j = 0;
						while (j < additional_descriptors_length)
						{
							LogDescriptor(DESCRIPTOR_VCT, v->cvct[nCVCTIndex].pAdditionalDescriptors[j + 0]);
							j += v->cvct[nCVCTIndex].pAdditionalDescriptors[j + 1] + 2;
						}
					}
				}

				// If we made it here, we've just added a new CVCT
				if (fNewTransportID)
					PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_CVCT, nCVCTIndex);
			}
			break;
		case 0xca:		// RRT
			set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
			{
				int table_id = get_bits(BM_PARSER_THREAD, 8);
				int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
				int private_indicator = get_bits(BM_PARSER_THREAD, 1);
				int reserved1 = get_bits(BM_PARSER_THREAD, 2);
				int section_length = get_bits(BM_PARSER_THREAD, 12);
				int reserved2 = get_bits(BM_PARSER_THREAD, 8);
				int rating_region = get_bits(BM_PARSER_THREAD, 8);
				if (v->prrt[rating_region] == NULL)
				{
					int reserved3 = get_bits(BM_PARSER_THREAD, 2);
					int version_number = get_bits(BM_PARSER_THREAD, 5);
					int current_next_indicator = get_bits(BM_PARSER_THREAD, 1);
					int section_number = get_bits(BM_PARSER_THREAD, 8);
					int last_section_number = get_bits(BM_PARSER_THREAD, 8);
					int protocol_version = get_bits(BM_PARSER_THREAD, 8);
					int ration_region_name_length = get_bits(BM_PARSER_THREAD, 8);

					v->prrt[rating_region] = LocalAlloc(LPTR, sizeof(RRT) + 4);
									
					memset(v->prrt[rating_region]->szRegionName,
						   0,
						   sizeof(v->prrt[rating_region]->szRegionName));
					GetATSCMultipleString(BM_PARSER_THREAD, v->prrt[rating_region]->szRegionName, ration_region_name_length);		
					{
						int i;
						int dimensions_defined = get_bits(BM_PARSER_THREAD, 8);

						v->prrt[rating_region]->nDimensionsDefined = dimensions_defined;
						for (i = 0; i < dimensions_defined; i++)
						{
							int dimension_name_length = get_bits(BM_PARSER_THREAD, 8);
							memset(v->prrt[rating_region]->rrtdimension[i].szRatingDimension,
								   0,
								   sizeof(v->prrt[rating_region]->rrtdimension[i].szRatingDimension));
							GetATSCMultipleString(BM_PARSER_THREAD, v->prrt[rating_region]->rrtdimension[i].szRatingDimension, dimension_name_length);
							{
								int j;

								int reserved4 = get_bits(BM_PARSER_THREAD, 3);
								int graduated_scale = get_bits(BM_PARSER_THREAD, 1);
								int values_defined = get_bits(BM_PARSER_THREAD, 4);
								
								v->prrt[rating_region]->rrtdimension[i].nRatingsDefined = values_defined;
								for (j = 0; j < values_defined; j++)
								{
									int abbrev_rating_value_length = get_bits(BM_PARSER_THREAD, 8);
									memset(v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szAbbreviatedRating, 
										   0,
										   sizeof(v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szAbbreviatedRating));
									GetATSCMultipleString(BM_PARSER_THREAD, v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szAbbreviatedRating, abbrev_rating_value_length);
									{
										int rating_value_length = get_bits(BM_PARSER_THREAD, 8);
										memset(v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szRating,
											   0,
											   sizeof(v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szRating));
										GetATSCMultipleString(BM_PARSER_THREAD, v->prrt[rating_region]->rrtdimension[i].rrtrating[j].szRating, rating_value_length);
									}
								}
							}
						}
					}
					PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_RRT, rating_region);					
				}
			}
			break;
		case 0xcd:		// STT
			{
				DWORD system_time = pSectionPointer[9] << 24 | pSectionPointer[10] << 16 | pSectionPointer[11] << 8 | pSectionPointer[12];
				int GPS_UTC_offset = pSectionPointer[13];
				int daylight_savings = pSectionPointer[14] << 8 | pSectionPointer[15];

				if (v->nNetworkPID != 0x0010)
				{
					SYSTEMTIME st;

					system_time -= GPS_UTC_offset;

					ConvertATSCDateTime(system_time, &st);
					v->dvbtdt.nYear = st.wYear;
					v->dvbtdt.nMonth = st.wMonth;
					v->dvbtdt.nDay = st.wDay;
					v->dvbtdt.nHour = st.wHour;
					v->dvbtdt.nMinute = st.wMinute;
					v->dvbtdt.nSecond = st.wSecond;
					v->dvbtdt.nDaylightSavings = daylight_savings;
					v->dvbtdt.nGPSOffset = GPS_UTC_offset;
					v->dvbtdt.fSTTSeen = TRUE;
					SetEPGStreamTime(st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
					PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_TDT, 0);
				}
			}
			break;
		default:
	//#ifdef DEBUG_MESSAGES
			{
				char szTemp[128];
				wsprintf(szTemp, "TSReader: ATSC PSIP table 0x%02x\n", nTableID);
				OutputDebugString(szTemp);
			}
	//#endif DEBUG_MESSAGES
			break;
		}
	}

	return TRUE;

}

BOOL ParseCATPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nTableID, nSectionLength;
	int nSectionNumber, nLastSectionNumber;
	int nVersionNumber;

	nTableID = pSectionPointer[0];
	if (nTableID != 1)
	{
#ifdef PRO
		v->nSIParserTableErrors[SI_PARSER_STATS_CAT]++;
#endif PRO
		return FALSE;
	}

	nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
	if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
		return FALSE;

	if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
	{
		v->nSIParserCRCs[SI_PARSER_STATS_CAT]++;
		if (!v->fIgnoreTableCRCErrors)
			return FALSE;
	}

	if (v->fDidCAT == TRUE)
		return FALSE;

	nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
	nSectionNumber = pSectionPointer[6];
	nLastSectionNumber = pSectionPointer[7];

	nSectionLength -= 5 + 4; // 5 bytes preceed length and 4 bytes are CRC
	pSectionPointer += 8;

	v->cat.nVersionNumber = nVersionNumber;
	while (nSectionLength > 0)
	{
		// Get data from each descriptor
		int nDescriptorLength, nDescriptorTag;
		int i;

		nDescriptorTag = pSectionPointer[0];
		nDescriptorLength = pSectionPointer[1] + 2;
		LogDescriptor(DESCRIPTOR_CAT, nDescriptorTag);

		for (i = 0; i < MAX_CAT_DESCRIPTORS; i++)
		{
			if (v->cat.pDescriptor[i] == NULL)
			{
				v->cat.pDescriptor[i] = LocalAlloc(LPTR, nDescriptorLength + 4);
				memcpy(v->cat.pDescriptor[i], pSectionPointer, nDescriptorLength);
				if (*pSectionPointer == 9)	// CA descriptor
				{
					int nEMMPID = (*(pSectionPointer + 4) << 8 | *(pSectionPointer + 5) ) & 0x1fff;
					PIDManagement(TRUE, nEMMPID, FALSE);					
				}
				break;
			}
		}
		nSectionLength -= nDescriptorLength;
		pSectionPointer += nDescriptorLength;
	};

	return TRUE;
	//return (nSectionNumber == nLastSectionNumber);
}

#define DCII_CABLE 0
#define DCII_SATELLITE 1
#define DCII_MMDS 2
#define DCII_SMATV 3
#define DCII_OVER_THE_AIR 4

void ParseDCIINetworkPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nSectionLength, nTableID;

	do
	{
		// Check for stuffing
		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;

		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
			return;
		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_NIT]++;
			if (!v->fIgnoreTableCRCErrors)
				return;
		}

		// Get data from each section header		
		nTableID = pSectionPointer[0];
		switch(nTableID)
		{
		case 0xc2:		// Network Information
			{
				int number_of_records = pSectionPointer[5];
				int transmission_medium = pSectionPointer[6] >> 4;
				int table_type = pSectionPointer[6] & 0x0f;
				int satellite_ID;
				int i;
				
				pSectionPointer += 7;		// point to satellite ID or the record
				nPacketLength -= 7;

				if (table_type == 4) // TDT special
				{
					satellite_ID = pSectionPointer[0];
					pSectionPointer++;
					nPacketLength--;
				}

				for (i = 0; i < number_of_records; i++)
				{
					int j;
					int descriptors_count;

					switch(table_type)
					{
					case 1:				// CDT
						{
							v->nMaxCDT = number_of_records;
							v->cdt[i].number_of_carriers = pSectionPointer[0];
							v->cdt[i].spacing_unit = pSectionPointer[1] & 0x80;
							v->cdt[i].frequency_spacing = (pSectionPointer[1] << 8 | pSectionPointer[2]) & 0x3fff;
							v->cdt[i].frequency_unit = pSectionPointer[3];
							v->cdt[i].first_carrier_frequency = (pSectionPointer[3] << 8 | pSectionPointer[4]) & 0x7fff;
							pSectionPointer += 5;
							nPacketLength -= 5;
						}
						break;
					case 2:				// MMT
						{
							v->nMaxMMT = number_of_records;
							v->mmt[i].transmission_system = pSectionPointer[0] >> 4;
							v->mmt[i].inner_coding_mode = pSectionPointer[0] & 0x0f;
							v->mmt[i].split_bitstream_mode = pSectionPointer[1] & 0x80;
							v->mmt[i].modulation_format = pSectionPointer[1] & 0x1f;
							v->mmt[i].symbol_rate = (pSectionPointer[2] << 24 | pSectionPointer[3] << 16 | pSectionPointer[4] << 8 | pSectionPointer[5]) & 0x0fffffff;
							pSectionPointer += 6;
							nPacketLength -= 6;
						}
						break;
					case 3:				// SIT
						{
							int j;
							int satellite_ID = pSectionPointer[0];
							BOOL fDontAdd = FALSE;

							for (j = 0; j < MAX_SIT_ENTRIES; j++)
							{
								if (v->sit[j].satellite_ID == satellite_ID)
								{
									fDontAdd = TRUE;
									break;
								}
								if (v->sit[j].satellite_ID == 0)
									break;
							}
							if (fDontAdd == TRUE)
								break;
							if (j == MAX_SIT_ENTRIES)
							{
#ifdef DEBUG_MESSAGES
								OutputDebugString("SIT overflow!!\n");
#endif DEBUG_MESSAGES
								break;
							}
							v->sit[j].satellite_ID = satellite_ID;
							v->sit[j].you_are_here = pSectionPointer[1] & 0x80;
							v->sit[j].frequency_band = (pSectionPointer[1] >> 5) & 0x03;
							v->sit[j].out_of_service = pSectionPointer[1] & 0x10;
							v->sit[j].hemisphere = pSectionPointer[1] & 0x08;
							v->sit[j].orbital_position = (pSectionPointer[1] << 8 | pSectionPointer[2]) & 0x7ff;
							v->sit[j].polarization_type = pSectionPointer[3] & 0x80;
							v->sit[j].number_of_transponders = pSectionPointer[3] & 0x3f;
							
							PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_SIT, j);
							
							pSectionPointer += 4;
							nPacketLength -= 4;
						}
						break;
					case 4:				// TDT
						{
							int j;
							BOOL fDontAdd = FALSE;
							int transponder_number = pSectionPointer[0] & 0x3f;
							
							if (transponder_number == 0)
								break;

							for (j = 0; j < MAX_TDT_ENTRIES; j++)
							{
								if ( (v->tdt[j].satellite_ID == satellite_ID) && (v->tdt[j].transponder_number == transponder_number) )
								{
									// already got this one
									fDontAdd = TRUE;
									break;
								}
								if (v->tdt[j].satellite_ID == 0)
									break;
							}
							if (fDontAdd == TRUE)
								break;
							if (j == MAX_TDT_ENTRIES)
							{
#ifdef DEBUG_MESSAGES
								OutputDebugString("TDT overflow!!\n");
#endif DEBUG_MESSAGES
								break;
							}
							
							memset(&v->tdt[j], 0, sizeof(v->tdt[j]));
							v->tdt[j].satellite_ID = satellite_ID;
							v->tdt[j].transport_type = pSectionPointer[0] & 0x80;
							v->tdt[j].polarization = pSectionPointer[0] & 0x40;
							v->tdt[j].transponder_number = transponder_number;
							v->tdt[j].CDT_reference = pSectionPointer[1];
							
							pSectionPointer += 2;
							nPacketLength -= 2;
							if (!v->tdt[j].transport_type)
							{
								// MPEG-2 transport
								v->tdt[j].MMT_reference = pSectionPointer[0];
								v->tdt[j].VCT_ID = pSectionPointer[1] << 8 | pSectionPointer[2];
								v->tdt[j].root_transponder = pSectionPointer[3] & 0x80;
							}
							else
							{
								// non-MPEG-2 transport
								v->tdt[j].wide_bandwidth_video = pSectionPointer[0] & 0x80;
								v->tdt[j].waveform_standard = pSectionPointer[0] & 0x1f;
								v->tdt[j].wide_bandwidth_audio = pSectionPointer[1] & 0x80;
								v->tdt[j].companded_audio = pSectionPointer[1] & 0x40;
								v->tdt[j].matrix_mode = (pSectionPointer[1] & 0x30) >> 4;
								v->tdt[j].subcarrier_2_offset = ((pSectionPointer[1] << 16 | pSectionPointer[2] << 8 | pSectionPointer[3]) & 0x7ffff) >> 10;
								v->tdt[j].subcarrier_1_offset = (pSectionPointer[1] << 16 | pSectionPointer[2] << 8 | pSectionPointer[3]) & 0x3ff;
							}
							PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_TDT, j);

							pSectionPointer += 4;
							nPacketLength -= 4;
						}
						break;
					}
					descriptors_count = pSectionPointer[0];
					pSectionPointer++;
					nPacketLength--;
					for (j = 0; j < descriptors_count; j++)
					{
						int nDescriptorTag = pSectionPointer[0];
						int nDescriptorLength = pSectionPointer[1] + 2;
						LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

						pSectionPointer += nDescriptorLength;
						nPacketLength -= nDescriptorLength;					
					}
				}

				switch(table_type)
				{
				case 1:		// CDT
					if (v->hCDTRootTreeItem == NULL)
						PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_CDT, 0);
					break;
				case 2:		// MMT
					if (v->hMMTRootTreeItem == NULL)
						PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_MMT, 0);
					break;
				}
			}
			break;
		case 0xc3:		// Network Text
			{
				int table_type = pSectionPointer[7] & 0x0f;
				int transmission_medium = (pSectionPointer[7] >> 4) & 0x0f;

				pSectionPointer += 8;
				nPacketLength -= 8;

				switch(table_type)
				{
				case 1:		// TNT - Transponder Name Table
					{
						int satellite_ID = pSectionPointer[0];
						int first_index = pSectionPointer[1];
						int number_of_TNT_records = pSectionPointer[2];
						int j;

						pSectionPointer += 3;
						nPacketLength -= 3;
						for (j = 0; j < number_of_TNT_records; j++)
						{
							int transponder_number = pSectionPointer[0] & 0x3f;
							int transponder_name_length = pSectionPointer[1] & 0x1f;
							int TNT_descriptors_count;
							int k;
							char transponder_name[100]; // this might be wrong. seems to be variable.
							
							pSectionPointer += 2;
							nPacketLength -= 2;
							if (transponder_name_length)
							{
								memcpy(transponder_name, pSectionPointer, transponder_name_length);
								transponder_name[transponder_name_length] = 0;
								pSectionPointer += transponder_name_length;
								nPacketLength -= transponder_name_length;
							}

							TNT_descriptors_count = pSectionPointer[0];
							pSectionPointer += 1;
							nPacketLength -= 1;
							for (k = 0; k < TNT_descriptors_count; k++)
							{
								int nDescriptorTag = pSectionPointer[0];
								int nDescriptorLength = pSectionPointer[1] + 2;
								LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

								pSectionPointer += nDescriptorLength;
								nPacketLength -= nDescriptorLength;					
							}
						}
					}
					break;
				case 2:		// STT - Satellite Text Table
					{
						int first_index = pSectionPointer[0];
						int number_of_STT_records = pSectionPointer[1];
						int j;

						pSectionPointer += 2;
						nPacketLength -= 2;
						for (j = 0; j < number_of_STT_records; j++)
						{
							int satellite_ID = pSectionPointer[0];
							int sat_reference_name_length = pSectionPointer[1] & 0x0f;
							int full_satellite_name_length;
							int STT_descriptors_count;
							int k;
							char sat_reference_name[100];
							char full_satellite_name[100];

							pSectionPointer += 2;
							nPacketLength -= 2;
							if (sat_reference_name_length)
							{
								memcpy(sat_reference_name, pSectionPointer, sat_reference_name_length);
								sat_reference_name[sat_reference_name_length] = 0;
								pSectionPointer += sat_reference_name_length;
								nPacketLength -= sat_reference_name_length;
							}
							full_satellite_name_length = pSectionPointer[0] & 0x1f;
							pSectionPointer += 1;
							nPacketLength -= 1;
							if (full_satellite_name_length)
							{
								memcpy(full_satellite_name, pSectionPointer, full_satellite_name_length);
								full_satellite_name[full_satellite_name_length] = 0;
								pSectionPointer += full_satellite_name_length;
								nPacketLength -= full_satellite_name_length;
							}
							STT_descriptors_count = pSectionPointer[0];
							pSectionPointer += 1; nPacketLength -= 1;
							for (k = 0; k < STT_descriptors_count; k++)
							{
								int nDescriptorTag = pSectionPointer[0];
								int nDescriptorLength = pSectionPointer[1] + 2;
								LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

								pSectionPointer += nDescriptorLength;
								nPacketLength -= nDescriptorLength;					
							}
						}
					}
					break;
				case 3:		// RTT - Ratings Text Table
					{
						int a=1;
					}
					break;
				case 4:		// RST - Rating System Table
					{
						int a=1;
					}
					break;
				case 5:		// SNT - Source Name Table
					{
						int number_of_SNT_records = pSectionPointer[0];
						int j;
						
						pSectionPointer += 1; nPacketLength -= 1;
						for (j = 0; j < number_of_SNT_records; j++)
						{
							int application_type = pSectionPointer[0] & 0x80;
							int application_ID;
							int source_ID;
							int name_length;
							int SNT_descriptors_count;
							int k;
							char source_name[100];

							if (application_type)
								application_ID = pSectionPointer[1] << 8 | pSectionPointer[2];
							else
								source_ID = pSectionPointer[1] << 8 | pSectionPointer[2];
							name_length = pSectionPointer[3];
							pSectionPointer += 4; nPacketLength -= 4;
							memcpy(source_name, pSectionPointer, name_length);
							source_name[name_length] = 0;
							pSectionPointer += name_length; nPacketLength -= name_length;
							SNT_descriptors_count = pSectionPointer[0];
							pSectionPointer += 1; nPacketLength -= 1;
							for (k = 0; k < SNT_descriptors_count; k++)
							{
								int nDescriptorTag = pSectionPointer[0];
								int nDescriptorLength = pSectionPointer[1] + 2;
								LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

								pSectionPointer += nDescriptorLength;
								nPacketLength -= nDescriptorLength;					
							}
						}
					}
					break;
				case 6:		// MNT - Map Name Table
					{
						int a=1;
					}
					break;
				}
			}
			break;
		case 0x92:		// ??
			break;
		case 0x94:		// ??
			break;
		case 0x9a:		// ??
			break;
		case 0xc4:		// Virtual Channel
			set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
			{
				int message_type_version;

				int message_type = get_bits(BM_PARSER_THREAD, 8);
				int MPEG_table_format = get_bits(BM_PARSER_THREAD, 1);
				int multicast16_addressed = get_bits(BM_PARSER_THREAD, 1);
				int ISO_reserved1 = get_bits(BM_PARSER_THREAD, 2);
				int message_length = get_bits(BM_PARSER_THREAD, 12);
				if (multicast16_addressed)
				{
					int multicast16_address = get_bits(BM_PARSER_THREAD, 16);
				}
				if (message_type >= 0x40)
				{
					int frames_extension_flag = get_bits(BM_PARSER_THREAD, 1);
					int segmentation_overlay_included = get_bits(BM_PARSER_THREAD, 1);
					int message_preamble_included = get_bits(BM_PARSER_THREAD, 1);
					message_type_version = get_bits(BM_PARSER_THREAD, 5);
					if (message_type_version == 0)
					{
						int table_extension = get_bits(BM_PARSER_THREAD, 16);
					}
					else
					{
						int table_version = get_bits(BM_PARSER_THREAD, 6);
						int originator_ID = get_bits(BM_PARSER_THREAD, 10);
						int table_instance = get_bits(BM_PARSER_THREAD, 16);
					}
					{
						int last_segment_number = get_bits(BM_PARSER_THREAD, 12);
						int segment_number = get_bits(BM_PARSER_THREAD, 12);
					}
					if (message_preamble_included)
					{
						int message_preamble_length = get_bits(BM_PARSER_THREAD, 8);
						int i;
						for (i = 0; i < message_preamble_length; i++)
							get_bits(BM_PARSER_THREAD, 8);
					}
				}
				{
					int transmission_medium = get_bits(BM_PARSER_THREAD, 4);
					int table_subtype = get_bits(BM_PARSER_THREAD, 4);
					int VCT_ID = get_bits(BM_PARSER_THREAD, 8);
					switch(table_subtype)
					{
					case 0:		// VCT
						{
							int number_of_VC_records;
							int i;

							int reserved1 = get_bits(BM_PARSER_THREAD, 2);
							int descriptors_included = get_bits(BM_PARSER_THREAD, 1);
							int reserved2 = get_bits(BM_PARSER_THREAD, 5);
							int splice = get_bits(BM_PARSER_THREAD, 1);
							int reserved3 = get_bits(BM_PARSER_THREAD, 7);
							int activation_time = get_bits(BM_PARSER_THREAD, 32);
							if (message_type_version == 0)
								number_of_VC_records = get_bits(BM_PARSER_THREAD, 8);
							else
								number_of_VC_records = get_bits(BM_PARSER_THREAD, 16);
							for (i = 0; i < number_of_VC_records; i++)
							{
								if (transmission_medium == DCII_SATELLITE)
								{
									// satellite virtual channel
									int a=1;
								}
								else if (transmission_medium == DCII_SMATV)
								{
									// SMATV_virtual_channel
								}
								else if (transmission_medium == DCII_OVER_THE_AIR)
								{
									// broadcast_virtual_channel
								}
								else
								{
									int reserved1 = get_bits(BM_PARSER_THREAD, 3);
									int prefered_source = get_bits(BM_PARSER_THREAD, 1);
									int virtual_channel_number = get_bits(BM_PARSER_THREAD, 12);
									int application_virtual_channel = get_bits(BM_PARSER_THREAD, 1);
									int bitstream_select = get_bits(BM_PARSER_THREAD, 1);
									int path_select = get_bits(BM_PARSER_THREAD, 1);
									int transport_type = get_bits(BM_PARSER_THREAD, 1);
									int channel_type = get_bits(BM_PARSER_THREAD, 4);

									// virtual_channel
								}
							}
						}
						break;
					case 1:		// DCM
						break;
					case 2:		// ICT
						break;
					}
				}
			}
			break;
/*
		case 0xc5:		// System Time
			{
				int a=1;
			}
			break;*/
		default:
			/*{
				char szTemp[100];
				wsprintf(szTemp, "DCII Network table_id = %02x\n", nTableID);
				OutputDebugString(szTemp);
			}*/
			break;
		}
		
		pSectionPointer += 4; // skip CRC
	} while (nPacketLength > 20);

}

void ParseDVBRSTPacket(BYTE * pSectionPointer, int nPacketLength)
{
	set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
	{
		int table_id = get_bits(BM_PARSER_THREAD, 8);
		int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
		int reserved_future_use1 = get_bits(BM_PARSER_THREAD, 1);
		int reserved = get_bits(BM_PARSER_THREAD, 2);
		int section_length = get_bits(BM_PARSER_THREAD, 12);
		if (table_id != 0x71 || table_id != 0x72)
		{
#ifdef PRO
			v->nSIParserTableErrors[SI_PARSER_STATS_RST]++;
#endif PRO
			return;
		}
		// todo -- handle stuffing section

		while (section_length)
		{
			PEITEVENT pCurrent, pNext;
			int transport_stream_id = get_bits(BM_PARSER_THREAD, 16);
			int original_network_id = get_bits(BM_PARSER_THREAD, 16);
			int service_id = get_bits(BM_PARSER_THREAD, 16);
			int event_id = get_bits(BM_PARSER_THREAD, 16);
			int reserved_future_use2 = get_bits(BM_PARSER_THREAD, 5);
			int running_status = get_bits(BM_PARSER_THREAD, 3);
			section_length -= 9;

			// See about updating running status if we can find the event
			pCurrent = v->pEvents[service_id];
			if (pCurrent != NULL)
			{
				do
				{
					pNext = (PEITEVENT)pCurrent->dwNextEvent;
					if (pCurrent->nEventID == event_id)
					{
						pCurrent->nRunningStatus = running_status;
						break;
					}
					pCurrent = pNext;
				} while (pCurrent != NULL);
			}		
		}
	}
}


void ParseISDBBITPacket(BYTE * pSectionPointer, int nPacketLength)
{
	BOOL fPriorISDBState;
	int nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
	
	// Check length, CRC and table_id
	if ( (nSectionLength <= 0) || (nSectionLength > 65536) )
			return;
	if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		return;
	if (pSectionPointer[0] != 0xc4)
		return;

	fPriorISDBState = v->fISDB;
	v->fISDB = TRUE;
	if (fPriorISDBState == FALSE)
	{
		int nPMTIndex;

		for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
		{
			int nESIndex;
			BOOL fNeedToUpdateEntry = FALSE;

			if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
				break;
			for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
			{
				if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
					break;
				if (v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType == 0x0f)
				{
					fNeedToUpdateEntry = TRUE;
					break;
				}
			}
			if (fNeedToUpdateEntry)
			{
				if (v->pat.pmt[nPMTIndex].fCompleted)
				{
					for (nESIndex = 0; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
					{
						if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
							continue;
						if (v->pat.pmt[nPMTIndex].es[nESIndex].hESTreeItem != NULL)
							SendMessage(v->hDlgSIParser, WM_USER + 6, 0, (LPARAM)v->pat.pmt[nPMTIndex].es[nESIndex].hESTreeItem);	// removes the ES tree item
						EnterCriticalSection(&v->csThumbnails);
						if (v->pat.pmt[nPMTIndex].es[nESIndex].nDescriptorsLength)
							LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pDescriptors);
						if (v->pat.pmt[nPMTIndex].es[nESIndex].pParsedData != NULL)
							LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pParsedData);
						if (v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame != NULL)
							LocalFree(v->pat.pmt[nPMTIndex].es[nESIndex].pRGBVideoFrame);
						memset(&v->pat.pmt[nPMTIndex].es[nESIndex], 0, sizeof(v->pat.pmt[nPMTIndex].es[nESIndex]));
						LeaveCriticalSection(&v->csThumbnails);
					}
					if (v->pat.pmt[nPMTIndex].hPCRTreeItem != NULL)
					{
						SendMessage(v->hDlgSIParser, WM_USER + 6, 0, (LPARAM)v->pat.pmt[nPMTIndex].hPCRTreeItem);	// removes the ES tree item
						v->pat.pmt[nPMTIndex].hPCRTreeItem = NULL;
					}
					v->pat.pmt[nPMTIndex].fCompleted = FALSE;
				}
			}
		}
	}
}

void ParseDVBTDTPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nSectionLength, nTableID;

	do
	{
		// Check for stuffing
		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;

		// Get data from each section header		
		nTableID = pSectionPointer[0];
		if (nTableID == 0)
			break;

#ifdef PRO
		if (nTableID == 0x70)
		{
			double dCurrentTime = GetStreamMonitorTime();

			if (v->dSIParserTableTime[SI_PARSER_STATS_TDT] == 0.0)
				v->dSIParserTableTime[SI_PARSER_STATS_TDT] = dCurrentTime;
			if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_TDT] + 30.0)
			{
				v->nSIParserTimingErrors[SI_PARSER_STATS_TDT]++;			
			}
			v->dSIParserTableTime[SI_PARSER_STATS_TDT] = dCurrentTime;
		}
#endif PRO

		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
			return;
		switch(nTableID)
		{
		case 0x70:			// TDT
			{
				int nMJD, nTime;

				nMJD = pSectionPointer[3] << 8 | pSectionPointer[4];
				nTime = pSectionPointer[5] << 16 | pSectionPointer[6] << 8 | pSectionPointer[7];
				ConvertDVBDate(nMJD, &v->dvbtdt.nYear, &v->dvbtdt.nMonth, &v->dvbtdt.nDay);
				ConvertDVBTime(nTime, &v->dvbtdt.nHour, &v->dvbtdt.nMinute, &v->dvbtdt.nSecond);			
				SetEPGStreamTime(v->dvbtdt.nYear, v->dvbtdt.nMonth, v->dvbtdt.nDay,
								 v->dvbtdt.nHour, v->dvbtdt.nMinute, v->dvbtdt.nSecond);
				PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_TDT, 0);
				pSectionPointer += 8; nPacketLength -= 8;
			}
			break;
		case 0x72:			// ST
			nPacketLength -= 3;		// table id and section length
			nPacketLength -= nSectionLength;
			pSectionPointer = &pSectionPointer[3] + nSectionLength;
			break;
		case 0x73:			// TOT
			if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
			{
				v->nSIParserCRCs[SI_PARSER_STATS_TDT]++;
				if (!v->fIgnoreTableCRCErrors)
					return;
			}						
			set_buf(BM_PARSER_THREAD, pSectionPointer, 0, FALSE);
			{
				int table_id = get_bits(BM_PARSER_THREAD, 8);
				int section_syntax_indicator = get_bits(BM_PARSER_THREAD, 1);
				int reserved_future_use = get_bits(BM_PARSER_THREAD, 1);
				int reserved1 = get_bits(BM_PARSER_THREAD, 2);
				int section_length = get_bits(BM_PARSER_THREAD, 12);
				int nMJD = get_bits(BM_PARSER_THREAD, 16);
				int nUTC = get_bits(BM_PARSER_THREAD, 24);
				int reserved2 = get_bits(BM_PARSER_THREAD, 4);
				int descriptor_loop_length = get_bits(BM_PARSER_THREAD, 12);

				ConvertDVBDate(nMJD, &v->dvbtot.nYear, &v->dvbtot.nMonth, &v->dvbtot.nDay);
				ConvertDVBTime(nUTC, &v->dvbtot.nHour, &v->dvbtot.nMinute, &v->dvbtot.nSecond);			
				SetEPGStreamTime(v->dvbtot.nYear, v->dvbtot.nMonth, v->dvbtot.nDay,
								 v->dvbtot.nHour, v->dvbtot.nMinute, v->dvbtot.nSecond);

				if (descriptor_loop_length)
				{
					if (v->dvbtot.pDescriptors == NULL)
					{
						int i;

						v->dvbtot.pDescriptors = LocalAlloc(LPTR, descriptor_loop_length + 4);
						v->dvbtot.nDescriptorsLength = descriptor_loop_length;
						for (i = 0; i < descriptor_loop_length; i++)
							v->dvbtot.pDescriptors[i] = get_bits(BM_PARSER_THREAD, 8);
						i = 0;
						while (i < descriptor_loop_length)
						{
							LogDescriptor(DESCRIPTOR_TOT, v->dvbtot.pDescriptors[i + 0]);
							i += v->dvbtot.pDescriptors[i + 1] + 2;
						}
					}
				}

				PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_TDT, 1);

				nPacketLength -= 10; // minus size of header
				pSectionPointer += 10;
				nPacketLength -= descriptor_loop_length; // minus descriptors
				pSectionPointer += descriptor_loop_length;
				pSectionPointer += 4; // skip CRC
				nPacketLength -= 4;
			}
			break;
		default:
#ifdef PRO
			v->nSIParserTableErrors[SI_PARSER_STATS_TDT]++;
#endif PRO
			return;		// dunno how to handle
		}
	} while (nPacketLength);
}
	
void ParseDVBNITPacket(BYTE * pSectionPointer, int nPacketLength)
{
	int nSectionLength, nTableID;
	int nDescriptorsLoopLength;
	int nTransportStreamLoopLength;
	int nNetworkID;
	int nNetworkDescriptorsLength;
	int nVersionNumber;
	BYTE * pNetworkDescriptorPtr;
	char szNetworkName[256];

	memset(szNetworkName, 0, sizeof(szNetworkName));
	do
	{
		// Check for stuffing
		do
		{
			if (*pSectionPointer != 0xff)
				break;
			pSectionPointer++;
			nPacketLength--;
		} while (nPacketLength > 0);
		if (nPacketLength == 0)
			break;

		nSectionLength = ((pSectionPointer[1] << 8) + pSectionPointer[2]) & 0xfff;
		if ( (nSectionLength <= 0) || (nSectionLength > 1024) )
			return;
		nTableID = pSectionPointer[0];
		if (nTableID == 0x72)
		{
			// stuffing - ignore it
			return;
		}
		if (SourceHelper_CRC_Check(pSectionPointer, nSectionLength + 3) != TRUE)
		{
			v->nSIParserCRCs[SI_PARSER_STATS_NIT]++;
			if (!v->fIgnoreTableCRCErrors)
				return;
		}

		// Get data from each section header		
		nNetworkID = (pSectionPointer[3] << 8) + pSectionPointer[4];
		if (!(nTableID == 0x40 || nTableID == 0x41 || nTableID == 0x72))
		{
#ifdef PRO
			v->nSIParserTableErrors[SI_PARSER_STATS_NIT]++;
#endif PRO
			break;
		}

		// Check for stuffing section
		if (nTableID == 0x72)
		{
			nPacketLength -= 3;		// table id and section length
			nPacketLength -= nSectionLength;
			pSectionPointer = &pSectionPointer[3] + nSectionLength;
			continue;
		}

#ifdef PRO
		{
			double dCurrentTime = GetStreamMonitorTime();

			if (v->dSIParserTableTime[SI_PARSER_STATS_NIT] == 0.0)
				v->dSIParserTableTime[SI_PARSER_STATS_NIT] = dCurrentTime;
			if (dCurrentTime  > v->dSIParserTableTime[SI_PARSER_STATS_NIT] + 10.0)
			{
				v->nSIParserTimingErrors[SI_PARSER_STATS_NIT]++;			
			}
			v->dSIParserTableTime[SI_PARSER_STATS_NIT] = dCurrentTime;
		}
#endif PRO

		nVersionNumber = (pSectionPointer[5] >> 1) & 0x1f;
		if (v->nSIParserVersionNumbers[SI_PARSER_STATS_NIT] == -1)
			v->nSIParserVersionNumbers[SI_PARSER_STATS_NIT] = nVersionNumber;

		nSectionLength -= 8; // detract size of the section header
		nPacketLength -= 4; // plus table id and section length
		nPacketLength -= nSectionLength;
		pSectionPointer = &pSectionPointer[8]; // point to network descriptors length

		nDescriptorsLoopLength = ((pSectionPointer[0] << 8 ) | pSectionPointer[1]) & 0xfff;
		nSectionLength -= 5;
		pSectionPointer = &pSectionPointer[2];
		nNetworkDescriptorsLength = nDescriptorsLoopLength;
		pNetworkDescriptorPtr = pSectionPointer;
		if (nDescriptorsLoopLength > 0)
		{
			do
			{
				// Get data from each descriptor
				int nDescriptorLength, nDescriptorTag;

				nDescriptorTag = pSectionPointer[0];
				nDescriptorLength = pSectionPointer[1] + 2;
				LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

				switch(nDescriptorTag)
				{
				case 0x40: // network name descriptor
					{
						int k;
						char * pText;

						memset(szNetworkName, 0, sizeof(szNetworkName));
						pText = &pSectionPointer[2];
						for (k = 0; k < nDescriptorLength - 2; k++)
							szNetworkName[k] = *pText++;
						break;
					}
#ifdef PRO
				case 0x4a:
					set_buf(BM_PARSER_THREAD, &pSectionPointer[2], 0, FALSE);
					{
						int transport_stream_id = get_bits(BM_PARSER_THREAD, 16);
						int original_network_id = get_bits(BM_PARSER_THREAD, 16);
						int service_id = get_bits(BM_PARSER_THREAD, 16);
						int linkage_type = get_bits(BM_PARSER_THREAD, 8);
						if (linkage_type == 0x0b && v->nINTService == 0)
						{
							if (v->nPMTPID == v->nNullPID)
							{
								// PMT parsing has completed
								int nPMTIndex;

								for (nPMTIndex = 0; nPMTIndex < MAX_PAT_ENTRIES; nPMTIndex++)
								{
									int nESIndex;

									if (v->pat.pmt[nPMTIndex].nPMTPID == 0)
										break;
									if (v->pat.pmt[nPMTIndex].nProgramNumber == service_id)
									{
										for (nESIndex = 0 ; nESIndex < MAX_ESLIST_ENTRIES; nESIndex++)
										{
											if (v->pat.pmt[nPMTIndex].es[nESIndex].nESPID == 0)
												break;
											if (v->pat.pmt[nPMTIndex].es[nESIndex].nStreamType == 0x05)
											{
												v->nINTPID = v->pat.pmt[nPMTIndex].es[nESIndex].nESPID;
												v->nINTService = service_id;
											}
										}
									}							
								}
							}
						}
					}
					break;
#endif PRO
				case 0:
					break;
				}

				nSectionLength -= nDescriptorLength;
				pSectionPointer += nDescriptorLength;
				nDescriptorsLoopLength -= nDescriptorLength;
			} while (nDescriptorsLoopLength > 0);
		}

		do
		{
			nTransportStreamLoopLength = ((pSectionPointer[0] << 8 ) | pSectionPointer[1]) & 0xfff;
			if (nTransportStreamLoopLength > nSectionLength)
				break;
			nSectionLength -= 2;
			pSectionPointer = &pSectionPointer[2];
			if (nTransportStreamLoopLength > 0)
			{
				do
				{
					int nTransportStreamID = pSectionPointer[0] << 8 | pSectionPointer[1];
					int nOriginalNetworkID = pSectionPointer[2] << 8 | pSectionPointer[3];
					int nTransportDescriptorsLength = (pSectionPointer[4] << 8 | pSectionPointer[5]) & 0xfff;
					int nNITIndex;
					BOOL fAddThisOne = FALSE;

					for (nNITIndex = 0; nNITIndex < MAX_TS_ENTRIES; nNITIndex++)
					{
						if (v->pNITData[nNITIndex] != NULL)
						{
							if (    (v->pNITData[nNITIndex]->nTransportStreamID == nTransportStreamID)
								 && (v->pNITData[nNITIndex]->nNetworkID == nNetworkID) )
							{
								int nExtraDescriptorIndex;

								// Already got this one
								if (v->pNITData[nNITIndex]->nVersionNumber == nVersionNumber)
									break;		// already got this one

								// Remove the old NIT entry so we can reallocate this new one
								SendMessage(v->hDlgSIParser, WM_USER + 6, 0, (LPARAM)v->pNITData[nNITIndex]->hNITTreeItem);	// removes the NIT tree item
								if (v->pNITData[nNITIndex]->nNetworkDescriptorsLength)
									LocalFree(v->pNITData[nNITIndex]->pNetworkDescriptors);
								for (nExtraDescriptorIndex = 0; nExtraDescriptorIndex < MAX_EIT_EXTRA_DESCRIPTORS; nExtraDescriptorIndex++)
								{
									if (v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex] != NULL)
										LocalFree(v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex]);
								}
								LocalFree(v->pNITData[nNITIndex]);
								v->pNITData[nNITIndex] = NULL;
							}
						}

						if (v->pNITData[nNITIndex] == NULL)
						{
							v->pNITData[nNITIndex] = LocalAlloc(LPTR, sizeof(NITENTRY) + 4);
							v->pNITData[nNITIndex]->nTransportStreamID = nTransportStreamID;
							v->pNITData[nNITIndex]->nOriginalNetworkID = nOriginalNetworkID;
							v->pNITData[nNITIndex]->nNetworkID = nNetworkID;
							v->pNITData[nNITIndex]->nVersionNumber = nVersionNumber;
							lstrcpy(v->pNITData[nNITIndex]->szNetworkName, szNetworkName);
							if (nTableID == 0x40)
								v->pNITData[nNITIndex]->fThisTS = TRUE;
							if (nNetworkDescriptorsLength)
							{
								v->pNITData[nNITIndex]->nNetworkDescriptorsLength = nNetworkDescriptorsLength;
								v->pNITData[nNITIndex]->pNetworkDescriptors = LocalAlloc(LPTR, nNetworkDescriptorsLength + 4);
								memcpy(v->pNITData[nNITIndex]->pNetworkDescriptors, pNetworkDescriptorPtr, nNetworkDescriptorsLength);
							}
							fAddThisOne = TRUE;
							break;
						}
					}

					pSectionPointer = &pSectionPointer[6];
					nTransportStreamLoopLength -= 6;
					if (nTransportDescriptorsLength > 0)
					{
						do
						{
							// Get data from each descriptor
							int nDescriptorLength, nDescriptorTag;

							nDescriptorTag = pSectionPointer[0];
							nDescriptorLength = pSectionPointer[1] + 2;
							LogDescriptor(DESCRIPTOR_NIT, nDescriptorTag);

							if (fAddThisOne == TRUE)
							{
								switch(nDescriptorTag)
								{
								case 0x43: // satellite delivery system descriptor
									v->pNITData[nNITIndex]->nType = NIT_DVBS;
									v->pNITData[nNITIndex]->nFrequency = ConvertBCD(pSectionPointer[2] << 24 | pSectionPointer[3] << 16 | pSectionPointer[4] << 8 | pSectionPointer[5]);
									v->pNITData[nNITIndex]->dvbs.nFEC = pSectionPointer[12] & 0x0f;
									v->pNITData[nNITIndex]->dvbs.nOrbitalPosition = ConvertBCD(pSectionPointer[6]  << 8 | pSectionPointer[7]);
									v->pNITData[nNITIndex]->dvbs.fEastern = (pSectionPointer[8] >> 7) & 1;
									v->pNITData[nNITIndex]->dvbs.nPolarization = (pSectionPointer[8] >> 5) & 3;
									v->pNITData[nNITIndex]->dvbs.nModulation = pSectionPointer[8] & 0x1f;
									v->pNITData[nNITIndex]->dvbs.nSymbolRate = ConvertBCD((pSectionPointer[9] << 24 | pSectionPointer[10] << 16 | pSectionPointer[11] << 8 | pSectionPointer[12]) >> 4);
									goto ParseDVBNIT_AddDescriptor;
									break;
								case 0x44:	// cable delivery system descriptor
									v->pNITData[nNITIndex]->nType = NIT_DVBC;
									set_buf(BM_PARSER_THREAD, &pSectionPointer[2], 0, FALSE);
									{
										int frequency = get_bits(BM_PARSER_THREAD, 32);
										int reserved_future_use = get_bits(BM_PARSER_THREAD, 12);
										v->pNITData[nNITIndex]->nFrequency = ConvertBCD(frequency);
										v->pNITData[nNITIndex]->dvbc.nFECOuter = get_bits(BM_PARSER_THREAD, 4);
										v->pNITData[nNITIndex]->dvbc.nModulation = get_bits(BM_PARSER_THREAD, 8);
										v->pNITData[nNITIndex]->dvbc.nSymbolRate = ConvertBCD(get_bits(BM_PARSER_THREAD, 28));
										v->pNITData[nNITIndex]->dvbc.nFECInner = get_bits(BM_PARSER_THREAD, 4);
									}
									goto ParseDVBNIT_AddDescriptor;
									break;
								case 0x5a:	// terrestrial delivery system descriptor
									v->pNITData[nNITIndex]->nType = NIT_DVBT;
									v->pNITData[nNITIndex]->nFrequency = pSectionPointer[2] << 24 | pSectionPointer[3] << 16 | pSectionPointer[4] << 8 | pSectionPointer[5];
									v->pNITData[nNITIndex]->dvbt.nBandwidth = (pSectionPointer[6] >> 5) & 7;
									v->pNITData[nNITIndex]->dvbt.nConstellation = (pSectionPointer[7] >> 6) & 3;
									v->pNITData[nNITIndex]->dvbt.nHierarchyInformation = (pSectionPointer[7] >> 3) & 7;
									v->pNITData[nNITIndex]->dvbt.nCodeRateHPStream = pSectionPointer[7] & 7;
									v->pNITData[nNITIndex]->dvbt.nCodeRateLPStream = (pSectionPointer[8] >> 5) & 7;
									v->pNITData[nNITIndex]->dvbt.nGuardInterval = (pSectionPointer[8] >> 3) & 3;
									v->pNITData[nNITIndex]->dvbt.nTransmissionMode = (pSectionPointer[8] >> 1) & 3;
									v->pNITData[nNITIndex]->dvbt.nOtherFrequencyFlag = pSectionPointer[8] & 1;
									goto ParseDVBNIT_AddDescriptor;
									break;
								case 0x83:	// logical channel number
									{
										int i;

										set_buf(BM_PARSER_THREAD, &pSectionPointer[2], 0, FALSE);
										for (i = 0; i < nDescriptorLength; i += 4)
										{
											int service_id = get_bits(BM_PARSER_THREAD, 16);
											int visible_service_flag = get_bits(BM_PARSER_THREAD, 1);
											int reserved = get_bits(BM_PARSER_THREAD, 5);
											int logical_channel_number = get_bits(BM_PARSER_THREAD, 10);

											if (logical_channel_number)
											{
												if (v->pChannelData[service_id] != NULL)
												{
													if (v->pChannelData[service_id]->nLogicalChannelNumber == 0)
														v->pChannelData[service_id]->nLogicalChannelNumber = logical_channel_number;
												}
											}
										}
									}
									// Note!! Don't break - we want to save this descriptor
									// in the NIT so we can show it when they select the NIT
									//break;
ParseDVBNIT_AddDescriptor:
								default:
									{
										int nExtraDescriptorIndex;
										for (nExtraDescriptorIndex = 0; nExtraDescriptorIndex < MAX_EIT_EXTRA_DESCRIPTORS; nExtraDescriptorIndex++)
										{
											if (v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex] != NULL)
											{
												if (IsDuplicateDescriptor(v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex], pSectionPointer) == TRUE)
													break;
												//if (v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex][0] == nDescriptorTag)
												//	break;	// already got this descriptor
											}
											if (v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex] == NULL)
											{
												v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex] = LocalAlloc(LPTR, nDescriptorLength + 4);
												if (v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex] != NULL)
													memcpy(v->pNITData[nNITIndex]->pExtraDescriptors[nExtraDescriptorIndex], &pSectionPointer[0], nDescriptorLength);
												break;
											}
										}
									}
									break;
								}
							}

							nSectionLength -= nDescriptorLength;
							pSectionPointer += nDescriptorLength;
							nTransportDescriptorsLength -= nDescriptorLength;
							nTransportStreamLoopLength -= nDescriptorLength;
						} while (nTransportDescriptorsLength > 0);
					}
					if (fAddThisOne == TRUE)
						PostMessage(v->hDlgSIParser, WM_USER + 2, SI_PARSER_NIT, nNITIndex);
				} while (nTransportStreamLoopLength > 0);
			}
		} while (nSectionLength > 4);
		pSectionPointer += 4; // skip CRC
	} while (nPacketLength > 20);
}

void ParseDCIIECMPacket(BYTE * pSection, int nLength)
{
	/*{
		char szTemp[128];
		wsprintf(szTemp, "+ParseDCIIECMPacket program %d\n", v->pat.pmt[v->nDCIIECMPMTIndex].nProgramNumber);
		OutputDebugString(szTemp);
	}*/

	if (v->nDCIIECMDescriptorTimeout++ == 10)
	{
		// we've got no channel name for this program so try the next
		//OutputDebugString("DCII ECM parsing timeout\n");
		GetNextECMPID();
		return;
	}

	do
	{
		int nTableID = pSection[0];
		int nSectionLength = (pSection[1] << 8 | pSection[2]) & 0x0fff;
		if (nTableID == 0xff)		// stuffing
			break;
		if (nSectionLength > 1024)
			break;

		switch(nTableID)
		{
		case 0xc1:// program name message
			set_buf(BM_PARSER_THREAD, &pSection[3], 0, FALSE);
			{
				int unknown = get_bits(BM_PARSER_THREAD, 8);
				int ISO_639_language_code = get_bits(BM_PARSER_THREAD, 24);
				int service_number = get_bits(BM_PARSER_THREAD, 16);
				int reserved = get_bits(BM_PARSER_THREAD, 8);
				int sequence = get_bits(BM_PARSER_THREAD, 8);
				int program_epoch_number = get_bits(BM_PARSER_THREAD, 8);
				int program_name_control_byte = get_bits(BM_PARSER_THREAD, 8);
				int program_name_length = get_bits(BM_PARSER_THREAD, 8);
				if (program_name_length > 2 && program_name_length <= 80)
				{
					char szProgramName[128];
					int i;
					int junk = get_bits(BM_PARSER_THREAD, 16);
					for (i = 0; i < program_name_length - 2; i++)
						szProgramName[i] = get_bits(BM_PARSER_THREAD, 8);
					szProgramName[i] = '\0';
					if (lstrlen(szProgramName))
					{
						if (v->pat.pmt[v->nDCIIECMPMTIndex].nProgramNumber == service_number)
						{
							if (v->pChannelData[service_number] == NULL)
							{
								v->pChannelData[service_number] = LocalAlloc(LPTR, sizeof(EITCHANNELDATA) + 4);
								if (v->pChannelData[service_number] != NULL)
								{
									{
										char szTemp[512];
										wsprintf(szTemp, "TSReader: ParseDCIIECMPacket service %d adding string \"%s\"\n", service_number, szProgramName);
										OutputDebugString(szTemp);
									}
									lstrcpy(v->pChannelData[service_number]->szShortName, szProgramName);
									v->pChannelData[service_number]->nChannelNumber = service_number;
									v->pat.pmt[v->nDCIIECMPMTIndex].fSetupSDTName = TRUE;
								}
							}
							GetNextECMPID();
							//OutputDebugString("-ParseDCIIECMPacket 1\n");
							//return;
						}
					}
				}
			}
			break;
		/*case 0x40:
		//case 0x41:
			{
				int i;
				char szTemp[256] = {0};

				wsprintf(szTemp, "%d ", GetTickCount());

				for (i = 0; i < nSectionLength + 3; i++)
				{
					char szTemp2[8];
					wsprintf(szTemp2, "%02x ", pSection[i]);
					lstrcat(szTemp, szTemp2);
				}
				lstrcat(szTemp, "\n");
				OutputDebugString(szTemp);
			}
			break;*/
		}
		nLength -= nSectionLength + 3;
		pSection += nSectionLength + 3;
	} while (nLength > 4);
	//OutputDebugString("-ParseDCIIECMPacket 2\n");

}

void QuickParseUserData(BYTE * pData, int user_data_len, int nESParsePMTIndex, int nESParseESIndex, int nES)
{
	if (user_data_len)
		v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nTeletextServices |= VBI_SERVICE_USER;
	
	while (user_data_len > 4)
	{
		int i;
		DWORD identifier;
		char szTemp[256] = {0};

		set_buf(BM_MPEG2_THREAD + nES, (BYTE *)pData, 0, FALSE);
		identifier = get_bits(BM_MPEG2_THREAD + nES, 32);
		user_data_len -= 4;
		
		switch(identifier)
		{
			case 0x47413934:	// ATSC
			{
				BYTE user_data_type_code = get_bits(BM_MPEG2_THREAD + nES, 8);
				user_data_len -= 1;
				if (user_data_type_code == 0x03)
				{
					BOOL process_em_data_flag = get_bits(BM_MPEG2_THREAD + nES, 1);
					BOOL process_cc_data_flag = get_bits(BM_MPEG2_THREAD + nES, 1);
					BOOL additional_data_flag = get_bits(BM_MPEG2_THREAD + nES, 1);
					int cc_count = get_bits(BM_MPEG2_THREAD + nES, 5);
					int em_data = get_bits(BM_MPEG2_THREAD + nES, 8);
					user_data_len -= 2;
					for (i = 0; i < cc_count; i++ )
					{
						int marker_bits = get_bits(BM_MPEG2_THREAD + nES, 5);
						BOOL cc_valid = get_bits(BM_MPEG2_THREAD + nES, 1);
						int cc_type = get_bits(BM_MPEG2_THREAD + nES, 2);
						BYTE cc_data_1 = get_bits(BM_MPEG2_THREAD + nES, 8) & 0x7f;
						BYTE cc_data_2 = get_bits(BM_MPEG2_THREAD + nES, 8) & 0x7f;
						user_data_len -= 3;

						if (cc_valid)
						{
							switch(cc_type)
							{
							case 0:
							case 1:
								v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nTeletextServices |= VBI_SERVICE_CC;
								break;
							case 2:
							case 3:
								v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nTeletextServices |= VBI_SERVICE_DTVCC;
								break;	
							}
						}
					}
					{
						int marker_bits = get_bits(BM_MPEG2_THREAD + nES, 8);
						user_data_len -= 1;
					}
				}
			}
			break;
		case 0x44544731:		// AFD
			{
				v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].nTeletextServices |= VBI_SERVICE_AFD;
				{
					int zero = get_bits(BM_MPEG2_THREAD + nES, 1);
					int active_format_flag = get_bits(BM_MPEG2_THREAD + nES, 1);
					int reserved = get_bits(BM_MPEG2_THREAD + nES, 6);
					user_data_len -= 1;
					if (active_format_flag == 1)
					{
						int reserved = get_bits(BM_MPEG2_THREAD + nES, 4);
						int active_format = get_bits(BM_MPEG2_THREAD + nES, 4);
						user_data_len -= 1;
						v->pat.pmt[nESParsePMTIndex].es[nESParseESIndex].dwAFDData = active_format;
					}
				}
			}
			break;
		}
	}
}

void ParseDVBINTPacket(BYTE * pSection, int nLength)
{
	int nSectionLength = ((pSection[1] << 8) + pSection[2]) & 0xfff;
	
	// Check length, CRC and table_id
	if ( (nSectionLength <= 0) || (nSectionLength > 65536) )
			return;
	if (pSection[0] != 0x4c)
		return;
	if (SourceHelper_CRC_Check(pSection, nSectionLength + 3) != TRUE)
		return;

	set_buf(BM_DVBINT, pSection, 0, FALSE);
	{
		int table_id = get_bits(BM_DVBINT, 8);
		int section_syntax_indicator = get_bits(BM_DVBINT, 1);
		int reserved_for_future_use = get_bits(BM_DVBINT, 1);
		int reserved1 = get_bits(BM_DVBINT, 2);
		int section_length = get_bits(BM_DVBINT, 12);
		int action_type = get_bits(BM_DVBINT, 8);
		int platform_id_hash = get_bits(BM_DVBINT, 8);
		int reserved2 = get_bits(BM_DVBINT, 2);
		int version_number = get_bits(BM_DVBINT, 5);
		int current_next_indicator = get_bits(BM_DVBINT, 1);
		int section_number = get_bits(BM_DVBINT, 8);
		int last_section_number = get_bits(BM_DVBINT, 8);
		int platform_id = get_bits(BM_DVBINT, 24);
		int processing_order = get_bits(BM_DVBINT, 8);
	}
	
/*
platform_descriptor_loop()
for (i=0, i<N1, i++) {
target_descriptor_loop()
operational_descriptor_loop()
}
CRC_32 32 rpchof
}

  */
}
