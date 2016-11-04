/*
** FamiTracker - NES/Famicom sound tracker
** Copyright (C) 2005-2007  Jonathan Liss
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

#ifndef _WAVEFILE_H_
#define _WAVEFILE_H_

#include <mmsystem.h>

class CWaveFile
{
	public:
		bool	OpenFile(char *Filename, int SampleRate, int SampleSize, int Channels);
		void	CloseFile();
		void	WriteWave(char *Data, int Size);

	private:
		PCMWAVEFORMAT	WaveFormat;
		MMCKINFO		ckOutRIFF, ckOut;
		MMIOINFO		mmioinfoOut;
		HMMIO			hmmioOut;

};

#endif /* _WAVEFILE_H_ */