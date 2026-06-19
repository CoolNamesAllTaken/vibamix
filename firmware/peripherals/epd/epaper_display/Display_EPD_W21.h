#ifndef _DISPLAY_EPD_W21_H_
#define _DISPLAY_EPD_W21_H_


#define EPD_WIDTH   176
#define EPD_HEIGHT  264

//EPD
void Epaper_READBUSY(void);
void Epaper_Spi_WriteByte(unsigned char TxData);
void Epaper_Write_Command(unsigned char cmd);
void Epaper_Write_Data(unsigned char data);


void Epaper_HW_SW_RESET(void);
void EPD_HW_Init(void); //Electronic paper initialization
void EPD_Update(void);

void EPD_Part_Init(void);//Local refresh initialization
void EPD_Part_Update(void); 

void EPD_WhiteScreen_White(void);
void EPD_DeepSleep(void);
//Display 
void EPD_WhiteScreen_ALL(const unsigned char *datas);
void EPD_SetRAMValue_BaseMap(const unsigned char * datas);
void EPD_Dis_Part(unsigned int x_start,unsigned int y_start,const unsigned char * datas,unsigned int PART_COLUMN,unsigned int PART_LINE);
void EPD_Dis_Part_myself(unsigned int x_startA,unsigned int y_startA,const unsigned char * datasA,
	                       unsigned int x_startB,unsigned int y_startB,const unsigned char * datasB,
												 unsigned int x_startC,unsigned int y_startC,const unsigned char * datasC,
												 unsigned int x_startD,unsigned int y_startD,const unsigned char * datasD,
											   unsigned int x_startE,unsigned int y_startE,const unsigned char * datasE,
												 unsigned int PART_COLUMN,unsigned int PART_LINE
	                      );
//Display canvas function
void EPD_HW_Init_GUI(void); //EPD init GUI
void EPD_Display(unsigned char *Image);
void EPD_SetBaseMap(const unsigned char *Image);     // set partial-refresh baseline (both RAM banks)
void EPD_Display_Partial(const unsigned char *Image); // fast flash-free differential update
void EPD_HW_Init_P(void);
void EPD_Standby(void);

void EPD_HW_Init_Fast(void);
void EPD_WhiteScreen_ALL_Fast(const unsigned char *datas);

// 4-level grayscale (full refresh only). The SSD1680 renders 4 grays from a custom
// waveform LUT (cmd 0x32) + two bitplanes: MSB plane -> RAM 0x26, LSB plane -> 0x24.
// Usage: EPD_Init_4Gray(); EPD_WritePlane(0x26, msb); EPD_WritePlane(0x24, lsb);
// EPD_Update_4Gray();  (planes built by gray2_to_plane() in dither.c)
void EPD_Init_4Gray(void);
void EPD_WritePlane(unsigned char ram_cmd, const unsigned char *plane);
void EPD_Update_4Gray(void);
// Pre-condition the panel before a 4-gray render (the gray LUT is a "from white"
// waveform). strength: 0=none, 1=single white flush, 2=white->black->white wipe.
void EPD_Condition_4Gray(unsigned char strength);
// Restore the B/W waveform after a 4-gray render so subsequent B/W refreshes (incl.
// the partial-refresh countdown) don't inherit the gray waveform/voltages.
void EPD_Restore_BW(void);
#endif
/***********************************************************
						end file
***********************************************************/


