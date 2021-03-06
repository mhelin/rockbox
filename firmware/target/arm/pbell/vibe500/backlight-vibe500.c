/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id:$
 *
 * Copyright (C) 2009 by Szymon Dziok
 * Based on the Iriver H10 and the Philips HD1630 code.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "cpu.h"
#include "system.h"
#include "backlight.h"
#include "backlight-target.h"
#include "lcd.h"
#include "synaptics-mep.h"

void backlight_hw_on(void)
{
#ifdef HAVE_LCD_ENABLE
    lcd_enable(true); /* power on lcd + visible display */
#endif
    GPIO_SET_BITWISE(GPIOJ_OUTPUT_VAL, 0x01);
}

void backlight_hw_off(void)
{
    GPIO_CLEAR_BITWISE(GPIOJ_OUTPUT_VAL, 0x01);
#ifdef HAVE_LCD_ENABLE
    lcd_enable(false); /* power off visible display */
#endif
}

#ifdef HAVE_BACKLIGHT_BRIGHTNESS
static const int brightness_vals[16] =
                {255,237,219,201,183,165,147,130,112,94,76,58,40,22,5,0};

void backlight_hw_brightness(int brightness)
{
    /* From PB Vibe Bootloader and OF */
    DEV_INIT1&=0xFFFF3F3F;
    DEV_INIT1+=0x4000;
    DEV_EN |= 0x20000;
    outl(0x80000000 | (brightness_vals[brightness-1] << 16), 0x7000a010);
}
#endif

#ifdef HAVE_BUTTON_LIGHT
static unsigned short buttonlight_status = 0;

void buttonlight_hw_on(void)
{
    if (!buttonlight_status)
    {
        touchpad_set_parameter(0, 0x22, 0x000f); /* 0x22 - GPO_ENABLE */
        buttonlight_status = 1;
    }
}

void buttonlight_hw_off(void)
{
    if (buttonlight_status)
    {
        touchpad_set_parameter(0, 0x22, 0x0000); /* 0x22 - GPO_ENABLE */
        buttonlight_status = 0;
    }
}

void buttonlight_hw_brightness(int brightness)
{
    /* no brightness control, but lights stays on - for compatibility */
    (void)brightness;
    touchpad_set_parameter(0, 0x22, 0x000f); /* 0x22 - GPO_ENABLE */
    buttonlight_status = 1;
}
#endif
