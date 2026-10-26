// Table-driven descriptor builder: definitions of descriptor fields, decoding a descriptor
// into field values and encoding field values back into a descriptor (DescriptorBuilder.c)

#ifndef DESCRIPTORBUILDER_H
#define DESCRIPTORBUILDER_H

// Field types
#define DF_UINT			1		// number, shown in decimal
#define DF_HEX			2		// number, shown in hex
#define DF_ENUM			3		// number with named values
#define DF_FLAG			4		// 1 bit, no / yes; other fields can depend on it
#define DF_RESERVED		5		// kept as received (default on a new descriptor), not shown
#define DF_LANG			6		// 24-bit ISO 639 language code
#define DF_TTXPAGE		7		// 11 bits: teletext magazine (3) + page (8), shown as e.g. 699
#define DF_FOURCC		8		// 32-bit format identifier, shown as text
#define DF_BYTES		9		// rest of the descriptor as hex
#define DF_TEXT			10		// rest of the descriptor as text
#define DF_PRESENT		11		// no bits: "the fields after this are present"; set when
								// a received descriptor still has bytes left at this point
#define DF_LOOP			12		// start of a repeating entry (runs to the end of the descriptor)
#define DF_LOOPEND		13

typedef struct
{
	int nValue;
	const char * szName;
} DBENUM;

typedef struct
{
	const char * szName;
	int nType;
	int nBits;
	int nCondition;				// index of the DF_FLAG / DF_PRESENT field this depends on, -1 none
	DWORD dwDefault;
	const DBENUM * pEnum;		// DF_ENUM: named values, ended by a NULL name
	const char * szHint;
	void (*pfnMeaning)(DWORD dwValue, char * szOut);	// optional: text for the Meaning column
} DBFIELD;

typedef struct
{
	int nTag;
	int nExtension;				// descriptor_tag_extension for tag 0x7F, else -1
	const char * szName;
	const DBFIELD * pFields;
	int nFields;
} DBDEF;

// A decoded descriptor: one value per field, loop fields once per entry
#define DB_MAX_FIELDS		64
#define DB_MAX_ENTRIES		32
#define DB_MAX_LOOP_FIELDS	8		// fields inside one repeating entry

typedef struct
{
	DWORD dwValue;
	int nBytes;					// DF_BYTES / DF_TEXT
	BYTE bBytes[256];
} DBVALUE;

typedef struct
{
	const DBDEF * pDef;
	DBVALUE top[DB_MAX_FIELDS];					// fields outside the loop
	DBVALUE loop[DB_MAX_ENTRIES][DB_MAX_LOOP_FIELDS];	// loop fields per entry (index: field - DF_LOOP - 1)
	int nEntries;
} DBINSTANCE;

int DB_DefinitionCount();
const DBDEF * DB_Definition(int nIndex);
const DBDEF * DB_FindDefinition(const BYTE * pDescriptor, int nLength);
BOOL DB_Decode(const DBDEF * pDef, const BYTE * pDescriptor, int nLength, DBINSTANCE * pInstance);
int DB_Encode(DBINSTANCE * pInstance, BYTE * pOut);		// whole descriptor, returns its length (0 = too long)
void DB_New(const DBDEF * pDef, DBINSTANCE * pInstance);
BOOL DB_FieldPresent(DBINSTANCE * pInstance, int nField, int nEntry);
DBVALUE * DB_Value(DBINSTANCE * pInstance, int nField, int nEntry);
int DB_LoopStart(const DBDEF * pDef);		// index of DF_LOOP, -1 when there is no loop
BOOL DB_InLoop(const DBDEF * pDef, int nField);
void DB_FormatValue(const DBFIELD * pField, DBVALUE * pValue, char * szOut, int nSize);
void DB_FormatMeaning(const DBFIELD * pField, DBVALUE * pValue, char * szOut, int nSize);
BOOL DB_ParseValue(const DBFIELD * pField, const char * szText, DBVALUE * pValue);

#endif
