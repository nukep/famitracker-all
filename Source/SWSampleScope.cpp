#include "stdafx.h"
#include <cmath>
#include "FamiTracker.h"
#include "SWSampleScope.h"

CSWSampleScope::CSWSampleScope(bool bBlur)
{
	m_iCount = 0;
	m_bBlur = bBlur;
}

void CSWSampleScope::Activate()
{
	memset(&bmi, 0, sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize		= sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biBitCount	= 32;
	bmi.bmiHeader.biHeight		= WIN_HEIGHT;
	bmi.bmiHeader.biWidth		= WIN_WIDTH;
	bmi.bmiHeader.biPlanes		= 1;

	m_pBlitBuffer = new int[WIN_WIDTH * WIN_HEIGHT * 2];
	memset(m_pBlitBuffer, 0, WIN_WIDTH * WIN_HEIGHT * sizeof(int));

	m_pWindowBuf = new int[WIN_WIDTH];
	m_iWindowBufPtr = 0;
}

void CSWSampleScope::Deactivate()
{
	delete [] m_pBlitBuffer;
	delete [] m_pWindowBuf;
}

void CSWSampleScope::SetSampleData(int *pSamples, unsigned int iCount)
{
	m_iCount = iCount;
	m_pSamples = pSamples;
}

void CSWSampleScope::Draw(CDC *pDC, bool bMessage)
{
	/*
	int GraphColor		= theApp.m_pSettings->Appearance.iColPatternText;
	int GraphColor2		= DIM(theApp.m_pSettings->Appearance.iColPatternText, 50);
	int GraphBgColor	= theApp.m_pSettings->Appearance.iColBackground;
	*/

	int GraphColor		= 0xFFFFFF;
	int GraphColor2		= DIM(GraphColor, 50);
	int GraphBgColor	= 0x000000;

	unsigned int i = 0;

	if (bMessage)
		return;

	GraphBgColor = ((GraphBgColor & 0xFF) << 16) | (GraphBgColor & 0xFF00) | ((GraphBgColor & 0xFF0000) >> 16);

	while (i < m_iCount) {

		m_pWindowBuf[m_iWindowBufPtr / 7] = m_pSamples[i++];
		m_iWindowBufPtr++;

		if ((m_iWindowBufPtr / 7) > WIN_WIDTH) {
			m_iWindowBufPtr = 0;
			int x = 0;
			int y = 0;
			int s;
			int l;

			l = (m_pWindowBuf[0] / 1000) + (WIN_HEIGHT / 2) - 1;

			for (x = 0; x < WIN_WIDTH; x++) {
			
				s = (m_pWindowBuf[x] / 1000) + (WIN_HEIGHT / 2) - 1;

				for (y = 0; y < WIN_HEIGHT; y++) {
					
					if ((y == s) || ((y > s && y <= l) || (y >= l && y < s))) {
						if (m_bBlur) {
							m_pBlitBuffer[(y + 0) * WIN_WIDTH + x] = 0xFFFFFF;
						}
						else {
							if (y > 2)
								m_pBlitBuffer[(y - 2) * WIN_WIDTH + x] = GraphColor2;
							if (y > 1)
								m_pBlitBuffer[(y - 1) * WIN_WIDTH + x] = GraphColor;
							m_pBlitBuffer[(y + 0) * WIN_WIDTH + x] = GraphColor2;
						}
					}
					else {
						if (m_bBlur) {
							if (y > 1 && y < (WIN_HEIGHT - 1) && x > 0 && x < (WIN_WIDTH - 1)) {
								const int BLUR_DECAY = 13;
								int Col1 = m_pBlitBuffer[(y + 1) * WIN_WIDTH + x];
								int Col2 = m_pBlitBuffer[(y - 1) * WIN_WIDTH + x];
								int Col3 = m_pBlitBuffer[y * WIN_WIDTH + (x + 1)];
								int Col4 = m_pBlitBuffer[y * WIN_WIDTH + (x - 1)];
								int Col5 = m_pBlitBuffer[(y - 1) * WIN_WIDTH + (x + 1)];
								int Col6 = m_pBlitBuffer[(y - 1) * WIN_WIDTH + (x - 1)];
								int Col7 = m_pBlitBuffer[(y + 1) * WIN_WIDTH + (x + 1)];
								int Col8 = m_pBlitBuffer[(y + 1) * WIN_WIDTH + (x - 1)];

								int r = ((Col1 >> 16) & 0xFF) + ((Col2 >> 16) & 0xFF) + ((Col3 >> 16) & 0xFF) + 
									((Col4 >> 16) & 0xFF) + ((Col5 >> 16) & 0xFF) + ((Col6 >> 16) & 0xFF) + 
									((Col7 >> 16) & 0xFF) + ((Col8 >> 16) & 0xFF);

								int g = ((Col1 >> 8) & 0xFF) + ((Col2 >> 8) & 0xFF) + ((Col3 >> 8) & 0xFF) + 
									((Col4 >> 8) & 0xFF) + ((Col5 >> 8) & 0xFF) + ((Col6 >> 8) & 0xFF) + 
									((Col7 >> 8) & 0xFF) + ((Col8 >> 8) & 0xFF);

								int b = ((Col1) & 0xFF) + ((Col2) & 0xFF) + ((Col3) & 0xFF) + 
									((Col4) & 0xFF) + ((Col5) & 0xFF) + ((Col6) & 0xFF) + 
									((Col7) & 0xFF) + ((Col8) & 0xFF);

								r = r / 8 - BLUR_DECAY / 1;
								g = g / 8 - BLUR_DECAY / 1;
								b = b / 8 - BLUR_DECAY / 2;

								if (r < 0) r = 0;
								if (g < 0) g = 0;
								if (b < 0) b = 0;

								m_pBlitBuffer[y * WIN_WIDTH + x] = (r << 16) + (g << 8) + b;
								//BlitBuffer[y * WIN_WIDTH + x] = DIM(BlitBuffer[y * WIN_WIDTH + x], 70);
							}
							else {
								m_pBlitBuffer[y * WIN_WIDTH + x] = GraphBgColor;
							}
						}
						else {
							//BlitBuffer[y * WIN_WIDTH + x] = GraphBgColor;
							m_pBlitBuffer[y * WIN_WIDTH + x] = DIM(GraphColor, (int)(sinf( (((y * 100) / WIN_HEIGHT) * 2 * 3.14f) / 100 + 1.8f) * 15 + 15));
						}
					}
				}
				l = s;
			}
			StretchDIBits(*pDC, 0, 0, WIN_WIDTH, WIN_HEIGHT, 0, 0, WIN_WIDTH, WIN_HEIGHT, m_pBlitBuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
		}
	}	
}