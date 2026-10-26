// Table-driven descriptor builder
//
// Each descriptor is described by a list of fields (bit width, type, named values, and
// which flag a field depends on). One engine decodes a received descriptor into values
// and encodes values back into the descriptor, so the Descriptor Hex / Editor window can
// edit any defined descriptor in a form and build new ones from scratch. Reserved bits are
// kept as received, so decoding and re-encoding gives the same bytes.
//
// First round: audio, subtitling and video descriptors (EN 300 468, ISO/IEC 13818-1).

#include <windows.h>
#include <stdio.h>
#include "DescriptorBuilder.h"

#define LANG(a, b, c)	(((DWORD)(a) << 16) | ((DWORD)(b) << 8) | (DWORD)(c))

// ---------------------------------------------------------------- named values

static const DBENUM enumAudioType[] = {
	{0, "undefined"}, {1, "clean effects"}, {2, "hearing impaired"}, {3, "visual impaired commentary"}, {0, NULL}};
static const DBENUM enumTeletextType[] = {
	{1, "initial teletext page"}, {2, "teletext subtitle page"}, {3, "additional information page"},
	{4, "programme schedule page"}, {5, "subtitle page for the hard of hearing"}, {0, NULL}};
static const DBENUM enumSubtitlingType[] = {
	{0x01, "EBU teletext subtitles"}, {0x02, "associated EBU teletext"}, {0x03, "VBI data"},
	{0x10, "DVB subtitles, no aspect ratio"}, {0x11, "DVB subtitles 4:3"}, {0x12, "DVB subtitles 16:9"},
	{0x13, "DVB subtitles 2.21:1"}, {0x14, "DVB subtitles for HD"}, {0x15, "DVB subtitles plano-stereoscopic"},
	{0x16, "DVB subtitles for UHD"},
	{0x20, "hard of hearing, no aspect ratio"}, {0x21, "hard of hearing 4:3"}, {0x22, "hard of hearing 16:9"},
	{0x23, "hard of hearing 2.21:1"}, {0x24, "hard of hearing for HD"}, {0x25, "hard of hearing plano-stereoscopic"},
	{0x26, "hard of hearing for UHD"}, {0, NULL}};
static const DBENUM enumEnhancedAC3[] = {{0, "AC-3"}, {1, "Enhanced AC-3"}, {0, NULL}};
static const DBENUM enumFullService[] = {{0, "not a full service (needs mixing)"}, {1, "full service"}, {0, NULL}};
static const DBENUM enumAC3ServiceType[] = {
	{0, "complete main (CM)"}, {1, "music and effects (ME)"}, {2, "visually impaired (VI)"},
	{3, "hearing impaired (HI)"}, {4, "dialogue (D)"}, {5, "commentary (C)"}, {6, "emergency (E)"},
	{7, "voiceover (VO) / karaoke"}, {0, NULL}};
static const DBENUM enumAC3Channels[] = {
	{0, "mono"}, {1, "1+1 dual mono"}, {2, "2 channels (stereo)"}, {3, "2 channels surround encoded"},
	{4, "multichannel, more than 2"}, {5, "multichannel, more than 5.1"}, {6, "multiple substreams (E-AC-3)"},
	{7, "reserved"}, {0, NULL}};
static const DBENUM enumAACProfileLevel[] = {
	{0x50, "AAC profile, level 1"}, {0x51, "AAC profile, level 2"}, {0x52, "AAC profile, level 4"},
	{0x53, "AAC profile, level 5"}, {0x58, "HE-AAC profile, level 2"}, {0x59, "HE-AAC profile, level 3"},
	{0x5A, "HE-AAC profile, level 4"}, {0x5B, "HE-AAC profile, level 5"}, {0x60, "HE-AAC v2 profile, level 2"},
	{0x61, "HE-AAC v2 profile, level 3"}, {0x62, "HE-AAC v2 profile, level 4"}, {0x63, "HE-AAC v2 profile, level 5"},
	{0, NULL}};
static const DBENUM enumAACType[] = {
	{0x01, "HE-AAC mono"}, {0x03, "HE-AAC stereo"}, {0x05, "HE-AAC surround"},
	{0x40, "HE-AAC audio description for the visually impaired"}, {0x41, "HE-AAC for the hard of hearing"},
	{0x42, "HE-AAC receiver-mix supplementary audio"}, {0x43, "HE-AAC v2 stereo"},
	{0x44, "HE-AAC v2 audio description for the visually impaired"}, {0x45, "HE-AAC v2 for the hard of hearing"},
	{0x46, "HE-AAC v2 receiver-mix supplementary audio"}, {0x47, "HE-AAC receiver-mix audio description"},
	{0x48, "HE-AAC broadcast-mix audio description"}, {0x49, "HE-AAC v2 receiver-mix audio description"},
	{0x4A, "HE-AAC v2 broadcast-mix audio description"}, {0, NULL}};
static const DBENUM enumMixType[] = {
	{0, "supplementary stream (mixed with the main audio)"}, {1, "complete and independent stream"}, {0, NULL}};
static const DBENUM enumEditorial[] = {
	{0, "main audio"}, {1, "audio description for the visually impaired"}, {2, "clean audio for the hearing impaired"},
	{3, "spoken subtitles for the visually impaired"}, {0, NULL}};
static const DBENUM enumAudioID[] = {{0, "MPEG-2 lower sampling rates"}, {1, "MPEG-1"}, {0, NULL}};
static const DBENUM enumAudioLayer[] = {{1, "Layer III"}, {2, "Layer II"}, {3, "Layer I"}, {0, NULL}};
static const DBENUM enumStreamContent[] = {
	{1, "MPEG-2 video"}, {2, "MPEG-1 Layer 2 audio"}, {3, "EBU teletext / DVB subtitles"}, {4, "AC-3 audio"},
	{5, "H.264/AVC video"}, {6, "HE-AAC audio"}, {7, "DTS audio"}, {8, "DVB SRM data"},
	{9, "HEVC video / AC-4 / NGA (see stream_content_ext)"}, {0, NULL}};
static const DBENUM enumAVCProfile[] = {
	{66, "Baseline"}, {77, "Main"}, {88, "Extended"}, {100, "High"}, {110, "High 10"},
	{122, "High 4:2:2"}, {244, "High 4:4:4 Predictive"}, {0, NULL}};
static const DBENUM enumHEVCTier[] = {{0, "Main tier"}, {1, "High tier"}, {0, NULL}};
static const DBENUM enumHEVCProfile[] = {
	{1, "Main"}, {2, "Main 10"}, {3, "Main Still Picture"}, {4, "Format range extensions"}, {0, NULL}};
static const DBENUM enumHDRWCG[] = {
	{0, "SDR"}, {1, "WCG only"}, {2, "HDR and WCG"}, {3, "no indication"}, {0, NULL}};

// ---------------------------------------------------------------- meaning texts

static void MeaningBitrate(DWORD dwValue, char * szOut)
{
	DWORD dwKbps = dwValue * 400 / 1000;		// units of 50 bytes/s
	wsprintf(szOut, "%u.%03u Mbit/s", dwKbps / 1000, dwKbps % 1000);
}

static void MeaningAVCLevel(DWORD dwValue, char * szOut)
{
	wsprintf(szOut, "level %u.%u", dwValue / 10, dwValue % 10);
}

static void MeaningHEVCLevel(DWORD dwValue, char * szOut)
{
	wsprintf(szOut, "level %u.%u", dwValue / 30, (dwValue % 30) / 3);
}

// ---------------------------------------------------------------- definitions
// Columns: name, type, bits, condition (field index, -1 none), default, named values, hint, meaning

static const DBFIELD fieldsISO639[] = {
	{"entry", DF_LOOP, 0, -1, 0, NULL, NULL, NULL},
	{"language", DF_LANG, 24, -1, LANG('f', 'i', 'n'), NULL, "ISO 639-2 code, e.g. fin, swe, eng", NULL},
	{"audio_type", DF_ENUM, 8, -1, 0, enumAudioType, NULL, NULL},
	{"", DF_LOOPEND, 0, -1, 0, NULL, NULL, NULL}};

static const DBFIELD fieldsTeletext[] = {
	{"page", DF_LOOP, 0, -1, 0, NULL, NULL, NULL},
	{"language", DF_LANG, 24, -1, LANG('f', 'i', 'n'), NULL, "ISO 639-2 code, e.g. fin, swe, eng", NULL},
	{"teletext_type", DF_ENUM, 5, -1, 2, enumTeletextType, NULL, NULL},
	{"page_number", DF_TTXPAGE, 11, -1, 0x699, NULL, "Teletext page as on screen, e.g. 699 or 888 (magazine 1-8 and two hex digits)", NULL},
	{"", DF_LOOPEND, 0, -1, 0, NULL, NULL, NULL}};

static const DBFIELD fieldsSubtitling[] = {
	{"subtitles", DF_LOOP, 0, -1, 0, NULL, NULL, NULL},
	{"language", DF_LANG, 24, -1, LANG('f', 'i', 'n'), NULL, "ISO 639-2 code, e.g. fin, swe, eng", NULL},
	{"subtitling_type", DF_ENUM, 8, -1, 0x10, enumSubtitlingType, NULL, NULL},
	{"composition_page_id", DF_UINT, 16, -1, 1, NULL, "Page id of the subtitle stream's composition segments", NULL},
	{"ancillary_page_id", DF_UINT, 16, -1, 1, NULL, "Page id of shared segments (same as composition page if none)", NULL},
	{"", DF_LOOPEND, 0, -1, 0, NULL, NULL, NULL}};

static const DBFIELD fieldsStreamIdentifier[] = {
	{"component_tag", DF_HEX, 8, -1, 0x01, NULL, "Links the stream to component descriptors in the EIT/SDT", NULL}};

static const DBFIELD fieldsRegistration[] = {
	{"format_identifier", DF_FOURCC, 32, -1, 0x41432D33, NULL, "Four characters, e.g. AC-3, EAC3, HEVC, or 0x hex", NULL},
	{"additional_identification_info", DF_BYTES, 0, -1, 0, NULL, "Optional bytes in hex", NULL}};

static const DBFIELD fieldsAC3[] = {
	{"component_type_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"bsid_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"mainid_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"asvc_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"reserved", DF_RESERVED, 4, -1, 0, NULL, NULL, NULL},
	{"component_type: coding", DF_ENUM, 1, 0, 0, enumEnhancedAC3, "Bit 7 of component_type", NULL},
	{"component_type: full_service", DF_ENUM, 1, 0, 1, enumFullService, "Bit 6 of component_type", NULL},
	{"component_type: service_type", DF_ENUM, 3, 0, 0, enumAC3ServiceType, "Bits 5-3 of component_type", NULL},
	{"component_type: channels", DF_ENUM, 3, 0, 2, enumAC3Channels, "Bits 2-0 of component_type", NULL},
	{"bsid", DF_UINT, 8, 1, 8, NULL, "Bit stream identification, 8 for AC-3", NULL},
	{"mainid", DF_UINT, 8, 2, 0, NULL, "Main audio service id", NULL},
	{"asvc", DF_HEX, 8, 3, 0, NULL, "Associated service", NULL},
	{"additional_info", DF_BYTES, 0, -1, 0, NULL, "Optional bytes in hex", NULL}};

static const DBFIELD fieldsEAC3[] = {
	{"component_type_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"bsid_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"mainid_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"asvc_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"mixinfoexists", DF_FLAG, 1, -1, 0, NULL, "The stream carries mixing metadata", NULL},
	{"substream1_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"substream2_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"substream3_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"component_type: coding", DF_ENUM, 1, 0, 1, enumEnhancedAC3, "Bit 7 of component_type", NULL},
	{"component_type: full_service", DF_ENUM, 1, 0, 1, enumFullService, "Bit 6 of component_type", NULL},
	{"component_type: service_type", DF_ENUM, 3, 0, 0, enumAC3ServiceType, "Bits 5-3 of component_type", NULL},
	{"component_type: channels", DF_ENUM, 3, 0, 4, enumAC3Channels, "Bits 2-0 of component_type", NULL},
	{"bsid", DF_UINT, 8, 1, 16, NULL, "Bit stream identification, 16 for E-AC-3", NULL},
	{"mainid", DF_UINT, 8, 2, 0, NULL, "Main audio service id", NULL},
	{"asvc", DF_HEX, 8, 3, 0, NULL, "Associated service", NULL},
	{"substream1", DF_HEX, 8, 5, 0, NULL, "Component type of substream 1", NULL},
	{"substream2", DF_HEX, 8, 6, 0, NULL, "Component type of substream 2", NULL},
	{"substream3", DF_HEX, 8, 7, 0, NULL, "Component type of substream 3", NULL},
	{"additional_info", DF_BYTES, 0, -1, 0, NULL, "Optional bytes in hex", NULL}};

static const DBFIELD fieldsAAC[] = {
	{"profile_and_level", DF_ENUM, 8, -1, 0x58, enumAACProfileLevel, NULL, NULL},
	{"optional fields", DF_PRESENT, 0, -1, 1, NULL, "Whether the AAC type byte and additional info follow", NULL},
	{"AAC_type_flag", DF_FLAG, 1, 1, 1, NULL, NULL, NULL},
	{"SAOC_DE_flag", DF_FLAG, 1, 1, 0, NULL, "Spatial audio object coding dialogue enhancement", NULL},
	{"reserved", DF_RESERVED, 6, 1, 0, NULL, NULL, NULL},
	{"AAC_type", DF_ENUM, 8, 2, 0x03, enumAACType, NULL, NULL},
	{"additional_info", DF_BYTES, 0, 1, 0, NULL, "Optional bytes in hex", NULL}};

static const DBFIELD fieldsSupplementaryAudio[] = {
	{"descriptor_tag_extension", DF_RESERVED, 8, -1, 0x06, NULL, NULL, NULL},
	{"mix_type", DF_ENUM, 1, -1, 1, enumMixType, NULL, NULL},
	{"editorial_classification", DF_ENUM, 5, -1, 1, enumEditorial, NULL, NULL},
	{"reserved", DF_RESERVED, 1, -1, 1, NULL, NULL, NULL},
	{"language_code_present", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"language", DF_LANG, 24, 4, LANG('f', 'i', 'n'), NULL, "ISO 639-2 code, e.g. fin, swe, eng", NULL},
	{"private_data", DF_BYTES, 0, -1, 0, NULL, "Optional bytes in hex", NULL}};

static const DBFIELD fieldsAudioStream[] = {
	{"free_format_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"ID", DF_ENUM, 1, -1, 1, enumAudioID, NULL, NULL},
	{"layer", DF_ENUM, 2, -1, 2, enumAudioLayer, NULL, NULL},
	{"variable_rate_audio_indicator", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"reserved", DF_RESERVED, 3, -1, 7, NULL, NULL, NULL}};

static const DBFIELD fieldsComponent[] = {
	{"stream_content_ext", DF_HEX, 4, -1, 0xF, NULL, "0xF unless stream_content is 9 or more (EN 300 468 table 26)", NULL},
	{"stream_content", DF_ENUM, 4, -1, 5, enumStreamContent, NULL, NULL},
	{"component_type", DF_HEX, 8, -1, 0x0B, NULL, "Depends on stream_content, e.g. H.264: 0x0B HD 16:9 25 Hz; AC-3: same as the AC-3 component type", NULL},
	{"component_tag", DF_HEX, 8, -1, 0x01, NULL, "Same as the stream identifier descriptor of the stream", NULL},
	{"language", DF_LANG, 24, -1, LANG('f', 'i', 'n'), NULL, "ISO 639-2 code, e.g. fin, swe, eng", NULL},
	{"text", DF_TEXT, 0, -1, 0, NULL, "Optional description text", NULL}};

static const DBFIELD fieldsMaximumBitrate[] = {
	{"reserved", DF_RESERVED, 2, -1, 3, NULL, NULL, NULL},
	{"maximum_bitrate", DF_UINT, 22, -1, 12500, NULL, "In units of 50 bytes/s (12500 = 5 Mbit/s)", MeaningBitrate}};

static const DBFIELD fieldsAVC[] = {
	{"profile_idc", DF_ENUM, 8, -1, 100, enumAVCProfile, NULL, NULL},
	{"constraint_set0_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"constraint_set1_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"constraint_set2_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"constraint_set3_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"constraint_set4_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"constraint_set5_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"AVC_compatible_flags", DF_UINT, 2, -1, 0, NULL, NULL, NULL},
	{"level_idc", DF_UINT, 8, -1, 40, NULL, "Level times 10, e.g. 40 = level 4.0", MeaningAVCLevel},
	{"AVC_still_present", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"AVC_24_hour_picture_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"frame_packing_SEI_not_present_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"reserved", DF_RESERVED, 5, -1, 0x1F, NULL, NULL, NULL}};

static const DBFIELD fieldsHEVC[] = {
	{"profile_space", DF_UINT, 2, -1, 0, NULL, NULL, NULL},
	{"tier_flag", DF_ENUM, 1, -1, 0, enumHEVCTier, NULL, NULL},
	{"profile_idc", DF_ENUM, 5, -1, 2, enumHEVCProfile, NULL, NULL},
	{"profile_compatibility_indication", DF_HEX, 32, -1, 0x20000000, NULL, NULL, NULL},
	{"progressive_source_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"interlaced_source_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"non_packed_constraint_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"frame_only_constraint_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"copied_44bits (first 12)", DF_HEX, 12, -1, 0, NULL, NULL, NULL},
	{"copied_44bits (last 32)", DF_HEX, 32, -1, 0, NULL, NULL, NULL},
	{"level_idc", DF_UINT, 8, -1, 123, NULL, "Level times 30, e.g. 120 = 4.0, 123 = 4.1, 150 = 5.0", MeaningHEVCLevel},
	{"temporal_layer_subset_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"HEVC_still_present_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"HEVC_24hr_picture_present_flag", DF_FLAG, 1, -1, 0, NULL, NULL, NULL},
	{"sub_pic_hrd_params_not_present_flag", DF_FLAG, 1, -1, 1, NULL, NULL, NULL},
	{"reserved", DF_RESERVED, 2, -1, 3, NULL, NULL, NULL},
	{"HDR_WCG_idc", DF_ENUM, 2, -1, 3, enumHDRWCG, NULL, NULL},
	{"reserved", DF_RESERVED, 5, 11, 0x1F, NULL, NULL, NULL},
	{"temporal_id_min", DF_UINT, 3, 11, 0, NULL, NULL, NULL},
	{"reserved", DF_RESERVED, 5, 11, 0x1F, NULL, NULL, NULL},
	{"temporal_id_max", DF_UINT, 3, 11, 0, NULL, NULL, NULL}};

#define FIELDS(a)	a, sizeof(a) / sizeof(a[0])

static const DBDEF definitions[] = {
	{0x0A, -1, "ISO 639 language (0x0A)", FIELDS(fieldsISO639)},
	{0x6A, -1, "AC-3 (0x6A)", FIELDS(fieldsAC3)},
	{0x7A, -1, "Enhanced AC-3 (0x7A)", FIELDS(fieldsEAC3)},
	{0x7C, -1, "AAC (0x7C)", FIELDS(fieldsAAC)},
	{0x7F, 0x06, "Supplementary audio (0x7F/0x06)", FIELDS(fieldsSupplementaryAudio)},
	{0x03, -1, "MPEG audio stream (0x03)", FIELDS(fieldsAudioStream)},
	{0x56, -1, "Teletext (0x56)", FIELDS(fieldsTeletext)},
	{0x46, -1, "VBI teletext (0x46)", FIELDS(fieldsTeletext)},
	{0x59, -1, "Subtitling (0x59)", FIELDS(fieldsSubtitling)},
	{0x52, -1, "Stream identifier (0x52)", FIELDS(fieldsStreamIdentifier)},
	{0x05, -1, "Registration (0x05)", FIELDS(fieldsRegistration)},
	{0x50, -1, "Component (0x50)", FIELDS(fieldsComponent)},
	{0x0E, -1, "Maximum bitrate (0x0E)", FIELDS(fieldsMaximumBitrate)},
	{0x28, -1, "AVC video (0x28)", FIELDS(fieldsAVC)},
	{0x38, -1, "HEVC video (0x38)", FIELDS(fieldsHEVC)},
};

int DB_DefinitionCount()
{
	return sizeof(definitions) / sizeof(definitions[0]);
}

const DBDEF * DB_Definition(int nIndex)
{
	if (nIndex < 0 || nIndex >= DB_DefinitionCount())
		return NULL;
	return &definitions[nIndex];
}

const DBDEF * DB_FindDefinition(const BYTE * pDescriptor, int nLength)
{
	int i;

	if (nLength < 2)
		return NULL;
	for (i = 0; i < DB_DefinitionCount(); i++)
	{
		if (definitions[i].nTag != pDescriptor[0])
			continue;
		if (definitions[i].nExtension >= 0 && (nLength < 3 || pDescriptor[2] != definitions[i].nExtension))
			continue;
		return &definitions[i];
	}
	return NULL;
}

// ---------------------------------------------------------------- engine

int DB_LoopStart(const DBDEF * pDef)
{
	int i;

	for (i = 0; i < pDef->nFields; i++)
		if (pDef->pFields[i].nType == DF_LOOP)
			return i;
	return -1;
}

BOOL DB_InLoop(const DBDEF * pDef, int nField)
{
	int nLoop = DB_LoopStart(pDef);
	return nLoop >= 0 && nField > nLoop && pDef->pFields[nField].nType != DF_LOOPEND;
}

DBVALUE * DB_Value(DBINSTANCE * pInstance, int nField, int nEntry)
{
	if (DB_InLoop(pInstance->pDef, nField))
		return &pInstance->loop[nEntry][nField - DB_LoopStart(pInstance->pDef) - 1];
	return &pInstance->top[nField];
}

BOOL DB_FieldPresent(DBINSTANCE * pInstance, int nField, int nEntry)
{
	int nCondition = pInstance->pDef->pFields[nField].nCondition;

	if (nCondition < 0)
		return TRUE;
	if (!DB_FieldPresent(pInstance, nCondition, nEntry))
		return FALSE;
	return DB_Value(pInstance, nCondition, nEntry)->dwValue != 0;
}

typedef struct
{
	const BYTE * p;
	int nBitPos;
	int nBitCount;
} BITREADER;

static DWORD ReadBits(BITREADER * br, int nBits)
{
	DWORD dwValue = 0;

	while (nBits-- > 0)
	{
		dwValue = (dwValue << 1) | ((br->p[br->nBitPos >> 3] >> (7 - (br->nBitPos & 7))) & 1);
		br->nBitPos++;
	}
	return dwValue;
}

static BOOL DecodeField(DBINSTANCE * pInstance, int nField, int nEntry, BITREADER * br)
{
	const DBFIELD * pField = &pInstance->pDef->pFields[nField];
	DBVALUE * pValue = DB_Value(pInstance, nField, nEntry);
	int nLeft = br->nBitCount - br->nBitPos;

	memset(pValue, 0, sizeof(DBVALUE));
	if (!DB_FieldPresent(pInstance, nField, nEntry))
	{
		pValue->dwValue = pField->dwDefault;
		return TRUE;
	}
	switch (pField->nType)
	{
	case DF_PRESENT:
		pValue->dwValue = nLeft > 0;
		return TRUE;
	case DF_BYTES:
	case DF_TEXT:
		if (br->nBitPos & 7)
			return FALSE;
		pValue->nBytes = nLeft / 8;
		memcpy(pValue->bBytes, &br->p[br->nBitPos / 8], pValue->nBytes);
		br->nBitPos = br->nBitCount;
		return TRUE;
	default:
		if (pField->nBits > nLeft)
			return FALSE;
		pValue->dwValue = ReadBits(br, pField->nBits);
		return TRUE;
	}
}

// Decodes a whole descriptor; FALSE when it does not fit the definition
BOOL DB_Decode(const DBDEF * pDef, const BYTE * pDescriptor, int nLength, DBINSTANCE * pInstance)
{
	BITREADER br;
	int nLoop = DB_LoopStart(pDef), i;

	memset(pInstance, 0, sizeof(DBINSTANCE));
	pInstance->pDef = pDef;
	if (nLength < 2 || pDescriptor[1] != nLength - 2)
		return FALSE;
	br.p = &pDescriptor[2];
	br.nBitPos = 0;
	br.nBitCount = (nLength - 2) * 8;

	for (i = 0; i < pDef->nFields && i != nLoop; i++)
		if (!DecodeField(pInstance, i, 0, &br))
			return FALSE;
	if (nLoop >= 0)
	{
		int nEntryBits = 0;

		for (i = nLoop + 1; pDef->pFields[i].nType != DF_LOOPEND; i++)
			nEntryBits += pDef->pFields[i].nBits;
		while (br.nBitCount - br.nBitPos >= nEntryBits && pInstance->nEntries < DB_MAX_ENTRIES)
		{
			for (i = nLoop + 1; pDef->pFields[i].nType != DF_LOOPEND; i++)
				if (!DecodeField(pInstance, i, pInstance->nEntries, &br))
					return FALSE;
			pInstance->nEntries++;
		}
	}
	return br.nBitPos == br.nBitCount;
}

typedef struct
{
	BYTE * p;
	int nBitPos;
} BITWRITER;

static void WriteBits(BITWRITER * bw, DWORD dwValue, int nBits)
{
	while (nBits-- > 0)
	{
		if ((bw->nBitPos & 7) == 0)
			bw->p[bw->nBitPos >> 3] = 0;
		if ((dwValue >> nBits) & 1)
			bw->p[bw->nBitPos >> 3] |= 0x80 >> (bw->nBitPos & 7);
		bw->nBitPos++;
	}
}

static void EncodeField(DBINSTANCE * pInstance, int nField, int nEntry, BITWRITER * bw)
{
	const DBFIELD * pField = &pInstance->pDef->pFields[nField];
	DBVALUE * pValue = DB_Value(pInstance, nField, nEntry);
	int i;

	if (!DB_FieldPresent(pInstance, nField, nEntry))
		return;
	switch (pField->nType)
	{
	case DF_PRESENT:
	case DF_LOOP:
	case DF_LOOPEND:
		break;
	case DF_BYTES:
	case DF_TEXT:
		for (i = 0; i < pValue->nBytes; i++)
			WriteBits(bw, pValue->bBytes[i], 8);
		break;
	default:
		WriteBits(bw, pValue->dwValue, pField->nBits);
		break;
	}
}

// Builds the descriptor (tag, length and body); returns its length, 0 if over 255 bytes
int DB_Encode(DBINSTANCE * pInstance, BYTE * pOut)
{
	const DBDEF * pDef = pInstance->pDef;
	BYTE bBody[1024];
	BITWRITER bw;
	int nLoop = DB_LoopStart(pDef), i, e, nBytes;

	bw.p = bBody;
	bw.nBitPos = 0;
	for (i = 0; i < pDef->nFields && i != nLoop; i++)
		EncodeField(pInstance, i, 0, &bw);
	if (nLoop >= 0)
		for (e = 0; e < pInstance->nEntries; e++)
			for (i = nLoop + 1; pDef->pFields[i].nType != DF_LOOPEND; i++)
				EncodeField(pInstance, i, e, &bw);
	if (bw.nBitPos & 7)
		WriteBits(&bw, 0, 8 - (bw.nBitPos & 7));
	nBytes = bw.nBitPos / 8;
	if (nBytes > 255)
		return 0;
	pOut[0] = (BYTE)pDef->nTag;
	pOut[1] = (BYTE)nBytes;
	memcpy(&pOut[2], bBody, nBytes);
	return nBytes + 2;
}

static void SetDefault(DBINSTANCE * pInstance, int nField, int nEntry)
{
	DBVALUE * pValue = DB_Value(pInstance, nField, nEntry);

	memset(pValue, 0, sizeof(DBVALUE));
	pValue->dwValue = pInstance->pDef->pFields[nField].dwDefault;
}

// A new descriptor with the default values (and one entry when it has a loop)
void DB_New(const DBDEF * pDef, DBINSTANCE * pInstance)
{
	int nLoop = DB_LoopStart(pDef), i;

	memset(pInstance, 0, sizeof(DBINSTANCE));
	pInstance->pDef = pDef;
	for (i = 0; i < pDef->nFields; i++)
		if (pDef->pFields[i].nType != DF_LOOP && pDef->pFields[i].nType != DF_LOOPEND)
			SetDefault(pInstance, i, 0);
	pInstance->nEntries = nLoop >= 0 ? 1 : 0;
}

// ---------------------------------------------------------------- text

void DB_FormatValue(const DBFIELD * pField, DBVALUE * pValue, char * szOut, int nSize)
{
	DWORD dw = pValue->dwValue;
	int i, nOut = 0;

	szOut[0] = '\0';
	switch (pField->nType)
	{
	case DF_UINT:
		wsprintf(szOut, "%u", dw);
		break;
	case DF_HEX:
		wsprintf(szOut, "0x%0*X", (pField->nBits + 3) / 4, dw);
		break;
	case DF_ENUM:
		if (pField->nBits >= 8)
			wsprintf(szOut, "0x%02X", dw);
		else
			wsprintf(szOut, "%u", dw);
		break;
	case DF_FLAG:
	case DF_PRESENT:
		lstrcpy(szOut, dw ? "1" : "0");
		break;
	case DF_LANG:
		wsprintf(szOut, "%c%c%c", (char)((dw >> 16) & 0xff), (char)((dw >> 8) & 0xff), (char)(dw & 0xff));
		break;
	case DF_TTXPAGE:
		wsprintf(szOut, "%u%02X", ((dw >> 8) & 7) ? ((dw >> 8) & 7) : 8, dw & 0xff);
		break;
	case DF_FOURCC:
		{
			BOOL fText = TRUE;
			for (i = 0; i < 4; i++)
			{
				BYTE c = (BYTE)(dw >> (24 - 8 * i));
				if (c < 0x20 || c > 0x7e)
					fText = FALSE;
			}
			if (fText)
				wsprintf(szOut, "%c%c%c%c", (char)(dw >> 24), (char)(dw >> 16), (char)(dw >> 8), (char)dw);
			else
				wsprintf(szOut, "0x%08X", dw);
		}
		break;
	case DF_BYTES:
		for (i = 0; i < pValue->nBytes && nOut + 4 < nSize; i++)
			nOut += wsprintf(&szOut[nOut], i ? " %02X" : "%02X", pValue->bBytes[i]);
		break;
	case DF_TEXT:
		for (i = 0; i < pValue->nBytes && nOut + 2 < nSize; i++)
			szOut[nOut++] = pValue->bBytes[i] >= 0x20 ? (char)pValue->bBytes[i] : '.';
		szOut[nOut] = '\0';
		break;
	}
}

void DB_FormatMeaning(const DBFIELD * pField, DBVALUE * pValue, char * szOut, int nSize)
{
	szOut[0] = '\0';
	if (pField->pfnMeaning != NULL)
	{
		pField->pfnMeaning(pValue->dwValue, szOut);
		return;
	}
	if (pField->nType == DF_FLAG || pField->nType == DF_PRESENT)
		lstrcpy(szOut, pValue->dwValue ? "yes" : "no");
	else if (pField->nType == DF_ENUM)
	{
		const DBENUM * pEnum;

		lstrcpy(szOut, "(not a listed value)");
		for (pEnum = pField->pEnum; pEnum != NULL && pEnum->szName != NULL; pEnum++)
		{
			if ((DWORD)pEnum->nValue == pValue->dwValue)
			{
				lstrcpyn(szOut, pEnum->szName, nSize);
				break;
			}
		}
	}
	else if (pField->nType == DF_BYTES || pField->nType == DF_TEXT)
		wsprintf(szOut, "%d bytes", pValue->nBytes);
}

static BOOL ParseNumber(const char * sz, DWORD * pdwValue)
{
	char * pEnd;

	while (*sz == ' ')
		sz++;
	if (*sz == '\0')
		return FALSE;
	if (sz[0] == '0' && (sz[1] == 'x' || sz[1] == 'X'))
		*pdwValue = strtoul(sz + 2, &pEnd, 16);
	else
		*pdwValue = strtoul(sz, &pEnd, 10);
	while (*pEnd == ' ')
		pEnd++;
	return *pEnd == '\0';
}

// Reads a typed value; FALSE when it is not valid for the field (value left unchanged)
BOOL DB_ParseValue(const DBFIELD * pField, const char * szText, DBVALUE * pValue)
{
	DWORD dw;
	DWORD dwMax = pField->nBits >= 32 ? 0xffffffff : ((DWORD)1 << pField->nBits) - 1;

	switch (pField->nType)
	{
	case DF_LANG:
		if (lstrlen(szText) != 3)
			return FALSE;
		pValue->dwValue = LANG(tolower(szText[0]), tolower(szText[1]), tolower(szText[2]));
		return TRUE;
	case DF_TTXPAGE:
		{
			int nMagazine;
			char * pEnd;

			if (lstrlen(szText) != 3 || szText[0] < '1' || szText[0] > '8')
				return FALSE;
			nMagazine = szText[0] - '0';
			dw = strtoul(szText + 1, &pEnd, 16);
			if (*pEnd != '\0')
				return FALSE;
			pValue->dwValue = ((nMagazine & 7) << 8) | dw;
			return TRUE;
		}
	case DF_FOURCC:
		if (lstrlen(szText) == 4 && !(szText[0] == '0' && (szText[1] == 'x' || szText[1] == 'X')))
		{
			pValue->dwValue = ((DWORD)(BYTE)szText[0] << 24) | ((DWORD)(BYTE)szText[1] << 16) | ((DWORD)(BYTE)szText[2] << 8) | (BYTE)szText[3];
			return TRUE;
		}
		if (!ParseNumber(szText, &dw))
			return FALSE;
		pValue->dwValue = dw;
		return TRUE;
	case DF_BYTES:
		{
			int nCount = 0, nNibble = -1;
			const char * p;

			for (p = szText; *p; p++)
			{
				int n = -1;
				if (*p >= '0' && *p <= '9') n = *p - '0';
				else if (*p >= 'a' && *p <= 'f') n = *p - 'a' + 10;
				else if (*p >= 'A' && *p <= 'F') n = *p - 'A' + 10;
				else if (*p == ' ' || *p == ',')
					continue;
				else
					return FALSE;
				if (nNibble < 0)
					nNibble = n;
				else
				{
					if (nCount >= 250)
						return FALSE;
					pValue->bBytes[nCount++] = (BYTE)((nNibble << 4) | n);
					nNibble = -1;
				}
			}
			if (nNibble >= 0)
				return FALSE;
			pValue->nBytes = nCount;
			return TRUE;
		}
	case DF_TEXT:
		pValue->nBytes = min(lstrlen(szText), 250);
		memcpy(pValue->bBytes, szText, pValue->nBytes);
		return TRUE;
	default:
		if (!ParseNumber(szText, &dw) || dw > dwMax)
			return FALSE;
		pValue->dwValue = dw;
		return TRUE;
	}
}
