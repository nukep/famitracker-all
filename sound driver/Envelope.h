/*
** FamiTracker - NES/Famicom sound tracker
** Copyright (C) 2005-2006  Jonathan Liss
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

#ifndef _ENVELOPE_H_
#define _ENVELOPE_H_

class CEnvelope
{
	public:
		void DoEnvelope() {
			if (--EnvelopeCounter < 1) {
				EnvelopeCounter += EnvelopeSpeed;
				if (!EnvelopeFix) {
					if (Looping)
						EnvelopeVolume = (EnvelopeVolume - 1) & 0x0F;
					else if (EnvelopeVolume > 0)
						EnvelopeVolume--;
				}
			}
		};

	protected:
		uint8	Looping;
		uint8	EnvelopeFix, EnvelopeSpeed, EnvelopeVolume;
		int8	EnvelopeCounter;
};

#endif /* _ENVELOPE_H_ */