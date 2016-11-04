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

#pragma once

typedef unsigned	char		uint8;
typedef unsigned	short		uint16;
typedef unsigned	long		uint32;
typedef unsigned	__int64		uint64;
typedef signed		char		int8;
typedef signed		short		int16;
typedef signed		long		int32;
typedef signed		__int64		int64;

#define SAMPLES_IN_BYTES(x) (x << SampleSizeShift)

const int SPEED_AUTO	= 0;
const int SPEED_NTSC	= 1;
const int SPEED_PAL		= 2;

class ICallback {
public:
	virtual void FlushBuffer(int16 *Buffer, uint32 Size) = 0;
};


// class for simulating CPU memory, used by the DPCM channel
class CSampleMem
{
	public:
		uint8	Read(uint16 Address) {
			if (!Mem)
				return 0;
			int Addr = ((Address & 0xFFFF) - 0xC000) % MemSize;
			return (Mem[Addr]);
		};

		void	SetMem(char *Ptr, int Size) {
			Mem = (uint8*)Ptr;
			MemSize = Size;
		};
	private:
		uint8	*Mem;
		uint16	MemSize;
};
