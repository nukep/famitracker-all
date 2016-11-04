/*
** FamiTracker - NES/Famicom sound tracker
** Copyright (C) 2005-2014  Jonathan Liss
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful, 
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU 
** Library General Public License for more details.  To obtain a 
** copy of the GNU Library General Public License, write to the Free 
** Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
**
** Any permitted reproduction of these routines, in whole or in part,
** must bear this legend.
*/

#include <map>
#include <vector>
#include "stdafx.h"
#include "FamiTracker.h"
#include "FamiTrackerDoc.h"
#include "PatternCompiler.h"
#include "Compiler.h"
#include "ChunkRender.h"
#include "Driver.h"
#include "SoundGen.h"

//
// This is the new NSF data compiler, music is compiled to an object list instead of a binary chunk
//
// The list can be translated to both a binary chunk and an assembly file
// 

/*
 * TODO:
 *  - Remove duplicated FDS waves
 *  - What to do with the bank value in CHUNK_SONG??
 *  - Derive classes for each output format instead of separate functions
 *  - Create a config file for NSF driver optimizations
 *  - Pattern hash collisions prevents detecting similar patterns, fix that
 *  - Add bankswitching schemes for other memory mappers
 *
 */

/*
 * Notes:
 *
 *  - DPCM samples and instruments is currently stored as a linear list,
 *    which currently limits the number of possible DPCM configurations
 *    to 127.
 *  - Instrument data is non bankswitched, it might be possible to create
 *    a too large chunk of instrument data which makes export impossible.
 *
 */

/*
 * Bankswitched file layout:
 *
 * - $8000 - $AFFF: Music driver and song data (instruments, frames & patterns, unpaged)
 * - $B000 - $BFFF: Swichted part of song data (frames + patterns, 1 page only)
 * - $C000 - $EFFF: Samples (3 pages)
 * - $F000 - $FFFF: Fixed to last bank for compatibility with TNS HFC carts
 *
 * Non-bankswitched, compressed layout:
 *
 * - Music data, driver, DPCM samples
 * 
 * Non-bankswitched + bankswitched, default layout:
 *
 * - Driver, music data, DPCM samples
 *
 */

// Note: Each CCompiler object may only be used once (fix this)

// Remove duplicated patterns (default on)
#define REMOVE_DUPLICATE_PATTERNS

// Don't remove patterns across different tracks (default off)
//#define LOCAL_DUPLICATE_PATTERN_REMOVAL

// Enable bankswitching on all songs (default off)
//#define FORCE_BANKSWITCH

const int CCompiler::PAGE_SIZE					= 0x1000;
const int CCompiler::PAGE_START					= 0x8000;
const int CCompiler::PAGE_BANKED				= 0xB000;	// 0xB000 -> 0xBFFF
const int CCompiler::PAGE_SAMPLES				= 0xC000;

const int CCompiler::PATTERN_SWITCH_BANK		= 3;		// 0xB000 -> 0xBFFF

const int CCompiler::DPCM_PAGE_WINDOW			= 3;		// Number of switchable pages in the DPCM area
const int CCompiler::DPCM_SWITCH_ADDRESS		= 0xF000;	// Switch to new banks when reaching this address

const bool CCompiler::LAST_BANK_FIXED			= true;		// Fix for TNS carts

// Define channel maps, DPCM (4) is always located last
const int CCompiler::CHAN_ORDER_DEFAULT[]		= {0, 1, 2, 3, 4};
const int CCompiler::CHAN_ORDER_VRC6[]			= {0, 1, 2, 3, 5, 6, 7, 4};
const int CCompiler::CHAN_ORDER_MMC5[]			= {0, 1, 2, 3, 5, 6, 4};
const int CCompiler::CHAN_ORDER_VRC7[]			= {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 4};
const int CCompiler::CHAN_ORDER_FDS[]			= {0, 1, 2, 3, 5, 4};
const int CCompiler::CHAN_ORDER_N163[]			= {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 4};
const int CCompiler::CHAN_ORDER_S5B[]			= {0, 1, 2, 3, 5, 6, 7, 4};

// Assembly labels
const char CCompiler::LABEL_SONG_LIST[]			= "ft_song_list";
const char CCompiler::LABEL_INSTRUMENT_LIST[]	= "ft_instrument_list";
const char CCompiler::LABEL_SAMPLES_LIST[]		= "ft_sample_list";
const char CCompiler::LABEL_SAMPLES[]			= "ft_samples";
const char CCompiler::LABEL_WAVETABLE[]			= "ft_wave_table";
const char CCompiler::LABEL_SAMPLE[]			= "ft_sample_%i";			// one argument
const char CCompiler::LABEL_WAVES[]				= "ft_waves_%i";			// one argument
const char CCompiler::LABEL_SEQ_2A03[]			= "ft_seq_2a03_%i";			// one argument
const char CCompiler::LABEL_SEQ_VRC6[]			= "ft_seq_vrc6_%i";			// one argument
const char CCompiler::LABEL_SEQ_FDS[]			= "ft_seq_fds_%i";			// one argument
const char CCompiler::LABEL_SEQ_N163[]			= "ft_seq_n163_%i";			// one argument
const char CCompiler::LABEL_INSTRUMENT[]		= "ft_inst_%i";				// one argument
const char CCompiler::LABEL_SONG[]				= "ft_song_%i";				// one argument
const char CCompiler::LABEL_SONG_FRAMES[]		= "ft_s%i_frames";			// one argument
const char CCompiler::LABEL_SONG_FRAME[]		= "ft_s%if%i";				// two arguments
const char CCompiler::LABEL_PATTERN[]			= "ft_s%ip%ic%i";			// three arguments

// Flag byte flags
const int CCompiler::FLAG_BANKSWITCHED	= 1 << 0;
const int CCompiler::FLAG_VIBRATO		= 1 << 1;

CCompiler *CCompiler::pCompiler = NULL;

CCompiler *CCompiler::GetCompiler()
{
	return pCompiler;
}

// CCompiler

CCompiler::CCompiler(CFamiTrackerDoc *pDoc, CCompilerLog *pLogger) : 
	m_pDocument(pDoc), 
	m_iBanksUsed(0),
	m_pLogger(pLogger),
	m_iWaveTables(0),
	m_pSamplePointersChunk(NULL),
	m_pHeaderChunk(NULL),
	m_pDriverData(NULL),
	m_pCurrentBank(NULL),
	m_iHashCollisions(0)
{
	ASSERT(CCompiler::pCompiler == NULL);
	CCompiler::pCompiler = this;

	memset(m_pFileBanks, 0, sizeof(CFileBank*) * 256);
}

CCompiler::~CCompiler()
{
	CCompiler::pCompiler = NULL;

	Cleanup();

	SAFE_RELEASE(m_pLogger);
}

void CCompiler::Print(LPCTSTR text, ...) const
{
 	static TCHAR buf[256];

	if (m_pLogger == NULL)
		return;

	va_list argp;
    va_start(argp, text);

    if (!text)
		return;

	_vsntprintf_s(buf, sizeof(buf), _TRUNCATE, text, argp);

	size_t len = _tcslen(buf);

	if (buf[len - 1] == '\n' && len < (sizeof(buf) - 1)) {
		buf[len - 1] = '\r';
		buf[len] = '\n';
		buf[len + 1] = 0;
	}

	m_pLogger->WriteLog(buf);
}

void CCompiler::ClearLog() const
{
	if (m_pLogger != NULL)
		m_pLogger->Clear();
}

void CCompiler::ExportNSF(LPCTSTR lpszFileName, int MachineType)
{
	stNSFHeader Header;
	unsigned short MusicDataAddress;

	// Compressed mode means that driver and music is located just below the sample space, no space is lost even when samples are used
	bool bCompressedMode;

	ClearLog();

	// Build the music data
	if (!CompileData()) {
		// Failed
		Cleanup();
		return;
	}

	if (m_bBankSwitched) {
		// Expand and allocate label addresses
		AddBankswitching();
		if (!ResolveLabelsBankswitched()) {
			Cleanup();
			return;
		}
		// Write bank data
		UpdateFrameBanks();
		UpdateSongBanks();
		// Make driver aware of bankswitching
		EnableBankswitching();
	}
	else {
		ResolveLabels();
		ClearSongBanks();
	}

	// Rewrite DPCM sample pointers
	UpdateSamplePointers(m_iSampleStart);

	// Find out load address
	if ((PAGE_SAMPLES - m_iDriverSize - m_iMusicDataSize) < 0x8000 || m_bBankSwitched)
		bCompressedMode = false;
	else
		bCompressedMode = true;
	
	if (bCompressedMode) {
		// Locate driver at $C000 - (driver size)
		m_iLoadAddress = PAGE_SAMPLES - m_iDriverSize - m_iMusicDataSize;
		m_iDriverAddress = PAGE_SAMPLES - m_iDriverSize;
		MusicDataAddress = m_iLoadAddress;
	}
	else {
		// Locate driver at $8000
		m_iLoadAddress = PAGE_START;
		m_iDriverAddress = PAGE_START;
		MusicDataAddress = m_iLoadAddress + m_iDriverSize;
	}

	// Init is located first at the driver
	m_iInitAddress = m_iDriverAddress + 8;

	// Load driver
	unsigned char *pDriver = LoadDriver(m_pDriverData, m_iDriverAddress);

	// Patch driver binary
	PatchVibratoTable(pDriver);

	// Copy the Namco table, if used
	if (m_pDocument->GetExpansionChip() & SNDCHIP_N163) {

		CSoundGen *pSoundGen = theApp.GetSoundGenerator();

		for (int i = 0; i < 96; ++i) {
			*(pDriver + m_iDriverSize - 258 - 192 + i * 2 + 0) = (unsigned char)(pSoundGen->ReadNamcoPeriodTable(i) & 0xFF);
			*(pDriver + m_iDriverSize - 258 - 192 + i * 2 + 1) = (unsigned char)(pSoundGen->ReadNamcoPeriodTable(i) >> 8);
		}

		// Patch the channel list
		// TODO move this to the actual music data
		int NamcoChannels = m_pDocument->GetNamcoChannels();
		if (NamcoChannels != 8) {
			/*
			TRACE0("before\n");
			for (int i = 0; i < 13; ++i)
				TRACE1("%02X ", *(pDriver + m_iDriverSize - 258 - 96 * 6 - 13 + i));
			TRACE0("\n");
			for (int i = 0; i < 13; ++i)
				TRACE1("%02X ", *(pDriver + m_iDriverSize - 258 - 96 * 6 - 13 * 2 + i));
			TRACE0("\n");
			*/
			
			// Channel type
			*(pDriver + m_iDriverSize - 258 - 96 * 6 - (9 - NamcoChannels)) = 0;		// 2A03
			// Channel id
			*(pDriver + m_iDriverSize - 258 - 96 * 6 - 13 - (9 - NamcoChannels)) = 5;	// DPCM
			
			/*
			TRACE0("after\n");
			for (int i = 0; i < 13; ++i)
				TRACE1("%02X ", *(pDriver + m_iDriverSize - 258 - 96 * 6 - 13 + i));
			TRACE0("\n");
			for (int i = 0; i < 13; ++i)
				TRACE1("%02X ", *(pDriver + m_iDriverSize - 258 - 96 * 6 - 13 * 2 + i));
			TRACE0("\n");
			*/
		}
	}

	// Write music data address
	SetDriverSongAddress(pDriver, MusicDataAddress);

	// Create first empty bank
	m_iBanksUsed = 0;
	AllocateBank(m_iLoadAddress & 0xF000);
	m_pCurrentBank->m_iOffset = m_iLoadAddress & 0xFFF;
	m_pCurrentBank->m_iSize = m_pFileBanks[m_iBanksUsed - 1]->m_iOffset;

	// Setup banks
	if (m_bBankSwitched) {
		// Copy data to banks
		CopyData((char*)pDriver, m_iDriverSize);
		WriteBinaryBankswitched();
		WriteSamplesToBanks(m_iSampleStart);
	}
	else {
		if (bCompressedMode) {
			// Copy data to banks
			WriteBinaryFlat();
			CopyData((char*)pDriver, m_iDriverSize);
			WriteSamplesToBanks(m_iSampleStart);
		}
		else {
			// Copy data to banks
			CopyData((char*)pDriver, m_iDriverSize);
			WriteBinaryFlat();
			WriteSamplesToBanks(m_iSampleStart);
		}
	}

	// Open output file
	CFileException ex;
	CFile OutputFile;
	if (!OutputFile.Open(lpszFileName, CFile::modeWrite | CFile::modeCreate, &ex)) {
		TCHAR szCause[255];
		CString strFormatted;
		ex.GetErrorMessage(szCause, 255);
		strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
		strFormatted += _T(" ");
		strFormatted += szCause;
		AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
		Print(_T("Error: Could not open output file\n"));
		Cleanup();
		SAFE_RELEASE_ARRAY(pDriver);
		return;
	}

	// Create NSF header
	CreateHeader(&Header, MachineType);

	// Write header
	OutputFile.Write(&Header, sizeof(stNSFHeader));

	// Write data to file
	for (unsigned int i = 0; i < m_iBanksUsed; ++i) {
		OutputFile.Write(m_pFileBanks[i]->m_data + m_pFileBanks[i]->m_iOffset, m_pFileBanks[i]->m_iSize - m_pFileBanks[i]->m_iOffset);
	}

	// Writing done, print some stats
	Print(_T(" * NSF load address: $%04X\n"), m_iLoadAddress);
	Print(_T("Writing output file...\n"));
	Print(_T(" * Driver size: %i bytes\n"), m_iDriverSize);

	if (m_bBankSwitched) {
		int Percent = (100 * m_iMusicDataSize) / (0x80000 - m_iDriverSize - m_iSamplesSize);
		Print(_T(" * Song data size: %i bytes (%i%%)\n"), m_iMusicDataSize, Percent);
		Print(_T(" * NSF type: Bankswitched (%i banks)\n"), m_iBanksUsed - 1);
	}
	else {
		int Percent = (100 * m_iMusicDataSize) / (0x8000 - m_iDriverSize - m_iSamplesSize);
		Print(_T(" * Song data size: %i bytes (%i%%)\n"), m_iMusicDataSize, Percent);
		Print(_T(" * NSF type: Linear (driver @ $%04X)\n"), m_iDriverAddress);
	}

	// Remove allocated data
	SAFE_RELEASE_ARRAY(pDriver);

	Print(_T("Done, total file size: %i bytes\n"), OutputFile.GetLength());

	// Done
	OutputFile.Close();

	Cleanup();
}

void CCompiler::ExportNES(LPCTSTR lpszFileName, bool EnablePAL)
{
	CFileException ex;
	CFile OutputFile;

	unsigned char *pDriver;
	unsigned short MusicDataAddress;

	// 32kb NROM, no CHR
	const char NES_HEADER[] = {
		0x4E, 0x45, 0x53, 0x1A, 0x02, 0x00, 0x00, 0x00, 
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
	};

	ClearLog();

	if (m_pDocument->GetExpansionChip() != SNDCHIP_NONE) {
		Print(_T("Error: Expansion chips not supported.\n"));
		AfxMessageBox(_T("Expansion chips are currently not supported when exporting to .NES!"), 0, 0);
		return;
	}

	if (!OutputFile.Open(lpszFileName, CFile::modeWrite | CFile::modeCreate, &ex)) {
		TCHAR szCause[255];
		CString strFormatted;
		ex.GetErrorMessage(szCause, 255);
		strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
		strFormatted += _T(" ");
		strFormatted += szCause;
		AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
		return;
	}

	// Build the music data
	if (!CompileData()) {
		Cleanup();
		return;
	}

	if (m_bBankSwitched) {
		// Abort if larger than 32kb
		Print(_T("Error: Song is too large, aborted.\n"));
		AfxMessageBox(_T("Song is too big to fit into 32kB!"), 0, 0);
		Cleanup();
		return;
	}

	ResolveLabels();

	// Rewrite DPCM sample pointers
	UpdateSamplePointers(m_iSampleStart);

	// Locate driver at $8000
	m_iLoadAddress = PAGE_START;
	m_iDriverAddress = PAGE_START;
	MusicDataAddress = m_iLoadAddress + m_iDriverSize;

	// Init is located first at the driver
	m_iInitAddress = m_iDriverAddress + 8;

	// Load driver
	pDriver = LoadDriver(m_pDriverData, m_iDriverAddress);

	// Patch driver binary
	PatchVibratoTable(pDriver);

	// Write music data address
	SetDriverSongAddress(pDriver, MusicDataAddress);

	// Create first empty bank
	m_iBanksUsed = 0;
	AllocateBank(m_iLoadAddress & 0xF000);
	m_pCurrentBank->m_iOffset = m_iLoadAddress & 0xFFF;
	m_pCurrentBank->m_iSize = m_pFileBanks[m_iBanksUsed - 1]->m_iOffset;

	// Copy data to banks
	CopyData((char*)pDriver, m_iDriverSize);
	WriteBinaryFlat();
	WriteSamplesToBanks(m_iSampleStart);

	// Write NSF caller
	while (m_iBanksUsed < 8)
		AllocateBank(m_pCurrentBank->m_iLocation + PAGE_SIZE);

	m_pCurrentBank->m_iSize = PAGE_SIZE;
	memcpy(m_pCurrentBank->m_data + PAGE_SIZE - NSF_CALLER_SIZE, NSF_CALLER_BIN, NSF_CALLER_SIZE);

	int Percent = (100 * m_iMusicDataSize) / (0x8000 - m_iDriverSize - m_iSamplesSize);

	Print(_T("Writing file...\n"));
	Print(_T(" * Driver size: %i bytes\n"), m_iDriverSize);
	Print(_T(" * Song data size: %i bytes (%i%%)\n"), m_iMusicDataSize, Percent);

	// Write header
	OutputFile.Write(NES_HEADER, 0x10);

	// Write data to file
	for (unsigned int i = 0; i < m_iBanksUsed; ++i) {
		OutputFile.Write(m_pFileBanks[i]->m_data + m_pFileBanks[i]->m_iOffset, m_pFileBanks[i]->m_iSize - m_pFileBanks[i]->m_iOffset);
	}

	Print(_T("Done, total file size: %i bytes\n"), 0x8000 + 0x10);

	// Done
	OutputFile.Close();

	Cleanup();
}

void CCompiler::ExportBIN(LPCTSTR lpszBIN_File, LPCTSTR lpszDPCM_File)
{
	CFileException ex;
	CFile OutputFileBIN, OutputFileDPCM;

	ClearLog();

	// Build the music data
	if (!CompileData())
		return;

	if (m_bBankSwitched) {
		Print(_T("Error: Can't write bankswitched songs!\n"));
		return;
	}

	// Convert to binary
	ResolveLabels();

	if (!OutputFileBIN.Open(lpszBIN_File, CFile::modeWrite | CFile::modeCreate, &ex)) {
		TCHAR szCause[255];
		CString strFormatted;
		ex.GetErrorMessage(szCause, 255);
		strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
		strFormatted += _T(" ");
		strFormatted += szCause;
		AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
		OutputFileBIN.Close();
		return;
	}

	if (_tcslen(lpszDPCM_File) != 0) {
		if (!OutputFileDPCM.Open(lpszDPCM_File, CFile::modeWrite | CFile::modeCreate, &ex)) {
			TCHAR szCause[255];
			CString strFormatted;
			ex.GetErrorMessage(szCause, 255);
			strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
			strFormatted += _T(" ");
			strFormatted += szCause;
			AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
			OutputFileBIN.Close();
			OutputFileDPCM.Close();
			return;
		}
	}

	Print(_T("Writing output files...\n"));

	WriteBinary(&OutputFileBIN);

	if (_tcslen(lpszDPCM_File) != 0)
		WriteSamplesBinary(&OutputFileDPCM);

	Print(_T("Done\n"));

	// Done
	OutputFileBIN.Close();

	if (_tcslen(lpszDPCM_File) != 0)
		OutputFileDPCM.Close();

	Cleanup();
}

void CCompiler::ExportPRG(LPCTSTR lpszFileName, bool EnablePAL)
{
	// Same as export to .NES but without the header

	CFileException ex;
	CFile OutputFile;

	unsigned char *pDriver;
	unsigned short MusicDataAddress;

	ClearLog();

	if (m_pDocument->GetExpansionChip() != SNDCHIP_NONE) {
		Print(_T("Expansion chips not supported.\n"));
		AfxMessageBox(_T("Error: Expansion chips is currently not supported when exporting to PRG!"), 0, 0);
		return;
	}

	if (!OutputFile.Open(lpszFileName, CFile::modeWrite | CFile::modeCreate, &ex)) {
		TCHAR szCause[255];
		CString strFormatted;
		ex.GetErrorMessage(szCause, 255);
		strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
		strFormatted += _T(" ");
		strFormatted += szCause;
		AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
		return;
	}

	// Build the music data
	if (!CompileData())
		return;

	if (m_bBankSwitched) {
		// Abort if larger than 32kb
		Print(_T("Song is too big, aborted.\n"));
		AfxMessageBox(_T("Error: Song is too big to fit!"), 0, 0);
		return;
	}

	ResolveLabels();

	// Rewrite DPCM sample pointers
	UpdateSamplePointers(m_iSampleStart);

	// Locate driver at $8000
	m_iLoadAddress = PAGE_START;
	m_iDriverAddress = PAGE_START;
	MusicDataAddress = m_iLoadAddress + m_iDriverSize;

	// Init is located first at the driver
	m_iInitAddress = m_iDriverAddress + 8;

	// Load driver
	pDriver = LoadDriver(m_pDriverData, m_iDriverAddress);

	// Patch driver binary
	PatchVibratoTable(pDriver);

	// Write music data address
	SetDriverSongAddress(pDriver, MusicDataAddress);

	// Create first empty bank
	m_iBanksUsed = 0;
	AllocateBank(m_iLoadAddress & 0xF000);
	m_pCurrentBank->m_iOffset = m_iLoadAddress & 0xFFF;
	m_pCurrentBank->m_iSize = m_pFileBanks[m_iBanksUsed - 1]->m_iOffset;

	// Copy data to banks
	CopyData((char*)pDriver, m_iDriverSize);
	WriteBinaryFlat();
	WriteSamplesToBanks(m_iSampleStart);

	// Write NSF caller
	while (m_iBanksUsed < 8)
		AllocateBank(m_pCurrentBank->m_iLocation + PAGE_SIZE);

	m_pCurrentBank->m_iSize = PAGE_SIZE;
	memcpy(m_pCurrentBank->m_data + PAGE_SIZE - NSF_CALLER_SIZE, NSF_CALLER_BIN, NSF_CALLER_SIZE);

	int Percent = (100 * m_iMusicDataSize) / (0x8000 - m_iDriverSize - m_iSamplesSize);

	Print(_T("Writing file...\n"));
	Print(_T(" * Driver size: %i bytes\n"), m_iDriverSize);
	Print(_T(" * Song data size: %i bytes (%i%%)\n"), m_iMusicDataSize, Percent);

	// Write data to file
	for (unsigned int i = 0; i < m_iBanksUsed; ++i) {
		OutputFile.Write(m_pFileBanks[i]->m_data + m_pFileBanks[i]->m_iOffset, m_pFileBanks[i]->m_iSize - m_pFileBanks[i]->m_iOffset);
	}

	Print(_T("Done, total file size: %i bytes\n"), 0x8000);

	// Done
	OutputFile.Close();

	Cleanup();
}

void CCompiler::ExportASM(LPCTSTR lpszFileName)
{
	ClearLog();

	// Build the music data
	if (!CompileData())
		return;

	if (m_bBankSwitched) {
		// TODO: bankswitching is still unsupported when exporting to ASM
		AddBankswitching();
		ResolveLabelsBankswitched();
		EnableBankswitching();
		UpdateFrameBanks();
		UpdateSongBanks();
	}
	else {
		ResolveLabels();
		ClearSongBanks();
	}

	UpdateSamplePointers(PAGE_SAMPLES);		// Always start at C000 when exporting to ASM

	Print(_T("Writing output files...\n"));

	CFileException ex;
	CFile OutputFile;
	if (!OutputFile.Open(lpszFileName, CFile::modeWrite | CFile::modeCreate, &ex)) {
		TCHAR szCause[255];
		CString strFormatted;
		ex.GetErrorMessage(szCause, 255);
		strFormatted.LoadString(IDS_OPEN_FILE_ERROR);
		strFormatted += _T(" ");
		strFormatted += szCause;
		AfxMessageBox(strFormatted, MB_OK | MB_ICONERROR);
		return;
	}

	// Write output file
	WriteAssembly(&OutputFile);

	// Write DPCM samples
	WriteSamplesAssembly(&OutputFile);

	// Done
	OutputFile.Close();

	Print(_T("Done\n"));

	Cleanup();
}

unsigned char *CCompiler::LoadDriver(const driver_t *pDriver, unsigned short Origin) const
{
	// Copy embedded driver
	unsigned char* pData = new unsigned char[pDriver->driver_size];
	memcpy(pData, pDriver->driver, pDriver->driver_size);

	// Relocate driver
	for (size_t i = 0; i < (pDriver->word_reloc_size / sizeof(int)); ++i) {
		// Words
		unsigned short value = pData[pDriver->word_reloc[i]] + (pData[pDriver->word_reloc[i] + 1] << 8);
		value += Origin;
		pData[pDriver->word_reloc[i]] = value & 0xFF;
		pData[pDriver->word_reloc[i] + 1] = value >> 8;
	}

	for (size_t i = 0; i < (pDriver->byte_reloc_size / sizeof(int)); i += 2) {	// each item is a pair
		// Low bytes
		int value = pDriver->byte_reloc_low[i + 1];
		if (value < 0)
			value = 65536 + value;
		value += Origin;
		pData[pDriver->byte_reloc_low[i]] = value & 0xFF;
		// High bytes
		value = pDriver->byte_reloc_high[i + 1];
		if (value < 0)
			value = 65536 + value;
		value += Origin;
		pData[pDriver->byte_reloc_high[i]] = value >> 8;
	}

	return pData;
}

void CCompiler::SetDriverSongAddress(unsigned char *pDriver, unsigned short Address) const
{
	// Write start address of music data
	pDriver[m_iDriverSize - 2] = Address & 0xFF;
	pDriver[m_iDriverSize - 1] = Address >> 8;
}

void CCompiler::PatchVibratoTable(unsigned char *pDriver) const
{
	// Copy the vibrato table, the stock one only works for new vibrato mode
	CSoundGen *pSoundGen = theApp.GetSoundGenerator();

	for (int i = 0; i < 256; ++i) {
		*(pDriver + m_iVibratoTableLocation + i) = (char)pSoundGen->ReadVibratoTable(i);
	}
}

void CCompiler::CreateHeader(stNSFHeader *pHeader, int MachineType) const
{
	// Fill the NSF header
	//
	// Speed will be the same for NTSC/PAL
	//

	int SpeedPAL, SpeedNTSC, Speed;

	Speed = m_pDocument->GetEngineSpeed();

	// If speed is default, write correct NTSC/PAL speed periods
	if (Speed == 0) {
		SpeedNTSC = 1000000 / 60;
		SpeedPAL = 1000000 / 50;
	}
	else {
		// else, set the same custom speed for both
		SpeedNTSC = SpeedPAL = 1000000 / Speed;
	}

	memset(pHeader, 0, 0x80);

	pHeader->Ident[0]	= 0x4E;
	pHeader->Ident[1]	= 0x45;
	pHeader->Ident[2]	= 0x53;
	pHeader->Ident[3]	= 0x4D;
	pHeader->Ident[4]	= 0x1A;

	pHeader->Version	= 0x01;
	pHeader->TotalSongs	= m_pDocument->GetTrackCount();
	pHeader->StartSong	= 1;
	pHeader->LoadAddr	= m_iLoadAddress;
	pHeader->InitAddr	= m_iInitAddress;
	pHeader->PlayAddr	= m_iInitAddress + 3;

	memset(pHeader->SongName, 0x00, 32);
	memset(pHeader->ArtistName, 0x00, 32);
	memset(pHeader->Copyright, 0x00, 32);

	strcpy_s((char*)pHeader->SongName,	 32, m_pDocument->GetSongName());
	strcpy_s((char*)pHeader->ArtistName, 32, m_pDocument->GetSongArtist());
	strcpy_s((char*)pHeader->Copyright,  32, m_pDocument->GetSongCopyright());

	pHeader->Speed_NTSC = SpeedNTSC; //0x411A; // default ntsc speed

	if (m_bBankSwitched) {
		for (int i = 0; i < 4; ++i) {
			pHeader->BankValues[i] = i;
			pHeader->BankValues[i + 4] = m_iFirstSampleBank + i;
		}
		if (LAST_BANK_FIXED) {
			// Bind last page to last bank
			pHeader->BankValues[7] = m_iBanksUsed - 1;
		}
	}
	else {
		for (int i = 0; i < 8; ++i) {
			pHeader->BankValues[i] = 0;
		}
	}

	pHeader->Speed_PAL = SpeedPAL; //0x4E20; // default pal speed

	// Allow PAL or dual tunes only if no expansion chip is selected
	// Expansion chips weren't available in PAL areas
	if (m_pDocument->GetExpansionChip() == SNDCHIP_NONE) {
		switch (MachineType) {
			case 0:	// NTSC
				pHeader->Flags = 0x00;
				break;
			case 1:	// PAL
				pHeader->Flags = 0x01;
				break;
			case 2:	// Dual
				pHeader->Flags = 0x02;
				break;
		}
	}
	else {
		pHeader->Flags = 0x00;
	}

	// Expansion chip
	pHeader->SoundChip = m_pDocument->GetExpansionChip();

	pHeader->Reserved[0] = 0x00;
	pHeader->Reserved[1] = 0x00;
	pHeader->Reserved[2] = 0x00;
	pHeader->Reserved[3] = 0x00;
}

unsigned int CCompiler::AdjustSampleAddress(unsigned int Address) const
{
	// Align samples to 64-byte pages
	return (0x40 - (Address & 0x3F)) & 0x3F;
}

void CCompiler::UpdateSamplePointers(unsigned int Origin)
{
	// Rewrite sample pointer list with valid addresses
	//
	// TODO: rewrite this to utilize the CChunkDataBank to resolve bank numbers automatically
	//

	ASSERT(m_pSamplePointersChunk != NULL);

	unsigned int Address = Origin;
	unsigned int Bank = m_iFirstSampleBank;

	if (!m_bBankSwitched)
		Bank = 0;			// Disable DPCM bank switching

	m_pSamplePointersChunk->Clear();

	// The list is stored in the same order as the samples vector
	for (std::vector<CDSample*>::iterator it = m_vSamples.begin(); it != m_vSamples.end(); ++it) {

		CDSample *pDSample = *it;
		unsigned int Size = pDSample->SampleSize;

		if (m_bBankSwitched) {
			if ((Address + Size) >= DPCM_SWITCH_ADDRESS) {
				Address = PAGE_SAMPLES;
				Bank += DPCM_PAGE_WINDOW;
			}
		}

		// Store
		m_pSamplePointersChunk->StoreByte(Address >> 6);
		m_pSamplePointersChunk->StoreByte(Size >> 4);
		m_pSamplePointersChunk->StoreByte(Bank);

#ifdef _DEBUG
		Print(_T(" * DPCM sample %s: $%04X, bank %i (%i bytes)\n"), pDSample->Name, Address, Bank, Size);
#endif
		Address += pDSample->SampleSize;
		Address += AdjustSampleAddress(Address);
	}
#ifdef _DEBUG
	Print(_T(" * DPCM sample banks: %i\n"), Bank - m_iFirstSampleBank + DPCM_PAGE_WINDOW);
#endif
}

void CCompiler::UpdateFrameBanks()
{
	// Write bank numbers to frame lists (can only be used when bankswitching is used)

	int Channels = m_pDocument->GetAvailableChannels();

	for (std::vector<CChunk*>::iterator it = m_vFrameChunks.begin(); it != m_vFrameChunks.end(); ++it) {
		CChunk *pChunk = *it;
		if (pChunk->GetType() == CHUNK_FRAME) {
			// Add bank data
			for (int j = 0; j < Channels; ++j) {
				unsigned char bank = GetObjectByRef(pChunk->GetDataRefName(j))->GetBank();
				if (bank < PATTERN_SWITCH_BANK)
					bank = PATTERN_SWITCH_BANK;
				pChunk->SetupBankData(j + Channels, bank);
			}
		}
	}
}

void CCompiler::UpdateSongBanks()
{
	// Write bank numbers to song lists (can only be used when bankswitching is used)

	for (std::vector<CChunk*>::iterator it = m_vSongChunks.begin(); it != m_vSongChunks.end(); ++it) {
		CChunk *pChunk = *it;
		int bank = GetObjectByRef(pChunk->GetDataRefName(0))->GetBank();
		if (bank < PATTERN_SWITCH_BANK)
			bank = PATTERN_SWITCH_BANK;
		pChunk->SetupBankData(m_iSongBankReference, bank);
	}
}

void CCompiler::ClearSongBanks()
{
	// Clear bank data in song chunks
	for (std::vector<CChunk*>::iterator it = m_vSongChunks.begin(); it != m_vSongChunks.end(); ++it) {
		(*it)->SetupBankData(m_iSongBankReference, 0);
	}
}

void CCompiler::EnableBankswitching()
{
	// Set bankswitching flag in the song header
	ASSERT(m_pHeaderChunk != NULL);
	unsigned char flags = (unsigned char)m_pHeaderChunk->GetData(m_iHeaderFlagOffset);
	flags |= FLAG_BANKSWITCHED;
	m_pHeaderChunk->ChangeByte(m_iHeaderFlagOffset, flags);
}

void CCompiler::ResolveLabels()
{
	// Resolve label addresses, no banks since bankswitching is disabled
	//

	CMap<CStringA, LPCSTR, int, int> labelMap;
	int Offset = 0;

	// Pass 1, collect labels

	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		labelMap[(*it)->GetLabel()] = Offset;
		Offset += (*it)->CountDataSize();
	}

	// Pass 2
	AssignLabels(labelMap);
}

bool CCompiler::ResolveLabelsBankswitched()
{
	// Resolve label addresses and banks
	//

	CMap<CStringA, LPCSTR, int, int> labelMap;
	int Offset = 0;
	int Bank = 3;

	// Pass 1, collect labels

	// Instruments and stuff
	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		int Size = pChunk->CountDataSize();

		switch (pChunk->GetType()) {
			case CHUNK_FRAME_LIST:
			case CHUNK_FRAME:
			case CHUNK_PATTERN:
				break;
			default:
				labelMap[pChunk->GetLabel()] = Offset;
				Offset += Size;
		}
	}

	if (Offset + m_iDriverSize > 0x3000) {
		// Instrument data did not fit within the limit, display an error and abort?
		Print(_T("Error: Instrument data overflow, can't export file!\n"));
		return false;
	}

	int iTrack = 0;

	// The switchable area is $B000-$C000
	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		int Size = pChunk->CountDataSize();

		switch (pChunk->GetType()) {
			case CHUNK_FRAME_LIST:
				// Make sure the entire frame list will fit, if not then allocate a new bank
				if (Offset + m_iDriverSize + m_iTrackFrameSize[iTrack++] > 0x4000) {
					Offset = 0x3000 - m_iDriverSize;
					++Bank;
				}
			case CHUNK_FRAME:
				labelMap[pChunk->GetLabel()] = Offset;
				pChunk->SetBank(Bank < 4 ? ((Offset + m_iDriverSize) >> 12) : Bank);
				Offset += Size;
				break;
			case CHUNK_PATTERN:
				// Make sure entire pattern will fit
				if (Offset + m_iDriverSize + Size > 0x4000) {
					Offset = 0x3000 - m_iDriverSize;
					++Bank;
				}
				labelMap[pChunk->GetLabel()] = Offset;
				pChunk->SetBank(Bank < 4 ? ((Offset + m_iDriverSize) >> 12) : Bank);
				Offset += Size;
			default:
				break;
		}
	}

	if (m_bBankSwitched)
		m_iFirstSampleBank = ((Bank < 4) ? ((Offset + m_iDriverSize) >> 12) : Bank) + 1;

	// Pass 2
	AssignLabels(labelMap);

	return true;
}

void CCompiler::CollectLabels(CMap<CStringA, LPCSTR, int, int> &labelMap)
{
	// Pass 1: collect labels
	// Todo: Move code here
}

void CCompiler::AssignLabels(CMap<CStringA, LPCSTR, int, int> &labelMap)
{
	// Pass 2: assign addresses to labels
	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		(*it)->AssignLabels(labelMap);
	}
}

bool CCompiler::CompileData()
{
	// Compile music data to an object tree
	//

	int Channels = m_pDocument->GetAvailableChannels();

	// Select driver and channel order
	switch (m_pDocument->GetExpansionChip()) {
		case SNDCHIP_NONE:
			m_pDriverData = &DRIVER_PACK_2A03;
			memcpy(m_iChanOrder, CHAN_ORDER_DEFAULT, Channels * sizeof(int));
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_NONE;
			Print(_T(" * No expansion chip\n"));
			break;
		case SNDCHIP_VRC6:
			m_pDriverData = &DRIVER_PACK_VRC6;
			memcpy(m_iChanOrder, CHAN_ORDER_VRC6, Channels * sizeof(int));
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_VRC6;
			Print(_T(" * VRC6 expansion enabled\n"));
			break;
		case SNDCHIP_MMC5:
			m_pDriverData = &DRIVER_PACK_MMC5;
			memcpy(m_iChanOrder, CHAN_ORDER_MMC5, Channels * sizeof(int));
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_MMC5;
			Print(_T(" * MMC5 expansion enabled\n"));
			break;
		case SNDCHIP_VRC7:
			m_pDriverData = &DRIVER_PACK_VRC7;
			memcpy(m_iChanOrder, CHAN_ORDER_VRC7, Channels * sizeof(int));
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_VRC7;
			Print(_T(" * VRC7 expansion enabled\n"));
			break;
		case SNDCHIP_FDS:
			m_pDriverData = &DRIVER_PACK_FDS;
			memcpy(m_iChanOrder, CHAN_ORDER_FDS, Channels * sizeof(int));
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_FDS;
			Print(_T(" * FDS expansion enabled\n"));
			break;
		case SNDCHIP_N163:
			m_pDriverData = &DRIVER_PACK_N163;
			memcpy(m_iChanOrder, CHAN_ORDER_N163, Channels * sizeof(int));
			{
				int namco_chans = m_pDocument->GetNamcoChannels();
				m_iChanOrder[4 + namco_chans] = CHANID_DPCM;	
			}
			m_iVibratoTableLocation = VIBRATO_TABLE_LOCATION_N163;
			Print(_T(" * N163 expansion enabled\n"));
			break;
		default:
			Print(_T("Error: Selected expansion chip is unsupported\n"));
			return false;
	}

	// Driver size
	m_iDriverSize = m_pDriverData->driver_size;

	// Scan and optimize song
	ScanSong();

	Print(_T("Building music data...\n"));

	// Build music data
	CreateMainHeader();
	CreateSequenceList();
	CreateInstrumentList();
	CreateSampleList();
	StoreSamples();
	StoreSongs();

	// Determine if bankswitching is needed
	m_bBankSwitched = false;
	m_iMusicDataSize = CountData();

	// Get samples start address
	m_iSampleStart = m_iDriverSize + m_iMusicDataSize;

	if (m_iSampleStart < 0x4000)
		m_iSampleStart = PAGE_SAMPLES;
	else
		m_iSampleStart += AdjustSampleAddress(m_iSampleStart) + PAGE_START;

	if (m_iSampleStart + m_iSamplesSize > 0xFFFF)
		m_bBankSwitched = true;

	if (m_iSamplesSize > 0x4000)
		m_bBankSwitched = true;

	if ((m_iMusicDataSize + m_iSamplesSize + m_iDriverSize) > 0x8000)
		m_bBankSwitched = true;

	if (m_bBankSwitched)
		m_iSampleStart = PAGE_SAMPLES;

	// Compiling done
	Print(_T(" * Samples located at: $%04X\n"), m_iSampleStart);

#ifdef FORCE_BANKSWITCH
	m_bBankSwitched = true;
#endif /* FORCE_BANKSWITCH */

	return true;
}

void CCompiler::Cleanup()
{
	// Delete objects

	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		delete *it;
	}

	for (unsigned int i = 0; i < m_iBanksUsed; ++i) {
		SAFE_RELEASE(m_pFileBanks[i]);
	}

	m_vChunks.clear();
	m_vSequenceChunks.clear();
	m_vInstrumentChunks.clear();
	m_vSongChunks.clear();
	m_vFrameChunks.clear();
	m_vPatternChunks.clear();

	m_pSamplePointersChunk = NULL;	// This pointer is also stored in m_vChunks
	m_pHeaderChunk = NULL;
}

void CCompiler::AddBankswitching()
{
	// Add bankswitching data

	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		// Frame chunks
		if (pChunk->GetType() == CHUNK_FRAME) {
			int Length = pChunk->GetLength();
			// Bank data is located at end
			for (int j = 0; j < Length; ++j) {
				pChunk->StoreBankReference(pChunk->GetDataRefName(j), 0);
			}
		}
	}

	// Frame lists sizes has changed
	int iTrackCount = m_pDocument->GetTrackCount();
	for (int i = 0; i < iTrackCount; ++i) {
		m_iTrackFrameSize[i] += m_pDocument->GetChannelCount() * m_pDocument->GetFrameCount(i);
	}

	// Data size has changed
	m_iMusicDataSize = CountData();
}

void CCompiler::ScanSong()
{
	// Scan and optimize song
	//

	// Re-assign instruments
	m_iInstruments = 0;

	memset(m_iAssignedInstruments, 0, sizeof(int) * MAX_INSTRUMENTS);
	memset(m_bSequencesUsed2A03, false, sizeof(bool) * MAX_SEQUENCES * SEQ_COUNT);
	memset(m_bSequencesUsedVRC6, false, sizeof(bool) * MAX_SEQUENCES * SEQ_COUNT);
	memset(m_bSequencesUsedN163, false, sizeof(bool) * MAX_SEQUENCES * SEQ_COUNT);

	for (int i = 0; i < MAX_INSTRUMENTS; ++i) {
		if (m_pDocument->IsInstrumentUsed(i) && IsInstrumentInPattern(i)) {
			
			// List of used instruments
			m_iAssignedInstruments[m_iInstruments++] = i;
			
			// Create a list of used sequences
			switch (m_pDocument->GetInstrumentType(i)) {
				case INST_2A03: 
					{
						CInstrumentContainer<CInstrument2A03> instContainer(m_pDocument, i);
						CInstrument2A03 *pInstrument = instContainer();
						for (int j = 0; j < CInstrument2A03::SEQUENCE_COUNT; ++j) {
							if (pInstrument->GetSeqEnable(j))
								m_bSequencesUsed2A03[pInstrument->GetSeqIndex(j)][j] = true;
						}
					}
					break;
				case INST_VRC6: 
					{
						CInstrumentContainer<CInstrumentVRC6> instContainer(m_pDocument, i);
						CInstrumentVRC6 *pInstrument = instContainer();
						for (int j = 0; j < CInstrumentVRC6::SEQUENCE_COUNT; ++j) {
							if (pInstrument->GetSeqEnable(j))
								m_bSequencesUsedVRC6[pInstrument->GetSeqIndex(j)][j] = true;
						}
					}
					break;
				case INST_N163:
					{
						CInstrumentContainer<CInstrumentN163> instContainer(m_pDocument, i);
						CInstrumentN163 *pInstrument = instContainer();
						for (int j = 0; j < CInstrumentN163::SEQUENCE_COUNT; ++j) {
							if (pInstrument->GetSeqEnable(j))
								m_bSequencesUsedN163[pInstrument->GetSeqIndex(j)][j] = true;
						}
					}
					break;
			}
		}
	}

	// See which samples are used
	m_iSamplesUsed = 0;

	memset(m_bSamplesAccessed, 0, MAX_INSTRUMENTS * OCTAVE_RANGE * NOTE_RANGE * sizeof(bool));

	const int DPCM_CHAN = 4;	// TODO: change this

	int trackCount = m_pDocument->GetTrackCount();
	unsigned int Instrument = 0;

	for (int i = 0; i < trackCount; ++i) {
		int patternlen = m_pDocument->GetPatternLength(i);
		int frames = m_pDocument->GetFrameCount(i);
		for (int j = 0; j < frames; ++j) {
			int p = m_pDocument->GetPatternAtFrame(i, j, DPCM_CHAN);
			for (int k = 0; k < patternlen; ++k) {
				stChanNote Note;
				m_pDocument->GetDataAtPattern(i, p, DPCM_CHAN, k, &Note);
				if (Note.Instrument < MAX_INSTRUMENTS)
					Instrument = Note.Instrument;
				if (Note.Note > 0) {
					m_bSamplesAccessed[Instrument][Note.Octave][Note.Note - 1] = true;
				}
			}
		}
	}
}

bool CCompiler::IsInstrumentInPattern(int index) const
{
	// Returns true if the instrument is used in a pattern

	int TrackCount = m_pDocument->GetTrackCount();
	int Channels = m_pDocument->GetAvailableChannels();
	stChanNote Note;

	// Scan patterns in entire module
	for (int i = 0; i < TrackCount; ++i) {
		int PatternLength = m_pDocument->GetPatternLength(i);
		for (int j = 0; j < Channels; ++j) {
			for (int k = 0; k < MAX_PATTERN; ++k) {
				for (int l = 0; l < PatternLength; ++l) {
					m_pDocument->GetDataAtPattern(i, k, j, l, &Note);
					if (Note.Instrument == index)
						return true;
				}
			}
		}
	}	

	return false;
}

void CCompiler::CreateMainHeader()
{
	// Writes the music header
	int TicksPerSec = m_pDocument->GetEngineSpeed();

	unsigned short DividerNTSC, DividerPAL;

	CChunk *pChunk = CreateChunk(CHUNK_HEADER, "");

	if (TicksPerSec == 0) {
		// Default
		DividerNTSC = CAPU::FRAME_RATE_NTSC * 60;
		DividerPAL	= CAPU::FRAME_RATE_PAL * 60;
	}
	else {
		// Custom
		DividerNTSC = TicksPerSec * 60;
		DividerPAL = TicksPerSec * 60;
	}

	unsigned char Flags = ((m_pDocument->GetVibratoStyle() == VIBRATO_OLD) ? FLAG_VIBRATO : 0);	// bankswitch flag is set later

	// Write header

	pChunk->StoreReference(LABEL_SONG_LIST);
	pChunk->StoreReference(LABEL_INSTRUMENT_LIST);
	pChunk->StoreReference(LABEL_SAMPLES_LIST);
	pChunk->StoreReference(LABEL_SAMPLES);
	
	m_iHeaderFlagOffset = pChunk->GetLength();		// Save the flags offset
	pChunk->StoreByte(Flags);

	// FDS table, only if FDS is enabled
	if (m_pDocument->ExpansionEnabled(SNDCHIP_FDS))
		pChunk->StoreReference(LABEL_WAVETABLE);

	pChunk->StoreWord(DividerNTSC);
	pChunk->StoreWord(DividerPAL);

	// N163 channel count
	if (m_pDocument->GetExpansionChip() & SNDCHIP_N163) {
		pChunk->StoreByte(m_pDocument->GetNamcoChannels());
	}

	m_pHeaderChunk = pChunk;
}

// Sequences

void CCompiler::CreateSequenceList()
{
	// Create sequence lists
	//

	unsigned int Size = 0, StoredCount = 0;
	CStringA label;

	for (int i = 0; i < MAX_SEQUENCES; ++i) {
		for (int j = 0; j < CInstrument2A03::SEQUENCE_COUNT; ++j) {
			CSequence* pSeq = m_pDocument->GetSequence(i, j);

			if (m_bSequencesUsed2A03[i][j] && pSeq->GetItemCount() > 0) {
				int Index = i * SEQ_COUNT + j;
				label.Format(LABEL_SEQ_2A03, Index);
				Size += StoreSequence(pSeq, label);
				++StoredCount;
			}
		}
	}

	if (m_pDocument->ExpansionEnabled(SNDCHIP_VRC6)) {
		for (int i = 0; i < MAX_SEQUENCES; ++i) {
			for (int j = 0; j < CInstrumentVRC6::SEQUENCE_COUNT; ++j) {
				CSequence* pSeq = m_pDocument->GetSequence(SNDCHIP_VRC6, i, j);

				if (m_bSequencesUsedVRC6[i][j] && pSeq->GetItemCount() > 0) {
					int Index = i * SEQ_COUNT + j;
					label.Format(LABEL_SEQ_VRC6, Index);
					Size += StoreSequence(pSeq, label);
					++StoredCount;
				}
			}
		}
	}

	
	if (m_pDocument->ExpansionEnabled(SNDCHIP_N163)) {
		for (int i = 0; i < MAX_SEQUENCES; ++i) {
			for (int j = 0; j < CInstrumentN163::SEQUENCE_COUNT; ++j) {
				CSequence* pSeq = m_pDocument->GetSequence(SNDCHIP_N163, i, j);

				if (m_bSequencesUsedN163[i][j] && pSeq->GetItemCount() > 0) {
					int Index = i * SEQ_COUNT + j;
					label.Format(LABEL_SEQ_N163, Index);
					Size += StoreSequence(pSeq, label);
					++StoredCount;
				}
			}
		}
	}
	

	if (m_pDocument->ExpansionEnabled(SNDCHIP_FDS)) {
		// TODO: this is bad, fds only uses 3 sequences

		for (int i = 0; i < MAX_INSTRUMENTS; ++i) {
			CInstrumentContainer<CInstrumentFDS> instContainer(m_pDocument, i);
			CInstrumentFDS *pInstrument = instContainer();

			if (pInstrument != NULL && pInstrument->GetType() == INST_FDS) {
				for (int j = 0; j < 3; ++j) {
					CSequence* pSeq;
					switch (j) {
						case 0: pSeq = pInstrument->GetVolumeSeq(); break;
						case 1: pSeq = pInstrument->GetArpSeq(); break;
						case 2: pSeq = pInstrument->GetPitchSeq(); break;
					}
					if (pSeq->GetItemCount() > 0) {
						int Index = i * SEQ_COUNT + j;
						label.Format(LABEL_SEQ_FDS, Index);
						Size += StoreSequence(pSeq, label);
						++StoredCount;
					}
				}
			}
		}
	}

	Print(_T(" * Sequences used: %i (%i bytes)\n"), StoredCount, Size);
}

int CCompiler::StoreSequence(CSequence *pSeq, CStringA &label)
{
	CChunk *pChunk = CreateChunk(CHUNK_SEQUENCE, label);
	m_vSequenceChunks.push_back(pChunk);

	// Store the sequence
	int iItemCount	  = pSeq->GetItemCount();
	int iLoopPoint	  = pSeq->GetLoopPoint();
	int iReleasePoint = pSeq->GetReleasePoint();
	int iSetting	  = pSeq->GetSetting();

	if (iReleasePoint != -1)
		iReleasePoint += 1;
	else
		iReleasePoint = 0;

	if (iLoopPoint > iItemCount)
		iLoopPoint = -1;

	pChunk->StoreByte((unsigned char)iItemCount);
	pChunk->StoreByte((unsigned char)iLoopPoint);
	pChunk->StoreByte((unsigned char)iReleasePoint);
	pChunk->StoreByte((unsigned char)iSetting);

	for (int i = 0; i < iItemCount; ++i) {
		pChunk->StoreByte(pSeq->GetItem(i));
	}

	// Return size of this chunk
	return iItemCount + 4;
}

// Instruments

void CCompiler::CreateInstrumentList()
{
	/*
	 * Create the instrument list
	 *
	 * The format of instruments depends on the type
	 *
	 */

	unsigned int iTotalSize = 0;	
	CChunk *pWavetableChunk = NULL;	// FDS
	CChunk *pWavesChunk = NULL;		// N163
	CStringA label;
	int iWaveSize = 0;				// N163 waves size

	CChunk *pInstListChunk = CreateChunk(CHUNK_INSTRUMENT_LIST, LABEL_INSTRUMENT_LIST);
	
	if (m_pDocument->GetExpansionChip() & SNDCHIP_FDS) {
		pWavetableChunk = CreateChunk(CHUNK_WAVETABLE, LABEL_WAVETABLE);
	}

	memset(m_iWaveBanks, -1, MAX_INSTRUMENTS * sizeof(int));

	// Collect N163 waves
	for (unsigned int i = 0; i < m_iInstruments; ++i) {
		int iIndex = m_iAssignedInstruments[i];
		if (m_pDocument->GetInstrumentType(iIndex) == INST_N163 && m_iWaveBanks[i] == -1) {

			CInstrumentContainer<CInstrumentN163> instContainer(m_pDocument, iIndex);
			CInstrumentN163 *pInstrument = instContainer();

			for (unsigned int j = i + 1; j < m_iInstruments; ++j) {
				int inst = m_iAssignedInstruments[j];
				if (m_pDocument->GetInstrumentType(inst) == INST_N163 && m_iWaveBanks[j] == -1) {
					CInstrumentContainer<CInstrumentN163> instContainer(m_pDocument, inst);
					CInstrumentN163 *pNewInst = instContainer();
					if (pInstrument->IsWaveEqual(pNewInst)) {
						m_iWaveBanks[j] = iIndex;
					}
				}
			}
			if (m_iWaveBanks[i] == -1) {
				m_iWaveBanks[i] = iIndex;
				// Store wave
				CStringA label;
				label.Format(LABEL_WAVES, iIndex);
				pWavesChunk = CreateChunk(CHUNK_WAVES, label);
				// Store waves
				iWaveSize += pInstrument->StoreWave(pWavesChunk);
			}
		}
	}

	// Store instruments
	for (unsigned int i = 0; i < m_iInstruments; ++i) {
		// Add reference to instrument list
		label.Format(LABEL_INSTRUMENT, i);
		pInstListChunk->StoreReference(label);
		iTotalSize += 2;

		// Actual instrument
		CChunk *pChunk = CreateChunk(CHUNK_INSTRUMENT, label);
		m_vInstrumentChunks.push_back(pChunk);

		int iIndex = m_iAssignedInstruments[i];
		CInstrumentContainer<CInstrument> instContainer(m_pDocument, iIndex);
		CInstrument *pInstrument = instContainer();

		// Check if FDS
		if (pInstrument->GetType() == INST_FDS && pWavetableChunk != NULL) {
			// Store wave
			AddWavetable(static_cast<CInstrumentFDS*>(pInstrument), pWavetableChunk);
			pChunk->StoreByte(m_iWaveTables - 1);
		}
/*
		if (pInstrument->GetType() == INST_N163) {
			CString label;
			label.Format(LABEL_WAVES, iIndex);
			pWavesChunk = CreateChunk(CHUNK_WAVES, label);
			// Store waves
			iWaveSize += ((CInstrumentN163*)pInstrument)->StoreWave(pWavesChunk);
		}
*/

		if (pInstrument->GetType() == INST_N163) {
			// Translate wave index
			iIndex = m_iWaveBanks[i];
		}

		// Returns number of bytes 
		iTotalSize += pInstrument->Compile(m_pDocument, pChunk, iIndex);
	}

	Print(_T(" * Instruments used: %i (%i bytes)\n"), m_iInstruments, iTotalSize);

	if (iWaveSize > 0)
		Print(_T(" * N163 waves size: %i bytes\n"), iWaveSize);
}

// Samples

void CCompiler::CreateSampleList()
{
	/*
	 * DPCM instrument list
	 *
	 * Each item is stored as a pair of the sample pitch and pointer to the sample table
	 *
	 */

	const int SAMPLE_ITEM_WIDTH = 3;	// 3 bytes / sample item

	unsigned char iSample, iSamplePitch, iSampleDelta;
	unsigned int Item = 0, iSampleIndex;

	// Clear the sample list
	memset(m_iSampleBank, 0xFF, MAX_DSAMPLES);
	
	CChunk *pChunk = CreateChunk(CHUNK_SAMPLE_LIST, LABEL_SAMPLES_LIST);

	// Store sample instruments
	for (int i = 0; i < MAX_INSTRUMENTS; ++i) {

		if (m_pDocument->IsInstrumentUsed(i) && m_pDocument->GetInstrumentType(i) == INST_2A03) {
			CInstrumentContainer<CInstrument2A03> instContainer(m_pDocument, i);
			CInstrument2A03 *pInstrument = instContainer();

			for (int j = 0; j < OCTAVE_RANGE; ++j) {
				for (int k = 0; k < NOTE_RANGE; ++k) {
					// Get sample
					iSample = pInstrument->GetSample(j, k);
					if ((iSample > 0) && m_bSamplesAccessed[i][j][k] && m_pDocument->GetSampleSize(iSample - 1) > 0) {

						iSamplePitch  = pInstrument->GetSamplePitch(j, k);
						iSamplePitch |= (iSamplePitch & 0x80) >> 1;
						iSampleIndex  = GetSampleIndex(iSample - 1);
						iSampleDelta  = pInstrument->GetSampleDeltaValue(j, k);

						// Save a reference to this item
						m_iSamplesLookUp[i][j][k] = ++Item;

						pChunk->StoreByte(iSamplePitch);
						pChunk->StoreByte(iSampleDelta);
						pChunk->StoreByte(iSampleIndex * SAMPLE_ITEM_WIDTH);
					}
					else
						// No instrument here
						m_iSamplesLookUp[i][j][k] = 0;
				}
			}
		}
	}
}

void CCompiler::StoreSamples()
{
	/*
	 * DPCM sample list
	 *
	 * Each sample is stored as a pair of the sample address and sample size
	 *
	 */

	unsigned int iAddedSamples = 0;
	unsigned int iSampleAddress = 0x0000;

	// Get sample start address
	m_iSamplesSize = 0;

	CChunk *pChunk = CreateChunk(CHUNK_SAMPLE_POINTERS, LABEL_SAMPLES);
	m_pSamplePointersChunk = pChunk;

	// Store DPCM samples in a separate array
	for (unsigned int i = 0; i < m_iSamplesUsed; ++i) {

		unsigned int iIndex = m_iSampleBank[i];
		ASSERT(iIndex != 0xFF);
		CDSample *pDSample = m_pDocument->GetSample(iIndex);
		unsigned int iSize = pDSample->SampleSize;

		if (iSize > 0) {
			// Fill sample list
			unsigned char iSampleAddr = iSampleAddress >> 6;
			unsigned char iSampleSize = iSize >> 4;
			unsigned char iSampleBank = 0;

			// Update SAMPLE_ITEM_WIDTH here
			pChunk->StoreByte(iSampleAddr);
			pChunk->StoreByte(iSampleSize);
			pChunk->StoreByte(iSampleBank);

			// Add this sample to storage
			m_vSamples.push_back(pDSample);

			// Pad end of samples
			unsigned int iAdjust = AdjustSampleAddress(iSampleAddress + iSize);

			iAddedSamples++;
			iSampleAddress += iSize + iAdjust;
			m_iSamplesSize += iSize + iAdjust;
		}
	}

	Print(_T(" * DPCM samples used: %i (%i bytes)\n"), m_iSamplesUsed, m_iSamplesSize);
}

int CCompiler::GetSampleIndex(int SampleNumber)
{
	// Returns a sample pos from the sample bank
	for (int i = 0; i < MAX_DSAMPLES; i++) {
		if (m_iSampleBank[i] == SampleNumber)
			return i;							// Sample is already stored
		else if(m_iSampleBank[i] == 0xFF) {
			m_iSampleBank[i] = SampleNumber;	// Allocate new position
			m_iSamplesUsed++;
			return i;
		}
	}

	// TODO: Fail if getting here!!!
	return SampleNumber;
}

// Songs

void CCompiler::StoreSongs()
{
	/*
	 * Store patterns and frames for each song
	 * 
	 */

	int iSongCount = m_pDocument->GetTrackCount();
	CStringA label;

	CChunk *pSongListChunk = CreateChunk(CHUNK_SONG_LIST, LABEL_SONG_LIST);

	m_iDuplicatePatterns = 0;

	// Store song info
	for (int i = 0; i < iSongCount; ++i) {

		// Add reference to song list
		label.Format(LABEL_SONG, i);
		pSongListChunk->StoreReference(label);

		// Create song
		CChunk *pChunk = CreateChunk(CHUNK_SONG, label);
		m_vSongChunks.push_back(pChunk);

		// Store reference to song
		label.Format(LABEL_SONG_FRAMES, i);
		pChunk->StoreReference(label);
		pChunk->StoreByte(m_pDocument->GetFrameCount(i));
		pChunk->StoreByte(m_pDocument->GetPatternLength(i));
		pChunk->StoreByte(m_pDocument->GetSongSpeed(i));
		pChunk->StoreByte(m_pDocument->GetSongTempo(i));
		pChunk->StoreBankReference(label, 0);
	}

	m_iSongBankReference = m_vSongChunks[0]->GetLength() - 1;	// Save bank value position (all songs are equal)

	// Store actual songs
	for (int i = 0; i < iSongCount; ++i) {

		// Store frames
		int iFrameCount, iFrameSize;
		CreateFrameList(i, iFrameSize, iFrameCount);

		// Store pattern data
		int iPatternCount, iPatternSize;
		StorePatterns(i, iPatternSize, iPatternCount);

		Print(_T(" * Song %i: %i frames (%i bytes), %i patterns (%i bytes)\r\n"), i + 1, iFrameCount, iFrameSize, iPatternCount, iPatternSize);
	}

	if (m_iDuplicatePatterns > 0)
		Print(_T(" * %i duplicated pattern(s) removed\n"), m_iDuplicatePatterns);
	
#ifdef _DEBUG
	Print(_T("Hash collisions: %i (of %i items)\r\n"), m_iHashCollisions, m_PatternMap.GetCount());
#endif
}

// Frames

void CCompiler::CreateFrameList(int Track, int &iFrameSize, int &iFrameCount)
{
	/*
	 * Creates a frame list
	 *
	 * The pointer list is just pointing to each item in the frame list
	 * and the frame list holds the offset addresses for the patterns for all channels
	 *
	 * ---------------------
	 *  Frame entry pointers
	 *  $XXXX (2 bytes, offset to a frame entry)
	 *  ...
	 * ---------------------
	 *
	 * ---------------------
	 *  Frame entries
	 *  $XXXX * 4 (2 * 2 bytes, each pair is an offset to the pattern)
	 * ---------------------
	 *
	 */
	
	int	FrameCount	 = m_pDocument->GetFrameCount(Track);
	int	ChannelCount = m_pDocument->GetAvailableChannels();
	CStringA label;

	// Create frame list
	label.Format(LABEL_SONG_FRAMES, Track);
	CChunk *pFrameListChunk = CreateChunk(CHUNK_FRAME_LIST, label);

	unsigned int TotalSize = 0;

	// Store addresses to patterns
	for (int i = 0; i < FrameCount; ++i) {

		// Add reference to frame list
		label.Format(LABEL_SONG_FRAME, Track, i);
		pFrameListChunk->StoreReference(label);
		TotalSize += 2;

		// Store frame item
		CChunk *pChunk = CreateChunk(CHUNK_FRAME, label);
		m_vFrameChunks.push_back(pChunk);

		// Pattern pointers
		for (int j = 0; j < ChannelCount; ++j) {
			int iChan = m_iChanOrder[j];
			if (m_pDocument->GetExpansionChip() & SNDCHIP_N163) {
				if (j == ChannelCount - 1)	// TODO hardcode last chan to DPCM
					iChan = 4;
			}
			int iPattern = m_pDocument->GetPatternAtFrame(Track, i, iChan);
			label.Format(LABEL_PATTERN, Track, iPattern, iChan);
			pChunk->StoreReference(label);
			TotalSize += 2;
		}
	}

	m_iTrackFrameSize[Track] = TotalSize;

	iFrameSize = TotalSize;
	iFrameCount = FrameCount;
}

// Patterns

void CCompiler::StorePatterns(unsigned int Track, int &iPatternSize, int &iPatternCount)
{
	/* 
	 * Store patterns and save references to them for the frame list
	 * 
	 */

	int	iChannels = m_pDocument->GetAvailableChannels();
	CPatternCompiler PatternCompiler(m_pDocument, m_iAssignedInstruments, (DPCM_List_t*)&m_iSamplesLookUp, m_pLogger);
	CStringA label;

	iPatternCount = 0;
	iPatternSize = 0;

	// Iterate through all patterns
	for (int i = 0; i < MAX_PATTERN; ++i) {
		for (int j = 0; j < iChannels; ++j) {
			// And store only used ones
			if (IsPatternAddressed(Track, i, j)) {

				label.Format(LABEL_PATTERN, Track, i, j);

				// Compile pattern data
				PatternCompiler.CompileData(Track, i, j);

				bool StoreNew = true;

#ifdef REMOVE_DUPLICATE_PATTERNS
				unsigned int Hash = PatternCompiler.GetHash();
				
				// Check for duplicate patterns
				CChunk *pDuplicate = m_PatternMap[Hash];

				if (pDuplicate != NULL) {
					// Hash only indicates that patterns may be equal, check exact data
					if (PatternCompiler.CompareData(pDuplicate->GetStringData(0), pDuplicate->GetStringLength(0))) {
						// Duplicate was found, store a reference to existing pattern
						m_DuplicateMap[label] = pDuplicate->GetLabel();
						++m_iDuplicatePatterns;
						StoreNew = false;
					}
				}
#endif /* REMOVE_DUPLICATE_PATTERNS */

				if (StoreNew) {
					// Store new pattern
					CChunk *pChunk = CreateChunk(CHUNK_PATTERN, label);
					m_vPatternChunks.push_back(pChunk);

#ifdef REMOVE_DUPLICATE_PATTERNS
					if (m_PatternMap[Hash] != NULL)
						m_iHashCollisions++;
					m_PatternMap[Hash] = pChunk;
#endif /* REMOVE_DUPLICATE_PATTERNS */
					
					// Store pattern data as string
					pChunk->StoreString(PatternCompiler.GetString(), PatternCompiler.GetStringSize());

					iPatternSize += PatternCompiler.GetStringSize();
					++iPatternCount;
				}
			}
		}
	}

#ifdef REMOVE_DUPLICATE_PATTERNS
	// Update references to duplicates
	for (std::vector<CChunk*>::const_iterator it = m_vFrameChunks.begin(); it != m_vFrameChunks.end(); ++it) {
		for (int j = 0; j < (*it)->GetLength(); ++j) {
			CStringA str = m_DuplicateMap[(*it)->GetDataRefName(j)];
			if (str.GetLength() != 0) {
				// Update reference
				(*it)->UpdateDataRefName(j, str);
			}
		}
	}
#endif /* REMOVE_DUPLICATE_PATTERNS */

#ifdef LOCAL_DUPLICATE_PATTERN_REMOVAL
	// Forget patterns when one whole track is stored
	m_PatternMap.RemoveAll();
	m_DuplicateMap.RemoveAll();
#endif /* LOCAL_DUPLICATE_PATTERN_REMOVAL */

}

bool CCompiler::IsPatternAddressed(int Track, int Pattern, int Channel)
{
	// Scan the frame list to see if a pattern is accessed for that frame
	int FrameCount = m_pDocument->GetFrameCount(Track);
	
	for (int i = 0; i < FrameCount; ++i) {
		if (m_pDocument->GetPatternAtFrame(Track, i, Channel) == Pattern)
			return true;
	}

	return false;
}

void CCompiler::AddWavetable(CInstrumentFDS *pInstrument, CChunk *pChunk)
{
	// TODO Find equal existing waves
	/*
	for (int i = 0; i < m_iWaveTables; ++i) {
		if (!memcmp(Wave, m_iWaveTable[i], 64))
			return i;
	}
	*/

	// Allocate new wave
	for (int i = 0; i < 64; ++i)
		pChunk->StoreByte(pInstrument->GetSample(i));

	m_iWaveTables++;
}

// File writing functions

void CCompiler::WriteSamplesAssembly(CFile *pFile)
{
	// Store DPCM samples in file, assembly format
	CStringA str, label;

	str.Format("\n; DPCM samples (located at DPCM segment)\n");
	pFile->Write(str.GetBuffer(), str.GetLength());

	if (m_bBankSwitched == false) {
		str.Format("\n\t.segment \"DPCM\"\n");
		pFile->Write(str.GetBuffer(), str.GetLength());
	}

	unsigned int Address = PAGE_SAMPLES;
	unsigned int TotalSize = 0;

	for (unsigned int i = 0; i < m_vSamples.size(); ++i) {
		CDSample *pDSample = m_vSamples[i];
		label.Format(LABEL_SAMPLE, i);
		str.Format("%s:\n\t.byte ", LPCSTR(label));
		int cntr = 0;
		for (unsigned int j = 0; j < pDSample->SampleSize; ++j) {
			unsigned char c = pDSample->SampleData[j];
			str.AppendFormat("$%02X", c);
			// Insert line breaks
			if (cntr++ == 30 && j < pDSample->SampleSize - 1) {
				str.Append("\n\t.byte ");
				cntr = 0;
			}
			else if (j < pDSample->SampleSize - 1) {
				str.Append(", ");
			}
		}

		Address += pDSample->SampleSize;
		TotalSize += pDSample->SampleSize;

		// Adjust if necessary
		if ((Address & 0x3F) > 0) {
			int PadSize = 0x40 - (Address & 0x3F);
			TotalSize += PadSize;
			Address	  += PadSize;
			str.Append("\n\t.align 64\n");
		}

		str.Append("\n");
		pFile->Write(str.GetBuffer(), str.GetLength());

	}

	Print(_T(" * DPCM size: %i bytes\n"), TotalSize);
}

void CCompiler::WriteSamplesBinary(CFile *pFile)
{
	// Store DPCM samples in file, assembly format
	unsigned int Address = PAGE_SAMPLES;
	unsigned int TotalSize = 0;

	for (std::vector<CDSample*>::iterator it = m_vSamples.begin(); it != m_vSamples.end(); ++it) {
		CDSample *pDSample = *it;

		pFile->Write(pDSample->SampleData, pDSample->SampleSize);

		Address += pDSample->SampleSize;
		TotalSize += pDSample->SampleSize;

		// Adjust size
		if ((Address & 0x3F) > 0) {
			int PadSize = 0x40 - (Address & 0x3F);
			TotalSize += PadSize;
			Address	  += PadSize;
			for (int j = 0; j < PadSize; ++j) {
				char c = 0;
				pFile->Write(&c, 1);
			}
		}
	}

	Print(_T(" * DPCM size: %i bytes\n"), TotalSize);
}

void CCompiler::WriteSamplesToBanks(unsigned int Address)
{
	// Write samples to NSF banks, starting at Address
	// If bankswitching is enabled then starting address will always be $C000

	unsigned int TotalSize = 0;

	if (!m_bBankSwitched) {
		// Set start offset in current bank
		while (m_pCurrentBank->m_iLocation + PAGE_SIZE <= (int)Address) {
			m_pCurrentBank->m_iSize = PAGE_SIZE;
			AllocateBank(m_pCurrentBank->m_iLocation + PAGE_SIZE);
		}
		m_pCurrentBank->m_iSize = Address & (PAGE_SIZE - 1);
	}
	else {
//		while (m_iBanksUsed < m_iFirstSampleBank + 1) {
			// Start on a clean bank
			AllocateBank(PAGE_SAMPLES);
//			m_iFirstSampleBank = m_iBanksUsed - 1;	// TODO this variable is not used after this?
//		}
		/*
		// Start on a clean bank
		AllocateBank(PAGE_SAMPLES);
		m_iFirstSampleBank = m_iBanksUsed - 1;
		*/
	}

	for (std::vector<CDSample*>::iterator it = m_vSamples.begin(); it != m_vSamples.end(); ++it) {
		CDSample *pDSample = *it;

		if (m_bBankSwitched) {
			if (Address + pDSample->SampleSize >= DPCM_SWITCH_ADDRESS) {
				Address = PAGE_SAMPLES;
				// Allocate new bank
				AllocateBank(PAGE_SAMPLES);
			}
		}

		CopyData(pDSample->SampleData, pDSample->SampleSize);

		Address += pDSample->SampleSize;
		TotalSize += pDSample->SampleSize;

		// Adjust size
		unsigned int iAdjust = AdjustSampleAddress(Address);
		if (iAdjust > 0) {
			TotalSize += iAdjust;
			Address	  += iAdjust;
			FillData(0, iAdjust);
		}
	}
}

void CCompiler::WriteAssembly(CFile *pFile)
{
	// Dump all chunks as assembly text

	CChunkRenderText Render;
	Render.StoreChunks(m_vChunks, pFile);
	Print(_T(" * Music data size: %i bytes\n"), m_iMusicDataSize);
}

void CCompiler::WriteBinary(CFile *pFile)
{
	CChunkRenderBinary Render;

	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		Render.StoreChunk(*it, pFile);
	}

	Print(_T(" * Music data size: %i bytes\n"), m_iMusicDataSize);
}

void CCompiler::WriteBinaryBankswitched()
{
	// Store chunks into NSF banks with bankswitching
	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		switch (pChunk->GetType()) {			
			case CHUNK_FRAME_LIST:
			case CHUNK_FRAME:
			case CHUNK_PATTERN:
				// Switchable data
				while (m_iBanksUsed <= pChunk->GetBank() && pChunk->GetBank() > 3)
					AllocateBank(PAGE_BANKED);
				// Write chunk
				if (pChunk->GetType() == CHUNK_PATTERN) {
					CopyData((char*)pChunk->GetStringData(0), pChunk->GetStringLength(0));
				}
				else {
					for (int j = 0; j < pChunk->GetLength(); ++j) {
						unsigned short data = pChunk->GetData(j);
						CopyData((char*)&data, pChunk->GetDataSize(j));
					}
				}
				break;
			default:
				// Write chunk
				for (int j = 0; j < pChunk->GetLength(); ++j) {
					unsigned short data = pChunk->GetData(j);
					CopyData((char*)&data, pChunk->GetDataSize(j));
				}
		}
	}
}

void CCompiler::WriteBinaryFlat()
{
	// Store chunks into NSF banks without bankswitching
	for (std::vector<CChunk*>::iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		// Write chunk
		if (pChunk->GetType() == CHUNK_PATTERN) {
			CopyData((char*)pChunk->GetStringData(0), pChunk->GetStringLength(0));
		}
		else {
			for (int j = 0; j < pChunk->GetLength(); ++j) {
				unsigned short data = pChunk->GetData(j);
				CopyData((char*)&data, pChunk->GetDataSize(j));
			}
		}
	}
}

// Object list functions

CChunk *CCompiler::CreateChunk(chunk_type_t Type, CStringA label)
{
	CChunk *pChunk = new CChunk(Type, label);
	m_vChunks.push_back(pChunk);
	return pChunk;
}

int CCompiler::CountData() const
{
	// Only count data
	int Offset = 0;

	for (std::vector<CChunk*>::const_iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		Offset += (*it)->CountDataSize();
	}

	return Offset;
}

CChunk *CCompiler::GetObjectByRef(CStringA label) const
{
	for (std::vector<CChunk*>::const_iterator it = m_vChunks.begin(); it != m_vChunks.end(); ++it) {
		CChunk *pChunk = *it;
		if (!label.Compare(pChunk->GetLabel()))
			return pChunk;
	}

	return NULL;
}

// NSF banks

void CCompiler::AllocateBank(int Location)
{
	if (m_pCurrentBank)
		m_pCurrentBank->m_iSize = PAGE_SIZE;

	CFileBank *m_pBank = new CFileBank();
	memset(m_pBank->m_data, 0, PAGE_SIZE);
	m_pBank->m_iLocation = Location;
	m_pFileBanks[m_iBanksUsed++] = m_pBank;

	m_pCurrentBank = m_pFileBanks[m_iBanksUsed - 1];
}

void CCompiler::CopyData(char *pData, int iSize)
{
	// Copy data to NSF banks

	int ptr = 0;

	while (iSize > 0) {
		int SpaceAvailable = PAGE_SIZE - m_pCurrentBank->m_iSize;
		if (iSize > SpaceAvailable) {
			memcpy(m_pCurrentBank->m_data + m_pCurrentBank->m_iSize, pData + ptr, SpaceAvailable);
			ptr += SpaceAvailable;
			m_pCurrentBank->m_iSize += SpaceAvailable;
			iSize -= SpaceAvailable;
			// Get new bank
			AllocateBank(m_pCurrentBank->m_iLocation + PAGE_SIZE);
		}
		else {
			memcpy(m_pCurrentBank->m_data + m_pCurrentBank->m_iSize, pData + ptr, iSize);
			m_pCurrentBank->m_iSize += iSize;
			iSize = 0;
		}
	}
}

void CCompiler::FillData(char data, int size)
{
	for (int i = 0; i < size; ++i) {
		CopyData(&data, 1);
	}
}

/*
 * CFileBank implementation
 *
 */

CFileBank::CFileBank() : m_iSize(0), m_iLocation(0), m_iOffset(0) 
{
	memset(m_data, 0, CCompiler::PAGE_SIZE);
}
