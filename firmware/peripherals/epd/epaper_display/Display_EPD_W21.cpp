#include "Display_EPD_W21_spi.h"
#include "Display_EPD_W21.h"
#include <zephyr/kernel.h>
#include <stdint.h>

void delay_xms(unsigned int xms)
{
	k_msleep(xms);
}

// Reset the RAM X/Y address counters to the origin (0,0). Every RAM stream must do
// this so full refreshes, the partial base map, and the 4-gray planes all start from
// the SAME origin — otherwise a render inherits whatever counter the previous update
// left and lands 1-2px off (the "shift after first render" bug). These are exactly the
// bytes EPD_Display_Partial already relies on.
static void EPD_ResetRamCounter(void)
{
	Epaper_Write_Command(0x4E); // RAM x address counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Command(0x4F); // RAM y address counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x00);
}

void EPD_Display(unsigned char *Image)
{
	uint8_t tempData;
	uint32_t length;

	uint8_t tempOriginal;
	uint32_t tempcol = 1;
	uint32_t templine = 0;
	unsigned int width, height, i, j;
	width = (EPD_WIDTH % 8 == 0) ? (EPD_WIDTH / 8) : (EPD_WIDTH / 8 + 1);
	height = EPD_HEIGHT;
	length = width * height;
	EPD_ResetRamCounter(); // deterministic origin so re-renders don't drift 1-2px
	EPD_W21_WriteCMD(0x24);
	for (i = 0; i < length; i++)
	{
		tempData = Image[length - (width * tempcol - i % width)];
		templine++;
		if (templine >= width)
		{
			tempcol++;
			templine = 0;
		}
		Epaper_Write_Data(tempData);
	}
	EPD_Update();
}

// Writes Image to the just-selected RAM bank using the SAME row-reversal as
// EPD_Display, so a framebuffer authored for EPD_Display keeps its orientation.
// (EPD_SetRAMValue_BaseMap / EPD_Dis_Part write linearly and would flip it.)
static void Epaper_Write_RowReversed(const unsigned char *Image)
{
	const unsigned int width = (EPD_WIDTH % 8 == 0) ? (EPD_WIDTH / 8) : (EPD_WIDTH / 8 + 1);
	const unsigned int length = width * EPD_HEIGHT;
	unsigned int tempcol = 1, templine = 0;

	for (unsigned int i = 0; i < length; i++) {
		Epaper_Write_Data(Image[length - (width * tempcol - i % width)]);
		if (++templine >= width) {
			tempcol++;
			templine = 0;
		}
	}
}

// Establishes the partial-refresh baseline: writes Image (row-reversed) into BOTH
// RAM banks (0x24 new, 0x26 old) and does one full refresh. Call before a run of
// EPD_Display_Partial() updates.
void EPD_SetBaseMap(const unsigned char *Image)
{
	EPD_ResetRamCounter(); // deterministic origin (shared with EPD_Display) so renders align
	Epaper_Write_Command(0x24);
	Epaper_Write_RowReversed(Image);
	EPD_ResetRamCounter();
	Epaper_Write_Command(0x26);
	Epaper_Write_RowReversed(Image);
	EPD_Update();
}

// Fast, flash-free differential update: writes Image (row-reversed) to bank 0x24
// and runs the partial waveform. Requires a prior EPD_SetBaseMap().
void EPD_Display_Partial(const unsigned char *Image)
{
	// Re-enter partial mode the way the vendor's EPD_Dis_Part does: a reset pulse +
	// the partial BorderWaveform (0x3C=0x80) + reset the RAM address counters.
	// Without this, the SSD1680 runs the partial waveform with the full-refresh
	// border/VCOM still configured and washes the whole panel out on the FIRST
	// partial (it does NOT accumulate over many refreshes — it's a missing setup).
	EPD_W21_RST_0; // Module reset
	delay_xms(10);
	EPD_W21_RST_1;
	delay_xms(10);

	Epaper_Write_Command(0x3C); // BorderWaveform
	Epaper_Write_Data(0x80);    // partial-mode border

	Epaper_Write_Command(0x4E); // RAM x address counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Command(0x4F); // RAM y address counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x00);

	Epaper_Write_Command(0x24);
	Epaper_Write_RowReversed(Image); // same packing as EPD_Display/EPD_SetBaseMap
	EPD_Part_Update();
}

void Epaper_Spi_WriteByte(unsigned char TxData)
{
	SPI_Write(TxData);
}

void Epaper_READBUSY(void)
{
	// Poll with a 1 ms yield rather than a tight spin: a refresh holds BUSY for
	// hundreds of ms (partial) to ~2 s (full), and busy-spinning would starve the
	// LED render thread (causing visible animation stutter) and waste power.
	//
	// Bound the wait (~3 s, above the ~2 s worst-case full refresh) and return on
	// timeout rather than spinning forever: if two threads ever drive the panel
	// concurrently (a regression) or BUSY genuinely sticks, an unbounded wait here
	// hangs the calling thread permanently — which, on the main thread, kills the
	// user button. A bounded wait turns that into a transient, self-recovering error.
	uint32_t start = k_uptime_get_32();
	while (isEPD_W21_BUSY != 0)
	{ //=1 BUSY
		if (k_uptime_get_32() - start >= 3000) {
			printk("EPD: BUSY wait timed out\n");
			return;
		}
		k_msleep(1);
	}
}

void Epaper_Write_Command(unsigned char cmd)
{
	EPD_W21_CS_1;
	EPD_W21_CS_0;
	EPD_W21_DC_0; // D/C#   0:command  1:data

	Epaper_Spi_WriteByte(cmd);
	EPD_W21_CS_1;
}

void Epaper_Write_Data(unsigned char data)
{
	EPD_W21_CS_1;
	EPD_W21_CS_0;
	EPD_W21_DC_1; // D/C#   0:command  1:data

	Epaper_Spi_WriteByte(data);
	EPD_W21_CS_1;
}

/////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////////////////
// SSD1680A
void EPD_HW_Init(void)
{
	EPD_W21_RST_0; // Module reset
	delay_xms(10); // At least 10ms delay
	EPD_W21_RST_1;
	delay_xms(10); // At least 10ms delay

	Epaper_READBUSY();
	Epaper_Write_Command(0x12); // SWRESET
	Epaper_READBUSY();
}
void EPD_HW_Init_Fast(void)
{
	EPD_W21_RST_0; // Module reset
	delay_xms(10); // At least 10ms delay
	EPD_W21_RST_1;
	delay_xms(10); // At least 10ms delay

	Epaper_Write_Command(0x12); // SWRESET
	Epaper_READBUSY();

	Epaper_Write_Command(0x18); // Read built-in temperature sensor
	Epaper_Write_Data(0x80);

	Epaper_Write_Command(0x22); // Load temperature value
	Epaper_Write_Data(0xB1);
	Epaper_Write_Command(0x20);
	Epaper_READBUSY();

	Epaper_Write_Command(0x1A); // Write to temperature register
	Epaper_Write_Data(0x64);
	Epaper_Write_Data(0x00);

	Epaper_Write_Command(0x22); // Load temperature value
	Epaper_Write_Data(0x91);
	Epaper_Write_Command(0x20);
	Epaper_READBUSY();
}

/////////////////////////////////////////////////////////////////////////////////////////
// 4-level grayscale support (SSD1680). The panel ships as B/W (OTP waveform only),
// but the controller can render 4 grays from a custom 153-byte waveform LUT loaded
// via command 0x32 plus two bitplanes written to RAM 0x26 (MSB) and 0x24 (LSB).
// LUT + voltages are adapted from the GxEPD2_4G SSD1680 reference (GDEY0213B74) —
// same controller, different resolution. May need on-hardware tuning for this panel.
static const unsigned char lut_4Gray[153] = {
	0x40, 0x48, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x08, 0x48, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x02, 0x48, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x20, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x0A, 0x19, 0x00, 0x03, 0x08, 0x00, 0x00,
	0x14, 0x01, 0x00, 0x14, 0x01, 0x00, 0x03,
	0x0A, 0x03, 0x00, 0x08, 0x19, 0x00, 0x00,
	0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x00, 0x00, 0x00,
};

void EPD_Init_4Gray(void)
{
	unsigned int i;

	EPD_W21_RST_0; // Module reset
	delay_xms(10);
	EPD_W21_RST_1;
	delay_xms(10);

	Epaper_READBUSY();
	Epaper_Write_Command(0x12); // SWRESET
	Epaper_READBUSY();

	Epaper_Write_Command(0x01); // Driver output control: 264 gate lines (264-1=0x107)
	Epaper_Write_Data(0x07);
	Epaper_Write_Data(0x01);
	Epaper_Write_Data(0x00);

	Epaper_Write_Command(0x11); // Data entry mode: X+ Y+
	Epaper_Write_Data(0x03);

	// Full-panel RAM window so each 5808-byte plane write wraps back to (0,0) —
	// the same behaviour EPD_SetBaseMap relies on when it fills both banks.
	Epaper_Write_Command(0x44); // RAM X start/end (bytes): 0..21
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x15);
	Epaper_Write_Command(0x45); // RAM Y start/end (lines): 0..263
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x07);
	Epaper_Write_Data(0x01);
	Epaper_Write_Command(0x4E); // RAM X counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Command(0x4F); // RAM Y counter = 0
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x00);

	// 4-gray analog/digital blocks + drive voltages (reference values).
	// --- Grain-tuning knobs (this panel isn't rated for gray; iterate on hardware) ---
	//   VCOM (0x2C=0x1C): centers the gray; nudge a few counts if mid-grays are blotchy.
	//   VSH1/VSH2/VSL (0x04): drive strength; stronger drive develops gray more uniformly.
	//   lut_4Gray group-timing rows (the 0x0A,0x19,... section): longer gray phases let the
	//     particles settle — the most impactful knob on the residual "X" grain.
	Epaper_Write_Command(0x74);
	Epaper_Write_Data(0x54);
	Epaper_Write_Command(0x7E);
	Epaper_Write_Data(0x3B);
	Epaper_Write_Command(0x2C); // VCOM
	Epaper_Write_Data(0x1C);
	Epaper_Write_Command(0x3F); // EOPQ (LUT tail)
	Epaper_Write_Data(0x22);
	Epaper_Write_Command(0x03); // VGH
	Epaper_Write_Data(0x17);
	Epaper_Write_Command(0x04); // VSH1, VSH2, VSL
	Epaper_Write_Data(0x41);
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x32);
	Epaper_Write_Command(0x3C); // BorderWaveform
	Epaper_Write_Data(0x00);
	Epaper_Write_Command(0x21); // Display update control 1 (use both RAM banks)
	Epaper_Write_Data(0x00);
	Epaper_Write_Data(0x80);

	Epaper_Write_Command(0x32); // Write the 4-gray waveform LUT
	for (i = 0; i < sizeof(lut_4Gray); i++) {
		Epaper_Write_Data(lut_4Gray[i]);
	}
	Epaper_READBUSY();
}

// Write one 5808-byte bitplane to the given RAM bank (0x24 or 0x26), reusing the
// same row-reversal as EPD_Display so a plane built with the framebuffer layout
// keeps its orientation. No update is triggered (call EPD_Update_4Gray after both).
void EPD_WritePlane(unsigned char ram_cmd, const unsigned char *plane)
{
	EPD_ResetRamCounter(); // start each plane at origin so 0x26 (MSB) and 0x24 (LSB) align
	Epaper_Write_Command(ram_cmd);
	Epaper_Write_RowReversed(plane);
}

// 4-gray refresh: 0x22=0xC7 keeps the LUT we loaded via 0x32 (unlike EPD_Update's
// 0xF7, which would reload the OTP waveform and clobber it).
void EPD_Update_4Gray(void)
{
	Epaper_Write_Command(0x22);
	Epaper_Write_Data(0xC7);
	Epaper_Write_Command(0x20);
	Epaper_READBUSY();
}

// Switch the controller back to the B/W waveform after a 4-gray render, so the gray
// waveform is only ever active while a grayscale image is on screen. EPD_Init_4Gray
// leaves custom analog config set (VCOM 0x2C, VGH 0x03, VSH/VSL 0x04, border 0x3C,
// update-control 0x21, gray LUT 0x32); without this, the next B/W partial refresh
// (e.g. the config countdown) would run on the gray waveform and scramble. The HW
// reset + SWRESET restores those parameters to their defaults while preserving RAM
// and the bistable image on the panel (no display-update is issued here, so the gray
// image stays up). The next B/W refresh reloads the OTP LUT, dropping the gray LUT.
void EPD_Restore_BW(void)
{
	EPD_HW_Init();
}

// Fill RAM bank 0x24 with a constant byte and full-refresh. 0xFF = white, 0x00 = black.
static void EPD_FullFill(unsigned char value)
{
	unsigned int i;

	Epaper_Write_Command(0x24);
	for (i = 0; i < 5808; i++) {
		Epaper_Write_Data(value);
	}
	EPD_Update();
}

// Condition the panel before a 4-gray render. The gray LUT is a "from white"
// waveform, so every pixel must start from a uniform saturated state or the gray
// develops unevenly and leaves a structured residual ("tiny X" grain). A
// white->black->white wipe exercises every pixel and clears residual charge; pass
// a lighter strength to trade thoroughness for speed/flashing:
//   0 = none, 1 = single white flush, 2 = white->black->white.
void EPD_Condition_4Gray(unsigned char strength)
{
	if (strength == 0) {
		return;
	}
	EPD_HW_Init(); // OTP B/W waveform for the clear refreshes
	if (strength >= 2) {
		EPD_FullFill(0xFF); // white
		EPD_FullFill(0x00); // black
	}
	EPD_FullFill(0xFF); // white (uniform start state for the gray waveform)
}

/////////////////////////////////////////////////////////////////////////////////////////
/*When the electronic paper screen is updated, do not unplug the electronic paper to avoid damage to the screen*/

void EPD_Update(void)
{
	Epaper_Write_Command(0x22); // Display Update Control
	Epaper_Write_Data(0xF7);
	Epaper_Write_Command(0x20); // Activate Display Update Sequence
	Epaper_READBUSY();
}
void EPD_Update_Fast(void)
{
	Epaper_Write_Command(0x22); // Display Update Control
	Epaper_Write_Data(0xC7);
	Epaper_Write_Command(0x20); // Activate Display Update Sequence
	Epaper_READBUSY();
}
/*When the electronic paper screen is updated, do not unplug the electronic paper to avoid damage to the screen*/
void EPD_Part_Update(void)
{
	Epaper_Write_Command(0x22); // Display Update Control
	Epaper_Write_Data(0xFF);
	Epaper_Write_Command(0x20); // Activate Display Update Sequence
	Epaper_READBUSY();
}
//////////////////////////////All screen update////////////////////////////////////////////
void EPD_WhiteScreen_ALL(const unsigned char *datas)
{
	unsigned int i;
	Epaper_Write_Command(0x24); // write RAM for black(0)/white (1)
	for (i = 0; i < 5808; i++)
	{
		Epaper_Write_Data(*datas);
		datas++;
	}
	EPD_Update();
}
void EPD_WhiteScreen_ALL_Fast(const unsigned char *datas)
{
	unsigned int i;
	Epaper_Write_Command(0x24); // write RAM for black(0)/white (1)
	for (i = 0; i < 5808; i++)
	{
		Epaper_Write_Data(*datas);
		datas++;
	}

	EPD_Update_Fast();
}
///////////////////////////Part update//////////////////////////////////////////////
// The x axis is reduced by one byte, and the y axis is reduced by one pixel.
void EPD_SetRAMValue_BaseMap(const unsigned char *datas)
{
	unsigned int i;
	const unsigned char *datas_flag;
	datas_flag = datas;

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < 5808; i++)
	{
		Epaper_Write_Data(*datas);
		datas++;
	}
	datas = datas_flag;
	Epaper_Write_Command(0x26); // Write Black and White image to RAM
	for (i = 0; i < 5808; i++)
	{
		Epaper_Write_Data(*datas);
		datas++;
	}
	EPD_Update();
}
void EPD_Dis_Part(unsigned int x_start, unsigned int y_start, const unsigned char *datas, unsigned int PART_COLUMN, unsigned int PART_LINE)
{
	unsigned int i;
	unsigned int x_end, y_start1, y_start2, y_end1, y_end2;
	x_start = x_start / 8;
	x_end = x_start + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_start;
	if (y_start >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_start + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}
	// Reset
	EPD_W21_RST_0; // Module reset
	delay_xms(10); // At least 10ms delay
	EPD_W21_RST_1;
	delay_xms(10); // At least 10ms delay

	Epaper_Write_Command(0x3C); // BorderWavefrom
	Epaper_Write_Data(0x80);
	//
	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_start);	 // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);	 // ????=0

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_start);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datas);
		datas++;
	}
	EPD_Part_Update();
}

void EPD_DeepSleep(void)
{
	Epaper_Write_Command(0x10); // enter deep sleep
	Epaper_Write_Data(0x01);
	delay_xms(100);
}

/////////////////////////////////Single display////////////////////////////////////////////////

void EPD_WhiteScreen_White(void)

{
	unsigned int i, k;
	Epaper_Write_Command(0x24); // write RAM for black(0)/white (1)
	for (k = 0; k < 264; k++)
	{
		for (i = 0; i < 22; i++)
		{
			Epaper_Write_Data(0xff);
		}
	}

	EPD_Update();
}

/////////////////////////////////////TIME///////////////////////////////////////////////////
void EPD_Dis_Part_myself(unsigned int x_startA, unsigned int y_startA, const unsigned char *datasA,
						 unsigned int x_startB, unsigned int y_startB, const unsigned char *datasB,
						 unsigned int x_startC, unsigned int y_startC, const unsigned char *datasC,
						 unsigned int x_startD, unsigned int y_startD, const unsigned char *datasD,
						 unsigned int x_startE, unsigned int y_startE, const unsigned char *datasE,
						 unsigned int PART_COLUMN, unsigned int PART_LINE)
{
	unsigned int i;
	unsigned int x_end, y_start1, y_start2, y_end1, y_end2;

	// Data A////////////////////////////
	x_startA = x_startA / 8; // Convert to byte
	x_end = x_startA + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_startA - 1;
	if (y_startA >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_startA + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}
	// Reset
	EPD_W21_RST_0; // Module reset
	delay_xms(10); // At least 10ms delay
	EPD_W21_RST_1;
	delay_xms(10); // At least 10ms delay

	Epaper_Write_Command(0x3C); // BorderWavefrom
	Epaper_Write_Data(0x80);
	//
	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_startA); // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_startA);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datasA);
		datasA++;
	}
	// Data B/////////////////////////////////////
	x_startB = x_startB / 8; // Convert to byte
	x_end = x_startB + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_startB - 1;
	if (y_startB >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_startB + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}

	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_startB); // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_startB);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datasB);
		datasB++;
	}

	// Data C//////////////////////////////////////
	x_startC = x_startC / 8; // Convert to byte
	x_end = x_startC + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_startC - 1;
	if (y_startC >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_startC + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}

	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_startC); // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_startC);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datasC);
		datasC++;
	}

	// Data D//////////////////////////////////////
	x_startD = x_startD / 8; // Convert to byte
	x_end = x_startD + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_startD - 1;
	if (y_startD >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_startD + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}

	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_startD); // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_startD);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datasD);
		datasD++;
	}
	// Data E//////////////////////////////////////
	x_startE = x_startE / 8; // Convert to byte
	x_end = x_startE + PART_LINE / 8 - 1;

	y_start1 = 0;
	y_start2 = y_startE - 1;
	if (y_startE >= 256)
	{
		y_start1 = y_start2 / 256;
		y_start2 = y_start2 % 256;
	}
	y_end1 = 0;
	y_end2 = y_startE + PART_COLUMN - 1;
	if (y_end2 >= 256)
	{
		y_end1 = y_end2 / 256;
		y_end2 = y_end2 % 256;
	}

	Epaper_Write_Command(0x44);	 // set RAM x address start/end, in page 35
	Epaper_Write_Data(x_startE); // RAM x address start at 00h;
	Epaper_Write_Data(x_end);	 // RAM x address end at 0fh(15+1)*8->128
	Epaper_Write_Command(0x45);	 // set RAM y address start/end, in page 35
	Epaper_Write_Data(y_start2); // RAM y address start at 0127h;
	Epaper_Write_Data(y_start1); // RAM y address start at 0127h;
	Epaper_Write_Data(y_end2);	 // RAM y address end at 00h;
	Epaper_Write_Data(y_end1);

	Epaper_Write_Command(0x4E); // set RAM x address count to 0;
	Epaper_Write_Data(x_startE);
	Epaper_Write_Command(0x4F); // set RAM y address count to 0X127;
	Epaper_Write_Data(y_start2);
	Epaper_Write_Data(y_start1);

	Epaper_Write_Command(0x24); // Write Black and White image to RAM
	for (i = 0; i < PART_COLUMN * PART_LINE / 8; i++)
	{
		Epaper_Write_Data(*datasE);
		datasE++;
	}
	EPD_Part_Update();
}

/***********************************************************
						end file
***********************************************************/
