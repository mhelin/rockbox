/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Alan Korr
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdbool.h>
#include "ata.h"
#include "kernel.h"
#include "thread.h"
#include "led.h"
#include "cpu.h"
#include "system.h"
#include "debug.h"
#include "panic.h"
#include "usb.h"
#include "power.h"
#include "string.h"
#include "hwcompat.h"
#include "ata_idle_notify.h"
#include "ata-target.h"

#define SECTOR_SIZE     (512)

#define ATA_FEATURE     ATA_ERROR

#define ATA_STATUS      ATA_COMMAND
#define ATA_ALT_STATUS  ATA_CONTROL

#define SELECT_DEVICE1  0x10
#define SELECT_LBA      0x40

#define CONTROL_nIEN    0x02
#define CONTROL_SRST    0x04

#define CMD_READ_SECTORS           0x20
#define CMD_WRITE_SECTORS          0x30
#define CMD_READ_MULTIPLE          0xC4
#define CMD_WRITE_MULTIPLE         0xC5
#define CMD_SET_MULTIPLE_MODE      0xC6
#define CMD_STANDBY_IMMEDIATE      0xE0
#define CMD_STANDBY                0xE2
#define CMD_IDENTIFY               0xEC
#define CMD_SLEEP                  0xE6
#define CMD_SET_FEATURES           0xEF
#define CMD_SECURITY_FREEZE_LOCK   0xF5

#define Q_SLEEP 0

#define READ_TIMEOUT 5*HZ

static struct mutex ata_mtx;
int ata_device; /* device 0 (master) or 1 (slave) */

int ata_spinup_time = 0;
#if CONFIG_LED == LED_REAL
static bool ata_led_enabled = true;
static bool ata_led_on = false;
#endif
static bool spinup = false;
static bool sleeping = true;
static bool poweroff = false;
static long sleep_timeout = 5*HZ;
#ifdef HAVE_ATA_POWER_OFF
static int poweroff_timeout = 2*HZ;
#endif
static long ata_stack[DEFAULT_STACK_SIZE/sizeof(long)];
static const char ata_thread_name[] = "ata";
static struct event_queue ata_queue;
static bool initialized = false;

static long last_user_activity = -1;
long last_disk_activity = -1;

static int multisectors; /* number of supported multisectors */
static unsigned short identify_info[SECTOR_SIZE];

static int ata_power_on(void);
static int perform_soft_reset(void);
static int set_multiple_mode(int sectors);
static int set_features(void);

static int wait_for_bsy(void) ICODE_ATTR;
static int wait_for_bsy(void)
{
    long timeout = current_tick + HZ*30;
    while (TIME_BEFORE(current_tick, timeout) && (ATA_STATUS & STATUS_BSY)) {
        last_disk_activity = current_tick;
        yield();
    }

    if (TIME_BEFORE(current_tick, timeout))
        return 1;
    else
        return 0; /* timeout */
}

static int wait_for_rdy(void) ICODE_ATTR;
static int wait_for_rdy(void)
{
    long timeout;

    if (!wait_for_bsy())
        return 0;

    timeout = current_tick + HZ*10;

    while (TIME_BEFORE(current_tick, timeout) &&
           !(ATA_ALT_STATUS & STATUS_RDY)) {
        last_disk_activity = current_tick;
        yield();
    }

    if (TIME_BEFORE(current_tick, timeout))
        return STATUS_RDY;
    else
        return 0; /* timeout */
}

static int wait_for_start_of_transfer(void) ICODE_ATTR;
static int wait_for_start_of_transfer(void)
{
    if (!wait_for_bsy())
        return 0;

    return (ATA_ALT_STATUS & (STATUS_BSY|STATUS_DRQ)) == STATUS_DRQ;
}

static int wait_for_end_of_transfer(void) ICODE_ATTR;
static int wait_for_end_of_transfer(void)
{
    if (!wait_for_bsy())
        return 0;
    return (ATA_ALT_STATUS & (STATUS_RDY|STATUS_DRQ)) == STATUS_RDY;
}    

#if CONFIG_LED == LED_REAL
/* Conditionally block LED access for the ATA driver, so the LED can be
 * (mis)used for other purposes */
static void ata_led(bool on) 
{
    ata_led_on = on;
    if (ata_led_enabled)
        led(ata_led_on);
}
#else
#define ata_led(on) led(on)
#endif

#ifndef ATA_OPTIMIZED_READING
static void copy_read_sectors(unsigned char* buf, int wordcount) ICODE_ATTR;
static void copy_read_sectors(unsigned char* buf, int wordcount)
{
    unsigned short tmp = 0;

    if ( (unsigned long)buf & 1)
    {   /* not 16-bit aligned, copy byte by byte */
        unsigned char* bufend = buf + wordcount*2;
        do
        {
            tmp = ATA_DATA;
#if defined(SWAP_WORDS) || defined(ROCKBOX_LITTLE_ENDIAN)
            *buf++ = tmp & 0xff; /* I assume big endian */
            *buf++ = tmp >> 8;   /*  and don't use the SWAB16 macro */
#else
            *buf++ = tmp >> 8;
            *buf++ = tmp & 0xff;
#endif
        } while (buf < bufend); /* tail loop is faster */
    }
    else
    {   /* 16-bit aligned, can do faster copy */
        unsigned short* wbuf = (unsigned short*)buf;
        unsigned short* wbufend = wbuf + wordcount;
        do
        {
#ifdef SWAP_WORDS
            *wbuf = swap16(ATA_DATA);
#else
            *wbuf = ATA_DATA;
#endif
        } while (++wbuf < wbufend); /* tail loop is faster */
    }
}
#endif /* !ATA_OPTIMIZED_READING */

int ata_read_sectors(IF_MV2(int drive,)
                     unsigned long start,
                     int incount,
                     void* inbuf)
{
    int ret = 0;
    long timeout;
    int count;
    void* buf;
    long spinup_start;

#ifdef HAVE_MULTIVOLUME
    (void)drive; /* unused for now */
#endif
    mutex_lock(&ata_mtx);

    last_disk_activity = current_tick;
    spinup_start = current_tick;

    ata_led(true);

    if ( sleeping ) {
        spinup = true;
        if (poweroff) {
            if (ata_power_on()) {
                mutex_unlock(&ata_mtx);
                ata_led(false);
                return -1;
            }
        }
        else {
            if (perform_soft_reset()) {
                mutex_unlock(&ata_mtx);
                ata_led(false);
                return -1;
            }
        }
    }

    timeout = current_tick + READ_TIMEOUT;

    SET_REG(ATA_SELECT, ata_device);
    if (!wait_for_rdy())
    {
        mutex_unlock(&ata_mtx);
        ata_led(false);
        return -2;
    }

 retry:
    buf = inbuf;
    count = incount;
    while (TIME_BEFORE(current_tick, timeout)) {
        ret = 0;
        last_disk_activity = current_tick;

        if ( count == 256 )
            SET_REG(ATA_NSECTOR, 0); /* 0 means 256 sectors */
        else
            SET_REG(ATA_NSECTOR, (unsigned char)count);

        SET_REG(ATA_SECTOR, start & 0xff);
        SET_REG(ATA_LCYL, (start >> 8) & 0xff);
        SET_REG(ATA_HCYL, (start >> 16) & 0xff);
        SET_REG(ATA_SELECT, ((start >> 24) & 0xf) | SELECT_LBA | ata_device);
        SET_REG(ATA_COMMAND, CMD_READ_MULTIPLE);

        /* wait at least 400ns between writing command and reading status */
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");
        __asm__ volatile ("nop");

        while (count) {
            int sectors;
            int wordcount;
            int status;

            if (!wait_for_start_of_transfer()) {
                /* We have timed out waiting for RDY and/or DRQ, possibly
                   because the hard drive is shaking and has problems reading
                   the data. We have two options:
                   1) Wait some more
                   2) Perform a soft reset and try again.

                   We choose alternative 2.
                */
                perform_soft_reset();
                ret = -4;
                goto retry;
            }

            if (spinup) {
                ata_spinup_time = current_tick - spinup_start;
                spinup = false;
                sleeping = false;
                poweroff = false;
            }

            /* read the status register exactly once per loop */
            status = ATA_STATUS;

            /* if destination address is odd, use byte copying,
               otherwise use word copying */

            if (count >= multisectors )
                sectors = multisectors;
            else
                sectors = count;

            wordcount = sectors * SECTOR_SIZE / 2;

            copy_read_sectors(buf, wordcount);

            /*
              "Device errors encountered during READ MULTIPLE commands are
              posted at the beginning of the block or partial block transfer,
              but the DRQ bit is still set to one and the data transfer shall
              take place, including transfer of corrupted data, if any."
                -- ATA specification
            */
            if ( status & (STATUS_BSY | STATUS_ERR | STATUS_DF) ) {
                perform_soft_reset();
                ret = -5;
                goto retry;
            }

            buf += sectors * SECTOR_SIZE; /* Advance one chunk of sectors */
            count -= sectors;

            last_disk_activity = current_tick;
        }

        if(!ret && !wait_for_end_of_transfer()) {
            perform_soft_reset();
            ret = -3;
            goto retry;
        }
        break;
    }
    ata_led(false);

    mutex_unlock(&ata_mtx);

    return ret;
}

#ifndef ATA_OPTIMIZED_WRITING
static void copy_write_sectors(const unsigned char* buf, int wordcount) ICODE_ATTR;
static void copy_write_sectors(const unsigned char* buf, int wordcount)
{
    if ( (unsigned long)buf & 1)
    {   /* not 16-bit aligned, copy byte by byte */
        unsigned short tmp = 0;
        const unsigned char* bufend = buf + wordcount*2;
        do
        {
#if defined(SWAP_WORDS) || defined(ROCKBOX_LITTLE_ENDIAN)
            tmp = (unsigned short) *buf++;
            tmp |= (unsigned short) *buf++ << 8;
            SET_16BITREG(ATA_DATA, tmp);
#else
            tmp = (unsigned short) *buf++ << 8;
            tmp |= (unsigned short) *buf++;
            SET_16BITREG(ATA_DATA, tmp);
#endif
        } while (buf < bufend); /* tail loop is faster */
    }
    else
    {   /* 16-bit aligned, can do faster copy */
        unsigned short* wbuf = (unsigned short*)buf;
        unsigned short* wbufend = wbuf + wordcount;
        do
        {
#ifdef SWAP_WORDS
            SET_16BITREG(ATA_DATA, swap16(*wbuf));
#else
            SET_16BITREG(ATA_DATA, *wbuf);
#endif
        } while (++wbuf < wbufend); /* tail loop is faster */
    }
}
#endif /* !ATA_OPTIMIZED_WRITING */

int ata_write_sectors(IF_MV2(int drive,)
                      unsigned long start,
                      int count,
                      const void* buf)
{
    int i;
    int ret = 0;
    long spinup_start;

#ifdef HAVE_MULTIVOLUME
    (void)drive; /* unused for now */
#endif
    if (start == 0)
        panicf("Writing on sector 0\n");

    mutex_lock(&ata_mtx);
    
    last_disk_activity = current_tick;
    spinup_start = current_tick;

    ata_led(true);

    if ( sleeping ) {
        spinup = true;
        if (poweroff) {
            if (ata_power_on()) {
                mutex_unlock(&ata_mtx);
                ata_led(false);
                return -1;
            }
        }
        else {
            if (perform_soft_reset()) {
                mutex_unlock(&ata_mtx);
                ata_led(false);
                return -1;
            }
        }
    }
    
    SET_REG(ATA_SELECT, ata_device);
    if (!wait_for_rdy())
    {
        mutex_unlock(&ata_mtx);
        ata_led(false);
        return -2;
    }

    if ( count == 256 )
        SET_REG(ATA_NSECTOR, 0); /* 0 means 256 sectors */
    else
        SET_REG(ATA_NSECTOR, (unsigned char)count);
    SET_REG(ATA_SECTOR, start & 0xff);
    SET_REG(ATA_LCYL, (start >> 8) & 0xff);
    SET_REG(ATA_HCYL, (start >> 16) & 0xff);
    SET_REG(ATA_SELECT, ((start >> 24) & 0xf) | SELECT_LBA | ata_device);
    SET_REG(ATA_COMMAND, CMD_WRITE_SECTORS);

    for (i=0; i<count; i++) {

        if (!wait_for_start_of_transfer()) {
            ret = -3;
            break;
        }

        if (spinup) {
            ata_spinup_time = current_tick - spinup_start;
            spinup = false;
            sleeping = false;
            poweroff = false;
        }

        copy_write_sectors(buf, SECTOR_SIZE/2);

#ifdef USE_INTERRUPT
        /* reading the status register clears the interrupt */
        j = ATA_STATUS;
#endif
        buf += SECTOR_SIZE;

        last_disk_activity = current_tick;
    }

    if(!ret && !wait_for_end_of_transfer()) {
        DEBUGF("End on transfer failed. -- jyp");
        ret = -4;
    }

    ata_led(false);

    mutex_unlock(&ata_mtx);

    return ret;
}

static int check_registers(void)
{
#if (CONFIG_CPU == PP5002)
    /* This fails on the PP5002, but the ATA driver still works.  This
       needs more investigation. */
    return 0;
#else
    int i;
    if ( ATA_STATUS & STATUS_BSY )
            return -1;

    for (i = 0; i<64; i++) {
        SET_REG(ATA_NSECTOR, WRITE_PATTERN1);
        SET_REG(ATA_SECTOR,  WRITE_PATTERN2);
        SET_REG(ATA_LCYL,    WRITE_PATTERN3);
        SET_REG(ATA_HCYL,    WRITE_PATTERN4);

        if (((ATA_NSECTOR & READ_PATTERN1_MASK) == READ_PATTERN1) &&
            ((ATA_SECTOR & READ_PATTERN2_MASK) == READ_PATTERN2) &&
            ((ATA_LCYL & READ_PATTERN3_MASK) == READ_PATTERN3) &&
            ((ATA_HCYL & READ_PATTERN4_MASK) == READ_PATTERN4))
            return 0;
    }
    return -2;
#endif
}

static int freeze_lock(void)
{
    /* does the disk support Security Mode feature set? */
    if (identify_info[82] & 2)
    {
        SET_REG(ATA_SELECT, ata_device);

        if (!wait_for_rdy())
            return -1;

        SET_REG(ATA_COMMAND, CMD_SECURITY_FREEZE_LOCK);

        if (!wait_for_rdy())
            return -2;
    }

    return 0;
}

void ata_spindown(int seconds)
{
    sleep_timeout = seconds * HZ;
}

#ifdef HAVE_ATA_POWER_OFF
void ata_poweroff(bool enable)
{
    if (enable)
        poweroff_timeout = 2*HZ;
    else
        poweroff_timeout = 0;
}
#endif

bool ata_disk_is_active(void)
{
    return !sleeping;
}

static int ata_perform_sleep(void)
{
    int ret = 0;

    mutex_lock(&ata_mtx);

    SET_REG(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("ata_perform_sleep() - not RDY\n");
        mutex_unlock(&ata_mtx);
        return -1;
    }

    SET_REG(ATA_COMMAND, CMD_SLEEP);

    if (!wait_for_rdy())
    {
        DEBUGF("ata_perform_sleep() - CMD failed\n");
        ret = -2;
    }

    sleeping = true;
    mutex_unlock(&ata_mtx);
    return ret;
}

void ata_sleep(void)
{
    queue_post(&ata_queue, Q_SLEEP, NULL);
}

void ata_sleepnow(void)
{
    if (!spinup && !sleeping && !ata_mtx.locked)
    {
        call_ata_idle_notifys(false);
        ata_perform_sleep();
    }
}

void ata_spin(void)
{
    last_user_activity = current_tick;
}

static void ata_thread(void)
{
    static long last_sleep = 0;
    struct event ev;
    static long last_seen_mtx_unlock = 0;
    
    while (1) {
        while ( queue_empty( &ata_queue ) ) {
            if (!spinup && !sleeping)
            {
                if (!ata_mtx.locked)
                {
                    if (!last_seen_mtx_unlock)
                        last_seen_mtx_unlock = current_tick;
                    if (TIME_AFTER(current_tick, last_seen_mtx_unlock+(HZ*2)))
                    {
                        call_ata_idle_notifys(false);
                        last_seen_mtx_unlock = 0;
                    }
                }
                if ( sleep_timeout &&
                     TIME_AFTER( current_tick, 
                                last_user_activity + sleep_timeout ) &&
                     TIME_AFTER( current_tick, 
                                last_disk_activity + sleep_timeout ) )
                {
                    call_ata_idle_notifys(true);
                    ata_perform_sleep();
                    last_sleep = current_tick;
                }
            }
#ifdef HAVE_ATA_POWER_OFF
            if ( !spinup && sleeping && poweroff_timeout && !poweroff &&
                 TIME_AFTER( current_tick, last_sleep + poweroff_timeout ))
            {
                mutex_lock(&ata_mtx);
                ide_power_enable(false);
                mutex_unlock(&ata_mtx);
                poweroff = true;
            }
#endif

            sleep(HZ/4);
        }
        queue_wait(&ata_queue, &ev);
        switch ( ev.id ) {
#ifndef USB_NONE
            case SYS_USB_CONNECTED:
                if (poweroff) {
                    mutex_lock(&ata_mtx);
                    ata_led(true);
                    ata_power_on();
                    ata_led(false);
                    mutex_unlock(&ata_mtx);
                }

                /* Tell the USB thread that we are safe */
                DEBUGF("ata_thread got SYS_USB_CONNECTED\n");
                usb_acknowledge(SYS_USB_CONNECTED_ACK);

                /* Wait until the USB cable is extracted again */
                usb_wait_for_disconnect(&ata_queue);
                break;
#endif
            case Q_SLEEP:
                call_ata_idle_notifys(false);
                last_disk_activity = current_tick - sleep_timeout + (HZ/2);
                break;
        }
    }
}

/* Hardware reset protocol as specified in chapter 9.1, ATA spec draft v5 */
int ata_hard_reset(void)
{
    int ret;

    ata_reset();

    /* state HRR2 */
    SET_REG(ATA_SELECT, ata_device); /* select the right device */
    ret = wait_for_bsy();

    /* Massage the return code so it is 0 on success and -1 on failure */
    ret = ret?0:-1;

    return ret;
}

static int perform_soft_reset(void)
{
/* If this code is allowed to run on a Nano, the next reads from the flash will
 * time out, so we disable it. It shouldn't be necessary anyway, since the
 * ATA -> Flash interface automatically sleeps almost immediately after the
 * last command.
 */
#ifndef IPOD_NANO
    int ret;
    int retry_count;
    
    SET_REG(ATA_SELECT, SELECT_LBA | ata_device );
    SET_REG(ATA_CONTROL, CONTROL_nIEN|CONTROL_SRST );
    sleep(1); /* >= 5us */

    SET_REG(ATA_CONTROL, CONTROL_nIEN);
    sleep(1); /* >2ms */

    /* This little sucker can take up to 30 seconds */
    retry_count = 8;
    do
    {
        ret = wait_for_rdy();
    } while(!ret && retry_count--);

    /* Massage the return code so it is 0 on success and -1 on failure */
    ret = ret?0:-1;

    return ret;
#else
    return 0; /* Always report success */
#endif
}

int ata_soft_reset(void)
{
    int ret;
    
    mutex_lock(&ata_mtx);

    ret = perform_soft_reset();

    mutex_unlock(&ata_mtx);
    return ret;
}

static int ata_power_on(void)
{
    int rc;
    
    ide_power_enable(true);
    if( ata_hard_reset() )
        return -1;

    rc = set_features();
    if (rc)
        return rc * 10 - 2;

    if (set_multiple_mode(multisectors))
        return -3;

    if (freeze_lock())
        return -4;

    return 0;
}

static int master_slave_detect(void)
{
    /* master? */
    SET_REG(ATA_SELECT, 0);
    if ( ATA_STATUS & (STATUS_RDY|STATUS_BSY) ) {
        ata_device = 0;
        DEBUGF("Found master harddisk\n");
    }
    else {
        /* slave? */
        SET_REG(ATA_SELECT, SELECT_DEVICE1);
        if ( ATA_STATUS & (STATUS_RDY|STATUS_BSY) ) {
            ata_device = SELECT_DEVICE1;
            DEBUGF("Found slave harddisk\n");
        }
        else
            return -1;
    }
    return 0;
}

static int identify(void)
{
    int i;

    SET_REG(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("identify() - not RDY\n");
        return -1;
    }
    SET_REG(ATA_COMMAND, CMD_IDENTIFY);

    if (!wait_for_start_of_transfer())
    {
        DEBUGF("identify() - CMD failed\n");
        return -2;
    }

    for (i=0; i<SECTOR_SIZE/2; i++) {
        /* the IDENTIFY words are already swapped, so we need to treat
           this info differently that normal sector data */
#if defined(ROCKBOX_BIG_ENDIAN) && !defined(SWAP_WORDS)
        identify_info[i] = swap16(ATA_DATA);
#else
        identify_info[i] = ATA_DATA;
#endif
    }
    
    return 0;
}

static int set_multiple_mode(int sectors)
{
    SET_REG(ATA_SELECT, ata_device);

    if(!wait_for_rdy()) {
        DEBUGF("set_multiple_mode() - not RDY\n");
        return -1;
    }

    SET_REG(ATA_NSECTOR, sectors);
    SET_REG(ATA_COMMAND, CMD_SET_MULTIPLE_MODE);

    if (!wait_for_rdy())
    {
        DEBUGF("set_multiple_mode() - CMD failed\n");
        return -2;
    }

    return 0;
}

static int set_features(void)
{
    struct {
        unsigned char id_word;
        unsigned char id_bit;
        unsigned char subcommand;
        unsigned char parameter;
    } features[] = {
        { 83, 3, 0x05, 0x80 }, /* power management: lowest power without standby */
        { 83, 9, 0x42, 0x80 }, /* acoustic management: lowest noise */
        { 82, 6, 0xaa, 0 },    /* enable read look-ahead */
        { 83, 14, 0x03, 0 },   /* force PIO mode */
        { 0, 0, 0, 0 }         /* <end of list> */
    };
    int i;
    int pio_mode = 2;

    /* Find out the highest supported PIO mode */
    if(identify_info[64] & 2)
        pio_mode = 4;
    else
        if(identify_info[64] & 1)
            pio_mode = 3;

    /* Update the table */
    features[3].parameter = 8 + pio_mode;
    
    SET_REG(ATA_SELECT, ata_device);

    if (!wait_for_rdy()) {
        DEBUGF("set_features() - not RDY\n");
        return -1;
    }

    for (i=0; features[i].id_word; i++) {
        if (identify_info[features[i].id_word] & (1 << features[i].id_bit)) {
            SET_REG(ATA_FEATURE, features[i].subcommand);
            SET_REG(ATA_NSECTOR, features[i].parameter);
            SET_REG(ATA_COMMAND, CMD_SET_FEATURES);

            if (!wait_for_rdy()) {
                DEBUGF("set_features() - CMD failed\n");
                return -10 - i;
            }

            if(ATA_ALT_STATUS & STATUS_ERR) {
                if(ATA_ERROR & ERROR_ABRT) {
                    return -20 - i;
                }
            }
        }
    }

    return 0;
}

unsigned short* ata_get_identify(void)
{
    return identify_info;
}

static int init_and_check(bool hard_reset)
{
    int rc;

    if (hard_reset)
    {
        /* This should reset both master and slave, we don't yet know what's in */
        ata_device = 0;
        if (ata_hard_reset())
            return -1;
    }

    rc = master_slave_detect();
    if (rc)
        return -10 + rc;

    /* symptom fix: else check_registers() below may fail */
    if (hard_reset && !wait_for_bsy())
        return -20;

    rc = check_registers();
    if (rc)
        return -30 + rc;
    
    return 0;
}

int ata_init(void)
{
    int rc;
    bool coldstart = ata_is_coldstart();

    mutex_init(&ata_mtx);

    ata_led(false);
    ata_device_init();
    sleeping = false;
    ata_enable(true);

    if ( !initialized ) {
        if (!ide_powered()) /* somebody has switched it off */
        {
            ide_power_enable(true);
            sleep(HZ); /* allow voltage to build up */
        }

#ifdef ATA_ADDRESS_DETECT
        ata_address_detect();
#endif  
        /* first try, hard reset at cold start only */
        rc = init_and_check(coldstart);  

        if (rc) 
        {   /* failed? -> second try, always with hard reset */
            DEBUGF("ata: init failed, retrying...\n");
            rc  = init_and_check(true);
            if (rc)
                return rc;
        }

        rc = identify();

        if (rc)
            return -40 + rc;

        multisectors = identify_info[47] & 0xff;
        DEBUGF("ata: %d sectors per ata request\n",multisectors);

        rc = freeze_lock();

        if (rc)
            return -50 + rc;

        rc = set_features();
        if (rc)
            return -60 + rc;

        queue_init(&ata_queue, true);

        last_disk_activity = current_tick;
        create_thread(ata_thread, ata_stack,
                      sizeof(ata_stack), ata_thread_name
                      IF_PRIO(, PRIORITY_SYSTEM));
        initialized = true;

    }
    rc = set_multiple_mode(multisectors);
    if (rc)
        return -70 + rc;

    return 0;
}

#if CONFIG_LED == LED_REAL
void ata_set_led_enabled(bool enabled) 
{
    ata_led_enabled = enabled;
    if (ata_led_enabled)
        led(ata_led_on);
    else
        led(false);
}
#endif
