#ifndef __MY_OLED_H
#define __MY_OLED_H

void My_OLED_Init(void);
void My_OLED_WriteCommand(uint8_t Command);
void My_OLED_WriteData(uint8_t *Data,uint8_t Count);
void My_OLED_SetCursor(uint8_t X,uint8_t Page);
void My_OLED_Update(void);
void My_OLED_Clear(void);
void My_OLED_ClearArea(uint8_t X,uint8_t Y,uint8_t Width,uint8_t Height);
void My_OLED_ShowChar(uint8_t X,uint8_t Y,char Char,uint8_t FintSize);
void My_OLED_ShowString(uint8_t X,uint8_t Y,char *String,uint8_t FontSize);
void My_OLED_ShowImage(uint8_t X, uint8_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);
void My_OLED_ShowChinese(uint8_t X,uint8_t Y,char *Chinese);
void My_OLED_DrawPoint(uint8_t X,uint8_t Y);
uint8_t My_OLED_GetPoint(uint8_t X,uint8_t Y);
void My_OLED_DrawLine(uint8_t X0,uint8_t Y0,uint8_t X1,uint8_t Y1);

#endif
