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

#include "stdafx.h"
#include "FamiTracker.h"
#include "SampleWindow.h"
#include "..\fft\fft.h"
#include "resource.h"

enum {GRAPH, GRAPH_BLUR, FFT, LOGO};

// CSampleWindow

IMPLEMENT_DYNAMIC(CSampleWindow, CWnd)
CSampleWindow::CSampleWindow()
{
}

CSampleWindow::~CSampleWindow()
{
}


BEGIN_MESSAGE_MAP(CSampleWindow, CWnd)
	ON_WM_ERASEBKGND()
	ON_WM_LBUTTONDOWN()
	ON_WM_PAINT()
END_MESSAGE_MAP()

int *WndBuf;
int WndBufPtr;
bool FFT_Active = true;

int m_iStyle = GRAPH;

const int FFT_POINTS = 256;

Fft FftObject(FFT_POINTS, 44100);

int FftPoint[FFT_POINTS];

// CSampleWinProc message handlers

BOOL CSampleWinProc::InitInstance()
{
	for (int i = 0; i < FFT_POINTS; i++)
		FftPoint[i] = 0;
	//return CWinThread::InitInstance();
	return TRUE;
}

int CSampleWinProc::Run()
{
	bool Running = true;

	MSG msg;

	while (Running) {
		while (GetMessage(&msg, NULL, 0, 0)) {
			if (msg.message == WM_USER) {
				Wnd->DrawSamples((int*)msg.wParam, (int)msg.lParam);
			}
		}
	}

	return CWinThread::Run();
}


// CSampleWindow message handlers

#define USE_BLUR 1

void CSampleWindow::DrawSamples(int *Samples, int Count)
{
	//this was made quickly and dirty

	if (!Active)
		return;

	if (m_hWnd == NULL)
		return;

	CDC *pDC = GetDC();

	switch (m_iStyle) {
		case GRAPH:
			DrawGraph(Samples, Count, pDC);
			break;
		case GRAPH_BLUR:
			DrawGraph(Samples, Count, pDC);
			break;
		case FFT:
			DrawFFT(Samples, Count, pDC);
			break;
		case LOGO:
			break;
	}

	ReleaseDC(pDC);

	delete [] Samples;
}

void CSampleWindow::DrawFFT(int *Samples, int Count, CDC *pDC)
{
	int i = 0, y, bar;

	while (i < Count) {
		if ((Count - i) > FFT_POINTS) {
			FftObject.CopyIn(FFT_POINTS, Samples);
			FftObject.Transform();
			i += FFT_POINTS;
		}
		else {
			FftObject.CopyIn((Count - i), Samples);
			FftObject.Transform();
			i = Count;
		}
	}

	float Stepping = (float)(FFT_POINTS - (FFT_POINTS / 2) - 40) / (float)WIN_WIDTH;
	float Step = 0;

	for (i = 0; i < WIN_WIDTH; i++) {
		// skip the first 20 Hzs
		bar = (int)FftObject.GetIntensity(int(Step) + 20) / 80;

		if (bar > WIN_HEIGHT)
			bar = WIN_HEIGHT;

		if (bar > FftPoint[(int)Step]) {
			FftPoint[(int)Step] = bar;
		}
		else {
			FftPoint[(int)Step] -= 1;
		}

		bar = FftPoint[(int)Step];

		for (y = 0; y < WIN_HEIGHT; y++) {
			if (y < bar)
				BlitBuffer[(WIN_HEIGHT - y) * WIN_WIDTH + i] = 0xF0F0F0 - (DIM(0xFFFF80, (y * 100) / bar) + 0x80) + 0x282888;
			else
				BlitBuffer[(WIN_HEIGHT - y) * WIN_WIDTH + i] = 0x000000;
		}

		Step += Stepping;
	}

	StretchDIBits(*pDC, 0, 0, WIN_WIDTH, WIN_HEIGHT, 0, 0, WIN_WIDTH, WIN_HEIGHT, BlitBuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

void CSampleWindow::DrawGraph(int *Samples, int Count, CDC *pDC)
{
//	CDC *pDC = GetDC();
	
	int GraphColor		= theApp.m_pSettings->Appearance.iColPatternText;
	int GraphColor2		= DIM(theApp.m_pSettings->Appearance.iColPatternText, 50);
	int GraphBgColor	= theApp.m_pSettings->Appearance.iColBackground;

	int i = 0;

	GraphBgColor = ((GraphBgColor & 0xFF) << 16) | (GraphBgColor & 0xFF00) | ((GraphBgColor & 0xFF0000) >> 16);

	while (i < Count) {
		WndBuf[WndBufPtr / 7] = Samples[i++];
		WndBufPtr++;

		if ((WndBufPtr / 7) > WIN_WIDTH) {
			WndBufPtr = 0;
			int x = 0;
			int y = 0;
			int s;
			int l;

			l = (WndBuf[0] / 1000) + (WIN_HEIGHT / 2) - 1;

			for (x = 0; x < WIN_WIDTH; x++) {
			
				s = (WndBuf[x] / 1000) + (WIN_HEIGHT / 2) - 1;

				for (y = 0; y < WIN_HEIGHT; y++) {
					
					if ((y == s) || ((y > s && y <= l) || (y >= l && y < s))) {
						if (m_iStyle == GRAPH_BLUR) {
							BlitBuffer[(y + 0) * WIN_WIDTH + x] = 0xFFFFFF;
						}
						else {
							if (y > 2)
								BlitBuffer[(y - 2) * WIN_WIDTH + x] = /*0x508040*/ GraphColor2;
							if (y > 1)
								BlitBuffer[(y - 1) * WIN_WIDTH + x] = /*0x80F070*/ GraphColor;
							BlitBuffer[(y + 0) * WIN_WIDTH + x] = /*0x508040*/ GraphColor2;
						}
					}
					else {
						if (m_iStyle == GRAPH_BLUR) {
							if (y > 1 && y < (WIN_HEIGHT - 1) && x > 0 && x < (WIN_WIDTH - 1)) {
								const int BLUR_DECAY = 13;
								int Col1 = BlitBuffer[(y + 1) * WIN_WIDTH + x];
								int Col2 = BlitBuffer[(y - 1) * WIN_WIDTH + x];
								int Col3 = BlitBuffer[y * WIN_WIDTH + (x + 1)];
								int Col4 = BlitBuffer[y * WIN_WIDTH + (x - 1)];
								int Col5 = BlitBuffer[(y - 1) * WIN_WIDTH + (x + 1)];
								int Col6 = BlitBuffer[(y - 1) * WIN_WIDTH + (x - 1)];
								int Col7 = BlitBuffer[(y + 1) * WIN_WIDTH + (x + 1)];
								int Col8 = BlitBuffer[(y + 1) * WIN_WIDTH + (x - 1)];

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

								BlitBuffer[y * WIN_WIDTH + x] = (r << 16) + (g << 8) + b;
								//BlitBuffer[y * WIN_WIDTH + x] = DIM(BlitBuffer[y * WIN_WIDTH + x], 70);
							}
							else
								BlitBuffer[y * WIN_WIDTH + x] = GraphBgColor;
						}
						else {
							BlitBuffer[y * WIN_WIDTH + x] = GraphBgColor;
						}
					}
				}
				l = s;
			}

			StretchDIBits(*pDC, 0, 0, WIN_WIDTH, WIN_HEIGHT, 0, 0, WIN_WIDTH, WIN_HEIGHT, BlitBuffer, &bmi, DIB_RGB_COLORS, SRCCOPY);
		}
	}
}

BOOL CSampleWindow::OnEraseBkgnd(CDC* pDC)
{
	if (m_iStyle == LOGO) {
		CBitmap Bmp, *OldBmp;
		CDC		BitmapDC;

		Bmp.LoadBitmap(IDB_SAMPLEBG);
		BitmapDC.CreateCompatibleDC(pDC);
		OldBmp = BitmapDC.SelectObject(&Bmp);

		pDC->BitBlt(0, 0, WIN_WIDTH, WIN_HEIGHT, &BitmapDC, 0, 0, SRCCOPY);

		BitmapDC.SelectObject(OldBmp);
	}

	return FALSE;
}

BOOL CSampleWindow::CreateEx(DWORD dwExStyle, LPCTSTR lpszClassName, LPCTSTR lpszWindowName, DWORD dwStyle, const RECT& rect, CWnd* pParentWnd, UINT nID, CCreateContext* pContext)
{
	memset(&bmi, 0, sizeof(BITMAPINFO));
	bmi.bmiHeader.biSize		= sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biBitCount	= 32;
	bmi.bmiHeader.biHeight		= -WIN_HEIGHT;
	bmi.bmiHeader.biWidth		= WIN_WIDTH;
	bmi.bmiHeader.biPlanes		= 1;

	BlitBuffer = new int[WIN_WIDTH * WIN_HEIGHT * 2];

	memset(BlitBuffer, 0, WIN_WIDTH * WIN_HEIGHT * sizeof(int));

	Active = true;
	Blur = false;

	WndBufPtr = 0;
	WndBuf = new int[WIN_WIDTH];

	return CWnd::CreateEx(dwExStyle, lpszClassName, lpszWindowName, dwStyle, rect, pParentWnd, nID, pContext);
}

void CSampleWindow::OnLButtonDown(UINT nFlags, CPoint point)
{
	switch (m_iStyle) {
		case GRAPH: 
			m_iStyle = GRAPH_BLUR;
			break;
		case GRAPH_BLUR: 
			m_iStyle = FFT;
			break;
		case FFT: 
			m_iStyle = LOGO;
			RedrawWindow();
			break;
		case LOGO: 
			m_iStyle = GRAPH;
			break;
	}

	CWnd::OnLButtonDown(nFlags, point);
}

void CSampleWindow::OnPaint()
{
	CPaintDC dc(this); // device context for painting
	
	if (m_iStyle == LOGO) {
		CBitmap Bmp, *OldBmp;
		CDC		BitmapDC;

		Bmp.LoadBitmap(IDB_SAMPLEBG);
		BitmapDC.CreateCompatibleDC(&dc);
		OldBmp = BitmapDC.SelectObject(&Bmp);

		dc.BitBlt(0, 0, WIN_WIDTH, WIN_HEIGHT, &BitmapDC, 0, 0, SRCCOPY);

		BitmapDC.SelectObject(OldBmp);
	}
}

