/* Copyright (c) 2009, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/delay.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <media/msm_camera.h>
#include <mach/gpio.h>

#include "s5k4ecgx_gio.h"
#include <mach/camera.h>
#include <mach/vreg.h>

#define SENSOR_DEBUG 0

#undef CONFIG_LOAD_FILE
//#define CONFIG_LOAD_FILE
#ifndef CONFIG_LOAD_FILE
#define S5K4ECGX_USE_BURSTMODE
#endif

#ifdef  S5K4ECGX_USE_BURSTMODE
#define S5K4ECGX_WRITE_LIST(A) \
	{\
		if(s5k4ecgx_status.id == 0x0011)s5k4ecgx_sensor_burst_write(A##_EVT1,(sizeof(A##_EVT1) / sizeof(A##_EVT1[0])),#A"_EVT1");\
		else s5k4ecgx_sensor_burst_write(A,(sizeof(A) / sizeof(A[0])),#A);\
	}
#else
#define S5K4ECGX_WRITE_LIST(A) \
	{\
		if(s5k4ecgx_status.id == 0x0011)s5k4ecgx_sensor_write_list(A##_EVT1,(sizeof(A##_EVT1) / sizeof(A##_EVT1[0])),#A"_EVT1");\
		else s5k4ecgx_sensor_write_list(A,(sizeof(A) / sizeof(A[0])),#A);\
	}
#endif

#define CAM_FLASH_ENSET 1
#define CAM_FLASH_FLEN 2
#define FULL_FLASH 20
#define PRE_FLASH 7
#define MOVIE_FLASH 7
#define MACRO_FLASH 14
#define PRE_FLASH_OFF -1
#define FLASH_OFF 0

#define PREVIEW 1
#define SNAPSHOT 2

#define CHECK_EFFECT 0x00000001
#define CHECK_BRIGHTNESS 0x00000002
#define CHECK_CONTRAST 0x00000004
#define CHECK_SATURATION 0x00000008
#define CHECK_SHARPNESS 0x00000010
#define CHECK_WB 0x00000020
#define CHECK_ISO 0x00000040
#define CHECK_AE 0x00000080
#define CHECK_SCENE 0x00000100
#define CHECK_AFMODE 0x00000200
#define CHECK_DTP 0x00000400
#define CHECK_SNAPSHOT_SIZE 0x00000800
#define CHECK_PREVIEW_SIZE 0x00001000
#define CHECK_ZOOM 0x00002000
#define CHECK_JPEGQUALITY 0x00004000
#define CHECK_AUTOCONTRAST 0x00008000
#define CHECK_FPS 0x00010000
#define CHECK_AE_AWB_LOCK 0x00020000

struct s5k4ecgx_status_struct {
	char camera_initailized;//  1 is not init a sensor
	u32 need_configuration;
	char camera_mode;
	char effect;
	char brightness;
	char contrast;
	char saturation;
	char sharpness;
	char whiteBalance;
	char iso;
	char auto_exposure;
	char scene;
	char afmode;
	char afcanceled;
	char dtp;
	char snapshot_size;
	char preview_size;
	char flash_mode;
	char flash_exifinfo;
	char zoom;
	char lowcap_on;
	char nightcap_on;
	char power_on;
	char fps;
	char ae_lock;
	char awb_lock;
	char camera_status;
	int flash_status;
	int jpeg_quality;
	int auto_contrast;
	int current_lux;
	unsigned short id;
};

static struct s5k4ecgx_status_struct s5k4ecgx_status;

bool isPreviewReturnWrite = false;
static unsigned int i2c_retry = 0;
static unsigned int probe_init_retry = 0;

struct s5k4ecgx_work {
	struct work_struct work;
};

static struct  s5k4ecgx_work *s5k4ecgx_sensorw;
static struct  i2c_client *s5k4ecgx_client;

struct s5k4ecgx_ctrl {
	const struct msm_camera_sensor_info *sensordata;
};

static struct s5k4ecgx_ctrl *s5k4ecgx_ctrl;
#ifdef USE_FLASHOFF_TIMER
static struct timer_list flashoff_timer;
#endif
static DECLARE_WAIT_QUEUE_HEAD(s5k4ecgx_wait_queue);
DECLARE_MUTEX(s5k4ecgx_sem);

/*=============================================================
	EXTERNAL DECLARATIONS
==============================================================*/
extern struct s5k4ecgx_reg s5k4ecgx_regs;
extern int cpufreq_direct_set_policy(unsigned int cpu, const char *buf);

/*=============================================================*/

static int s5k4ecgx_probe_init_sensor(void);

#ifdef CONFIG_LOAD_FILE
	static int s5k4ecgx_regs_table_write(char *name);
#endif

static int s5k4ecgx_sensor_read(unsigned short subaddr, unsigned short *data)
{
	int ret;
	unsigned char buf[2];
	struct i2c_msg msg = { s5k4ecgx_client->addr, 0, 2, buf };
	
	buf[0] = (subaddr >> 8);
	buf[1] = (subaddr & 0xFF);
	
	if(!s5k4ecgx_status.power_on)return -1;
	
	ret = i2c_transfer(s5k4ecgx_client->adapter, &msg, 1) == 1 ? 0 : -EIO;
	if (ret == -EIO) 
		goto error;

	msg.flags = I2C_M_RD;
	
	ret = i2c_transfer(s5k4ecgx_client->adapter, &msg, 1) == 1 ? 0 : -EIO;
	if (ret == -EIO) 
		goto error;

	*data = ((buf[0] << 8) | buf[1]);
	/*  [Arun c]Data should be written in Little Endian in parallel mode; So there is no need for byte swapping here */
	//*data = *(unsigned long *)(&buf);
error:
	return ret;
}

static int s5k4ecgx_sensor_write(unsigned short subaddr, unsigned short val)
{
	unsigned char buf[4];
	struct i2c_msg msg = { s5k4ecgx_client->addr, 0, 4, buf };

	//PCAM_DEBUG("[PGH] on write func s5k4ecgx_client->addr : %x\n", s5k4ecgx_client->addr);
	//PCAM_DEBUG("[PGH] on write func  s5k4ecgx_client->adapter->nr : %d\n", s5k4ecgx_client->adapter->nr);

	buf[0] = (subaddr >> 8);
	buf[1] = (subaddr & 0xFF);
	buf[2] = (val >> 8);
	buf[3] = (val & 0xFF);

	return i2c_transfer(s5k4ecgx_client->adapter, &msg, 1) == 1 ? 0 : -EIO;
}

static int s5k4ecgx_sensor_write_list(struct s5k4ecgx_reg *list,int size, char *name)
{
	int ret, i;
	unsigned short subaddr;
	unsigned short value;

	if(!s5k4ecgx_status.power_on)return -1;
	
#ifdef CONFIG_LOAD_FILE	
	ret = s5k4ecgx_regs_table_write(name);
#else
	PCAM_DEBUG("s5k4ecgx_sensor_write_list : %s\n",name);
	  
	for (i = 0; i < size; i++)
	{
		//PCAM_DEBUG("[PGH] %x      %x\n", list[i].subaddr, list[i].value);
		subaddr = ((list[i].value)>> 16); //address
		value = ((list[i].value)& 0xFFFF); //value
		if(subaddr == 0xffff)
		{
			PCAM_DEBUG("SETFILE DELAY : %dms\n",value);
			msleep(value);
		}
		else
		{
		    if(s5k4ecgx_sensor_write(subaddr, value) < 0)
		    {
			    printk("[S5K4ECGX]sensor_write_list fail...-_-\n");
			    return -1;
		    }
		}
	}
#endif
	return ret;
}

#ifdef S5K4ECGX_USE_BURSTMODE
#define BURST_MODE_BUFFER_MAX_SIZE 2700
unsigned char s5k4ecgx_buf_for_burstmode[BURST_MODE_BUFFER_MAX_SIZE];
static int s5k4ecgx_sensor_burst_write(struct s5k4ecgx_reg *list, int size, char *name)
{
	int i = 0;
	int idx = 0;
	int err = -EINVAL;
	int retry = 0;
	unsigned short subaddr=0,next_subaddr=0;
	unsigned short value=0;
	struct i2c_msg msg = { s5k4ecgx_client->addr, 0, 0, s5k4ecgx_buf_for_burstmode };

	PCAM_DEBUG("s5k4ecgx_sensor_burst_write : %s\n",name);

I2C_RETRY:
	idx = 0;
	for (i = 0; i < size; i++) 
	{
		if(idx > (BURST_MODE_BUFFER_MAX_SIZE-10))
		{
			printk("[S5K4ECGX]s5k4ecgx_buf_for_burstmode overflow will occur!!!\n");
			return err;
		}
			
		subaddr = ((list[i].value)>> 16); //address
		if(subaddr == 0x0F12) next_subaddr= ((list[i+1].value)>> 16); //address
		value = ((list[i].value)& 0xFFFF); //value
		
		switch(subaddr)
		{
			case 0x0F12 :
				// make and fill buffer for burst mode write
				if(idx ==0) 
				{
					s5k4ecgx_buf_for_burstmode[idx++] = 0x0F;
					s5k4ecgx_buf_for_burstmode[idx++] = 0x12;
				}
				s5k4ecgx_buf_for_burstmode[idx++] = value>> 8;
				s5k4ecgx_buf_for_burstmode[idx++] = value & 0xFF;
			 	//write in burstmode	
				if(next_subaddr != 0x0F12)
				{
					msg.len = idx;
					err = i2c_transfer(s5k4ecgx_client->adapter, &msg, 1) == 1 ? 0 : -EIO;
					//printk("s5k4ecgx_sensor_burst_write, idx = %d\n",idx);
					idx=0;
				}
			break;
			case 0xFFFF :
			break;
			default:
				// Set Address
				idx=0;
				err = s5k4ecgx_sensor_write(subaddr,value);
			break;
		}
	}
	if (unlikely(err < 0))
	{
		printk("[S5K4ECGX]%s: register set failed. try again.\n",__func__);
		i2c_retry++;
		if((retry++)<10)goto I2C_RETRY;
		return err;
	}
	//PCAM_DEBUG("s5k4ecgx_sensor_burst_write end!\n");
	return 0;
}
#endif /*S5K4ECGX_USE_BURSTMODE*/

void s5k4ecgx_set_effect(char value)
{
	switch(value)
	{
		case PCAM_EFFECT_NORMAL :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Effect_Normal);
		break;
		case PCAM_EFFECT_NEGATIVE :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Effect_Negative);
		break;
		case PCAM_EFFECT_MONO :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Effect_Black_White);
		break;
		case PCAM_EFFECT_SEPIA :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Effect_Sepia);
		break;
		default :
			printk("[S5K4ECGX]Unexpected Effect mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_whitebalance(char value)
{
	int REG_TC_DBG_AutoAlgEnBits = 0;

	/* Read 04E6 */
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x04E6);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&REG_TC_DBG_AutoAlgEnBits);
	
	switch(value)
	{
		case PCAM_WB_AUTO :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits | 0x8;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_WB_Auto);
		break;
		case PCAM_WB_DAYLIGHT :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_WB_Sunny);
		break;
		case PCAM_WB_CLOUDY :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_WB_Cloudy);
		break;
		case PCAM_WB_FLUORESCENT :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_WB_Fluorescent);
		break;
		case PCAM_WB_INCANDESCENT :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_WB_Tungsten);
		break;
		default :
			printk("[S5K4ECGX]Unexpected WB mode : %d\n",  value);
		break;
		}
}

void s5k4ecgx_set_brightness(char value)
{
	switch(value)
	{
		case PCAM_BR_STEP_P_4 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Plus_4);
		break;
		case PCAM_BR_STEP_P_3 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Plus_3);
		break;
		case PCAM_BR_STEP_P_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Plus_2);
		break;
		case PCAM_BR_STEP_P_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Plus_1);
		break;
		case PCAM_BR_STEP_0 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Default);
		break;
		case PCAM_BR_STEP_M_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Minus_1);
		break;
		case PCAM_BR_STEP_M_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Minus_2);
		break;
		case PCAM_BR_STEP_M_3 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Minus_3);
		break;
		case PCAM_BR_STEP_M_4 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_EV_Minus_4);
		break;
		default :
			printk("[S5K4ECGX]Unexpected BR mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_iso(char value)
{
	int REG_TC_DBG_AutoAlgEnBits = 0;
	
	/* Read 04E6 */
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x04E6);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&REG_TC_DBG_AutoAlgEnBits);
	
	switch(value)
	{
		case PCAM_ISO_AUTO :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits | 0x20;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_Auto);
		break;
		case PCAM_ISO_50 :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_50);
		break;
		case PCAM_ISO_100 :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_100);
		break;
		case PCAM_ISO_200 :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_200);
		break;
		case PCAM_ISO_400 :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_400);
		break;
		default :
			printk("[S5K4ECGX]Unexpected ISO mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_metering(char value)
{
	switch(value)
	{
		case PCAM_METERING_NORMAL :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Metering_Matrix);
		break;
		case PCAM_METERING_SPOT :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Metering_Spot);
		break;
		case PCAM_METERING_CENTER :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Metering_Center);
		break;
		default :
			printk("[S5K4ECGX]Unexpected METERING mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_contrast(char value)
{
	switch(value)
	{
		case PCAM_CR_STEP_M_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Contrast_Minus_2);
		break;
		case PCAM_CR_STEP_M_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Contrast_Minus_1);
		break;
		case PCAM_CR_STEP_0 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Contrast_Default);
		break;
		case PCAM_CR_STEP_P_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Contrast_Plus_1);
		break;
		case PCAM_CR_STEP_P_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Contrast_Plus_2);
		break;
		default :
			printk("[S5K4ECGX]Unexpected PCAM_CR_CONTROL mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_zoom(char value)
{
	switch(value)
	{
		case PCAM_ZOOM_STEP_0 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_0);
		break;
		case PCAM_ZOOM_STEP_1 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_1);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_1);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_1);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_1);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_1);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_1);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_1);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_1);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_2 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_2);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_2);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_2);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_2);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_2);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_2);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_2);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_2);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_3 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_3);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_3);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_3);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_3);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_3);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_3);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_3);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_3);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_4 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_4);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_4);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_4);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_4);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_4);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_4);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_4);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_4);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_5 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_5);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_5);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_5);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_5);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_5);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_5);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_5);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_5);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_6 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_6);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_6);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_6);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_6);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_6);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_6);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_6);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_6);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_7 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_7);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_7);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_7);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_7);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_7);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_7);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_7);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_7);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		case PCAM_ZOOM_STEP_8 :
			if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_CAMERA)
			{
				switch(s5k4ecgx_status.snapshot_size)
				{
					case PCAM_SNAPSHOT_SIZE_2048x1536_3M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_25_Zoom_8);break;
					case PCAM_SNAPSHOT_SIZE_1600x1200_2M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_6_Zoom_8);break;
					case PCAM_SNAPSHOT_SIZE_1280x960_1M: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_8);break;
					case PCAM_SNAPSHOT_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_8);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
			else
			{
				switch(s5k4ecgx_status.preview_size)
				{
					case PCAM_PREVIEW_SIZE_720x480_D1: S5K4ECGX_WRITE_LIST(s5k4ecgx_X1_77_Zoom_8);break;
					case PCAM_PREVIEW_SIZE_640x480_VGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_8);break;
					case PCAM_PREVIEW_SIZE_320x240_QVGA: S5K4ECGX_WRITE_LIST(s5k4ecgx_X2_Zoom_8);break;
					case PCAM_PREVIEW_SIZE_176x144_QCIF: S5K4ECGX_WRITE_LIST(s5k4ecgx_X4_Zoom_8);break;
					default: printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);break;
				}
			}
		break;
		default :
			printk("[S5K4ECGX]Unexpected ZOOM mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_saturation(char value)
{
	switch(value)
	{
		case PCAM_SA_STEP_M_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Saturation_Minus_2);
		break;
		case PCAM_SA_STEP_M_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Saturation_Minus_1);
		break;
		case PCAM_SA_STEP_0 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Saturation_Default);
		break;
		case PCAM_SA_STEP_P_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Saturation_Plus_1);
		break;
		case PCAM_SA_STEP_P_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Saturation_Plus_2);
		break;
		default :
			printk("[S5K4ECGX]Unexpected PCAM_SA_CONTROL mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_sharpness(char value)
{
	switch(value)
	{
		case PCAM_SP_STEP_M_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Sharpness_Minus_2);
		break;
		case PCAM_SP_STEP_M_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Sharpness_Minus_1);
		break;
		case PCAM_SP_STEP_0 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Sharpness_Default);
		break;
		case PCAM_SP_STEP_P_1 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Sharpness_Plus_1);
		break;
		case PCAM_SP_STEP_P_2 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Sharpness_Plus_2);
		break;
		default :
			printk("[S5K4ECGX]Unexpected PCAM_SP_CONTROL mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_auto_contrast(char value)
{
	switch(value)
	{
		case PCAM_AUTO_CONTRAST_ON :
			s5k4ecgx_set_contrast(PCAM_CR_STEP_0);
			s5k4ecgx_set_saturation(PCAM_SA_STEP_0);
			s5k4ecgx_set_sharpness(PCAM_SP_STEP_0);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Auto_Contrast_ON);
		break;
		case PCAM_AUTO_CONTRAST_OFF :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Auto_Contrast_OFF);
			s5k4ecgx_set_contrast(s5k4ecgx_status.contrast);
			s5k4ecgx_set_saturation(s5k4ecgx_status.saturation);
			s5k4ecgx_set_sharpness(s5k4ecgx_status.sharpness);
		break;
		default :
			printk("[S5K4ECGX]Unexpected auto_contrast mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_jpeg_quality(char value)
{
	switch(value)
	{
		case PCAM_JPEG_QUALITY_SUPERFINE :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Jpeg_Quality_High);
		break;
		case PCAM_JPEG_QUALITY_FINE :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Jpeg_Quality_Normal);
		break;
		case PCAM_JPEG_QUALITY_NORMAL :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Jpeg_Quality_Low);
		break;
		default :
			printk("[S5K4ECGX]Unexpected Jpeg quality mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_fps(char value)
{
	switch(value)
	{
		case PCAM_FRAME_AUTO :
			//S5K4ECGX_WRITE_LIST(s5k4ecgx_FPS_15);
		break;
		case PCAM_FRAME_FIX_15 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_FPS_15);
		break;
		case PCAM_FRAME_FIX_30 :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_FPS_20);
		break;
		default :
			printk("[S5K4ECGX]Unexpected PCAM_FRAME_CONTROL mode : %d\n", value);
		break;
	}
}

static int s5k4ecgx_get_lux(int* lux)
{
	int msb = 0;
	int lsb = 0;
	int cur_lux = -1;
	
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	if(s5k4ecgx_status.id == 0x0011)
		s5k4ecgx_sensor_write(0x002E, 0x2C18);//for EVT 1.1
	else
		s5k4ecgx_sensor_write(0x002E, 0x2B30);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&lsb);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&msb);

	cur_lux = (msb<<16) | lsb;
	*lux = cur_lux;
	PCAM_DEBUG("%s, s5k4ecgx_status.current_lux is %d\n", __func__, s5k4ecgx_status.current_lux);
	return cur_lux; //this value is under 0x0032 in low light condition 
}

static  int s5k4ecgx_set_flash(int lux_val)
{
	int i = 0;

	PCAM_DEBUG("%s, flash set is %d\n", __func__, lux_val);
	
	if(s5k4ecgx_status.flash_mode == PCAM_FLASH_OFF)return 0;

	/* initailize falsh IC */
	gpio_set_value(CAM_FLASH_ENSET,0);
	gpio_set_value(CAM_FLASH_FLEN,0);
	mdelay(1); // to enter a shutdown mode
	
	/* set to flash mode */
	if(lux_val>16)
	{
		S5K4ECGX_WRITE_LIST(s5k4ecgx_Main_Flash_On);
		gpio_set_value(CAM_FLASH_FLEN,1);
		s5k4ecgx_status.flash_exifinfo = true;
#ifdef USE_FLASHOFF_TIMER
		add_timer(&flashoff_timer); // for prevent LED 
#endif
	}
	else if(lux_val > 0 &&  lux_val<=16)
	{
		/* set to movie mode */
		S5K4ECGX_WRITE_LIST(s5k4ecgx_Pre_Flash_On);
		for(i=0;i<lux_val;i++)
		{
			udelay(1);
			gpio_set_value(CAM_FLASH_ENSET,1);
			udelay(1);
			gpio_set_value(CAM_FLASH_ENSET,0);
		}
		gpio_set_value(CAM_FLASH_ENSET,1); //value set
	}
	s5k4ecgx_status.flash_status = lux_val;
	
	/* setting a sensor #2*/
		if(lux_val==PRE_FLASH_OFF)S5K4ECGX_WRITE_LIST(s5k4ecgx_Pre_Flash_Off)
		else if(lux_val==FLASH_OFF && s5k4ecgx_status.afcanceled == false)S5K4ECGX_WRITE_LIST(s5k4ecgx_Main_Flash_Off)
		else if(lux_val==FLASH_OFF && s5k4ecgx_status.afcanceled == true)S5K4ECGX_WRITE_LIST(s5k4ecgx_Pre_Flash_Off)
	
	return 0;
}

#ifdef USE_FLASHOFF_TIMER
static void s5k4ecgx_flashoff_timer_handler(unsigned long data)
{
	s5k4ecgx_set_flash(FLASH_OFF);
}
#endif

int s5k4ecgx_set_af(char value)
{
	int val = 0, ret = 0;
	static int pre_flash_on = 0;
	//PCAM_DEBUG("%s : %d\n", __func__, value);
	switch(value)
	{
		case PCAM_AF_CHECK_STATUS :
			s5k4ecgx_sensor_write(0x002C, 0x7000);
			if(s5k4ecgx_status.id == 0x0011)
				s5k4ecgx_sensor_write(0x002E, 0x2EEE);
			else
				s5k4ecgx_sensor_write(0x002E, 0x2E06);
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&val);
			switch(val&0xFF)
			{
				case 1:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_STATUS -PCAM_AF_PROGRESS \n", __func__);
					ret = PCAM_AF_PROGRESS;
				break;
				case 2:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_STATUS -PCAM_AF_SUCCESS \n", __func__);
					ret = PCAM_AF_SUCCESS;
				break;
				default:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_STATUS -PCAM_AF_LOWCONF \n", __func__);
					ret = PCAM_AF_LOWCONF;
				break;
			}
		break;
		case PCAM_AF_CHECK_2nd_STATUS :
			s5k4ecgx_sensor_write(0x002C, 0x7000);
			if(s5k4ecgx_status.id == 0x0011)
				s5k4ecgx_sensor_write(0x002E, 0x2207);
			else
				s5k4ecgx_sensor_write(0x002E, 0x2167);
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&val);
			switch(val&0xFF)
			{
				case 1:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_2nd_STATUS -PCAM_AF_PROGRESS \n", __func__);
					ret = PCAM_AF_PROGRESS;
				break;
				case 0:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_2nd_STATUS -PCAM_AF_SUCCESS \n", __func__);
					ret = PCAM_AF_SUCCESS;
				break;
				default:
					PCAM_DEBUG("%s : PCAM_AF_CHECK_2nd_STATUS -PCAM_AF_PROGRESS \n", __func__);
					ret = PCAM_AF_PROGRESS;
				break;
			}
		break;
		case PCAM_AF_CHECK_AE_STATUS :
			{
				s5k4ecgx_sensor_write(0x002C, 0x7000);
				if(s5k4ecgx_status.id == 0x0011)
					s5k4ecgx_sensor_write(0x002E, 0x2C74);
				else
					s5k4ecgx_sensor_write(0x002E, 0x2B8C);
				s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&val);
				switch(val&0xFF)
				{
					case 1:
						PCAM_DEBUG("%s : PCAM_AF_CHECK_AE_STATUS -PCAM_AE_STABLE \n", __func__);
						ret = PCAM_AE_STABLE;
					break;
					default:
						PCAM_DEBUG("%s : PCAM_AF_CHECK_AE_STATUS -PCAM_AE_UNSTABLE \n", __func__);
						ret = PCAM_AE_UNSTABLE;
					break;
				}
			}
		break;
		case PCAM_AF_SET_NORMAL :
			PCAM_DEBUG("%s : PCAM_AF_SET_NORMAL \n", __func__);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_1);
			if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
			else mdelay(100);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_2);
			if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
			else mdelay(100);
			if(s5k4ecgx_status.scene != PCAM_SCENE_NIGHTSHOT)S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_3);
		break;
		case PCAM_AF_SET_MACRO :
			PCAM_DEBUG("%s : PCAM_AF_SET_MACRO \n", __func__);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_1);
			if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
			else mdelay(100);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_2);
			if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
			else mdelay(100);
			if(s5k4ecgx_status.scene != PCAM_SCENE_NIGHTSHOT)S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_3);
		break;
		case PCAM_AF_OFF :
			PCAM_DEBUG("%s : PCAM_AF_OFF (afmode:%d)\n", __func__,s5k4ecgx_status.afmode);
			s5k4ecgx_status.afcanceled = true;
			if(s5k4ecgx_status.afmode == PCAM_AF_SET_NORMAL)
			{
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_1);
				if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
				else mdelay(100);
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_2);
				if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
				else mdelay(100);
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Normal_mode_3);
			}
			else
			{
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_1);
				if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
				else mdelay(100);
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_2);
				if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT)mdelay(250);
				else mdelay(100);
				S5K4ECGX_WRITE_LIST(s5k4ecgx_AF_Macro_mode_3);
			}
		break;
		case PCAM_AF_DO :
			PCAM_DEBUG("%s : PCAM_AF_DO \n", __func__);
			s5k4ecgx_status.afcanceled = false;
#if 0
			s5k4ecgx_sensor_write(0x002C, 0x7000);
			s5k4ecgx_sensor_write(0x002E, 0x2B30);
			s5k4ecgx_sensor_read(0x0F12, &val);
			
			if(val < 0x001E)
				S5K4ECGX_WRITE_LIST(s5k4ecgx_Low_Cap_On);
			else
				S5K4ECGX_WRITE_LIST(s5k4ecgx_Low_Cap_Off);
#endif
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Single_AF_Start);
		break;
		case PCAM_AF_SET_AE_FOR_FLASH :
			PCAM_DEBUG("%s : PCAM_AF_SET_AE_FOR_FLASH \n", __func__);
			s5k4ecgx_get_lux(&s5k4ecgx_status.current_lux);
			if(s5k4ecgx_status.flash_mode != PCAM_FLASH_OFF)
			{
				if(s5k4ecgx_status.flash_mode == PCAM_FLASH_AUTO)
				{
					if(s5k4ecgx_status.current_lux > 0x0032)break;
				}
				s5k4ecgx_sensor_write(0x0028, 0x7000);
				s5k4ecgx_sensor_write(0x002A, 0x057C);
				s5k4ecgx_sensor_write(0x0F12, 0x0000);
				if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_FACTORY_TEST || s5k4ecgx_status.afmode == PCAM_AF_SET_MACRO)
					s5k4ecgx_set_flash(MACRO_FLASH);
				else
					s5k4ecgx_set_flash(PRE_FLASH);
				mdelay(100);
				pre_flash_on = 1;
			}
		break;
		case PCAM_AF_BACK_AE_FOR_FLASH :
			PCAM_DEBUG("%s : PCAM_AF_BACK_AE_FOR_FLASH \n", __func__);
			if(pre_flash_on)
			{
				s5k4ecgx_sensor_write(0x0028, 0x7000);
				s5k4ecgx_sensor_write(0x002A, 0x057C);
				s5k4ecgx_sensor_write(0x0F12, 0x0002);
			}
			s5k4ecgx_set_flash(PRE_FLASH_OFF);
			pre_flash_on=0;
		break;
		default :
			printk("[S5K4ECGX] Unexpected AF command : %d\n",value);
		break;
	}	
	return ret;
}

void s5k4ecgx_set_DTP(char value)
{
	switch(value)
	{
		case PCAM_DTP_OFF:
			S5K4ECGX_WRITE_LIST(s5k4ecgx_DTP_stop);
		break;
		case PCAM_DTP_ON:
			S5K4ECGX_WRITE_LIST(s5k4ecgx_DTP_init);
		break;
		default:
			printk("[S5K4ECGX] Unexpected DTP control on PCAM\n");
		break;
	}
}

void s5k4ecgx_set_ae_lock(char value)
{
	switch(value)
	{
		case PCAM_AE_LOCK :
			s5k4ecgx_status.ae_lock = PCAM_AE_LOCK;
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ae_lock);
		break;
		case PCAM_AE_UNLOCK :
			s5k4ecgx_status.ae_lock = PCAM_AE_UNLOCK;
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ae_unlock);
		break;
		case PCAM_AWB_LOCK :
			s5k4ecgx_status.awb_lock = PCAM_AWB_LOCK;
			S5K4ECGX_WRITE_LIST(s5k4ecgx_awb_lock);
		break;
		case PCAM_AWB_UNLOCK :
			s5k4ecgx_status.awb_lock = PCAM_AWB_UNLOCK;
			S5K4ECGX_WRITE_LIST(s5k4ecgx_awb_unlock);
		break;
		default :
			printk("[S5K4ECGX]Unexpected AWB_AE mode : %d\n", value);
		break;
	}
}

void s5k4ecgx_set_scene(char value)
{
	int REG_TC_DBG_AutoAlgEnBits = 0;
	
	/* Read 04E6 */
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x04E6);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&REG_TC_DBG_AutoAlgEnBits);
	
	if(value != PCAM_SCENE_OFF)
	{
		S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Default);
		//if(value == PCAM_SCENE_TEXT)s5k4ecgx_set_af(PCAM_AF_SET_MACRO);
		//else s5k4ecgx_set_af(PCAM_AF_SET_NORMAL);
		if( s5k4ecgx_status.auto_contrast != PCAM_AUTO_CONTRAST_OFF)s5k4ecgx_set_auto_contrast(PCAM_AUTO_CONTRAST_OFF);
	}
	
	switch(value)
	{
		case PCAM_SCENE_OFF :	
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Default);
			//s5k4ecgx_set_af(s5k4ecgx_status.afmode);
			if(s5k4ecgx_status.effect != PCAM_EFFECT_NORMAL)s5k4ecgx_set_effect(s5k4ecgx_status.effect);
			if(s5k4ecgx_status.brightness != PCAM_BR_STEP_0)s5k4ecgx_set_brightness(s5k4ecgx_status.brightness);
			if(s5k4ecgx_status.auto_exposure != PCAM_METERING_NORMAL)s5k4ecgx_set_metering(s5k4ecgx_status.auto_exposure);
			if(s5k4ecgx_status.iso != PCAM_ISO_AUTO)s5k4ecgx_set_iso(s5k4ecgx_status.iso);
			if(s5k4ecgx_status.contrast != PCAM_CR_STEP_0 )s5k4ecgx_set_contrast(s5k4ecgx_status.contrast);
			if(s5k4ecgx_status.saturation != PCAM_SA_STEP_0)s5k4ecgx_set_saturation(s5k4ecgx_status.saturation);
			if(s5k4ecgx_status.sharpness != PCAM_SP_STEP_0)s5k4ecgx_set_sharpness(s5k4ecgx_status.sharpness);
			if(s5k4ecgx_status.whiteBalance != PCAM_WB_AUTO)s5k4ecgx_set_whitebalance(s5k4ecgx_status.whiteBalance);
			if(s5k4ecgx_status.auto_contrast != PCAM_AUTO_CONTRAST_OFF)s5k4ecgx_set_auto_contrast(s5k4ecgx_status.auto_contrast);
		break;
		case PCAM_SCENE_PORTRAIT :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Portrait);
		break;
		case PCAM_SCENE_LANDSCAPE :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Landscape);
			s5k4ecgx_set_metering(PCAM_METERING_NORMAL);
		break;
		case PCAM_SCENE_SPORTS :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Sports);
		break;
		case PCAM_SCENE_PARTY :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);	
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Party_Indoor);
		break;
		case PCAM_SCENE_BEACH :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFDF;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);	
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Beach_Snow);
		break;
		case PCAM_SCENE_SUNSET :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Sunset);
		break;
		case PCAM_SCENE_DAWN :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Duskdawn);
		break;
		case PCAM_SCENE_FALL :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Fall_Color);
		break;
		case PCAM_SCENE_NIGHTSHOT :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Nightshot);
		break;
		case PCAM_SCENE_BACKLIGHT :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Backlight);
			//if(s5k4ecgx_status.flash_mode == PCAM_FLASH_ON ||s5k4ecgx_status.flash_mode == PCAM_FLASH_AUTO)s5k4ecgx_set_metering(PCAM_METERING_CENTER);
			//else s5k4ecgx_set_metering(PCAM_METERING_SPOT);
		break;
		case PCAM_SCENE_FIREWORK :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Fireworks);
			S5K4ECGX_WRITE_LIST(s5k4ecgx_ISO_50);
		break;
		case PCAM_SCENE_TEXT :
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Text);
		break;
		case PCAM_SCENE_CANDLE :
			REG_TC_DBG_AutoAlgEnBits = REG_TC_DBG_AutoAlgEnBits & 0xFFF7;
			s5k4ecgx_sensor_write(0x0028, 0x7000);
			s5k4ecgx_sensor_write(0x002A, 0x04E6);
			s5k4ecgx_sensor_write(0x0F12, REG_TC_DBG_AutoAlgEnBits);			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Scene_Candle_Light);
		break;
		default :
			printk("[S5K4ECGX]Unexpected SCENE mode : %d\n",  value);
		break;
	}
}

void s5k4ecgx_set_capture_size(int size)
{
	PCAM_DEBUG("s5k4ecgx_set_capture_size %d \n",size );

	if(size == PCAM_SNAPSHOT_SIZE_2560x1920_5M)
	{
		S5K4ECGX_WRITE_LIST(s5k4ecgx_5M_Capture);
		s5k4ecgx_set_zoom(PCAM_ZOOM_STEP_0);
	}
	else if(size == PCAM_SNAPSHOT_SIZE_2048x1536_3M)S5K4ECGX_WRITE_LIST(s5k4ecgx_3M_Capture)
	else if(size == PCAM_SNAPSHOT_SIZE_1600x1200_2M)S5K4ECGX_WRITE_LIST(s5k4ecgx_2M_Capture)
	else if(size == PCAM_SNAPSHOT_SIZE_1280x960_1M)S5K4ECGX_WRITE_LIST(s5k4ecgx_1M_Capture)
	else if(size == PCAM_SNAPSHOT_SIZE_640x480_VGA)S5K4ECGX_WRITE_LIST(s5k4ecgx_VGA_Capture)
	else if(size == PCAM_SNAPSHOT_SIZE_320x240_QVGA)S5K4ECGX_WRITE_LIST(s5k4ecgx_QVGA_Capture)
	else printk("[S5K4ECGX]s5k4ecgx_set_capture : wrong size!!! \n");
	if(size != PCAM_SNAPSHOT_SIZE_2560x1920_5M && s5k4ecgx_status.zoom != PCAM_ZOOM_STEP_0)s5k4ecgx_set_zoom(s5k4ecgx_status.zoom);
}

void s5k4ecgx_set_preview_size(int size)
{
	PCAM_DEBUG("s5k4ecgx_set_capture_size %d \n",size );

	if(size == PCAM_PREVIEW_SIZE_640x480_VGA)S5K4ECGX_WRITE_LIST(s5k4ecgx_640_Preview)
	else if(size == PCAM_PREVIEW_SIZE_176x144_QCIF)S5K4ECGX_WRITE_LIST(s5k4ecgx_640_Preview)
	else if(size == PCAM_PREVIEW_SIZE_320x240_QVGA)S5K4ECGX_WRITE_LIST(s5k4ecgx_176_Preview)
	else if(size == PCAM_PREVIEW_SIZE_720x480_D1)S5K4ECGX_WRITE_LIST(s5k4ecgx_720_Preview)
	else printk("[S5K4ECGX]s5k4ecgx_set_capture : wrong size!!! \n");
}

void s5k4ecgx_set_preview(void)
{
	printk("[S5K4ECGX]s5k4ecgx_set_preview start\n");

//#define USE_EVT0_SENSOR
#ifdef USE_EVT0_SENSOR
	unsigned short id = 0;
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x01A6);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&id);
#endif

	if(s5k4ecgx_status.camera_initailized == false)
	{
#ifdef USE_EVT0_SENSOR
		if(id == 0x0001)
		{
#ifdef CONFIG_LOAD_FILE
			S5K4ECGX_WRITE_LIST(s5k4ecgx_init_reg1);
			msleep(100);
#endif
			S5K4ECGX_WRITE_LIST(s5k4ecgx_init_reg2);
		}
		 else
			S5K4ECGX_WRITE_LIST(s5k4ecgx_init0);
#else
#ifdef CONFIG_LOAD_FILE
		S5K4ECGX_WRITE_LIST(s5k4ecgx_init_reg1);
		msleep(100);
#endif
		S5K4ECGX_WRITE_LIST(s5k4ecgx_init_reg2);
#endif
	} 
	else if(s5k4ecgx_status.camera_initailized == true)
	{
		if(!isPreviewReturnWrite)
		{
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Preview_Return);
			isPreviewReturnWrite = true;
		}
	}
	
	/* workaround code for late arrival preview frame on FIREWORK mode */
	if(s5k4ecgx_status.scene == PCAM_SCENE_FIREWORK)msleep(600);
	
#if 0 //disable Frmae skip
		if((s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT) ||(s5k4ecgx_status.scene == PCAM_SCENE_FIREWORK) )
		{
			for(cnt=0; cnt<450; cnt++)
			{
				vsync_value = gpio_get_value(14);
				if(vsync_value)
					break;
				else
				{
					PCAM_DEBUG("wait cnt:%d vsync_value:%d\n", cnt, vsync_value);
					msleep(3);
				}
			}
		}
#endif
#define DATA_LINE_CHECK
#ifdef DATA_LINE_CHECK
	if(s5k4ecgx_status.need_configuration & CHECK_DTP&& s5k4ecgx_status.dtp != PCAM_DTP_OFF)s5k4ecgx_set_DTP(s5k4ecgx_status.dtp);
#endif
	S5K4ECGX_WRITE_LIST(s5k4ecgx_Flash_init);
	PCAM_DEBUG("[S5K4ECGX] sensor configuration start\n");
	//S5K4ECGX_WRITE_LIST(s5k4ecgx_awb_ae_unlock);
	if(s5k4ecgx_status.scene == PCAM_SCENE_OFF)
	{
		if(s5k4ecgx_status.need_configuration & CHECK_EFFECT && s5k4ecgx_status.effect != PCAM_EFFECT_NORMAL)s5k4ecgx_set_effect(s5k4ecgx_status.effect);
		if(s5k4ecgx_status.need_configuration & CHECK_BRIGHTNESS && s5k4ecgx_status.brightness != PCAM_BR_STEP_0)s5k4ecgx_set_brightness(s5k4ecgx_status.brightness);
		if(s5k4ecgx_status.need_configuration & CHECK_AE && s5k4ecgx_status.auto_exposure != PCAM_METERING_CENTER)s5k4ecgx_set_metering(s5k4ecgx_status.auto_exposure);
		if(s5k4ecgx_status.need_configuration & CHECK_ISO && s5k4ecgx_status.iso != PCAM_ISO_AUTO)s5k4ecgx_set_iso(s5k4ecgx_status.iso);
		if(s5k4ecgx_status.need_configuration & CHECK_CONTRAST && s5k4ecgx_status.contrast != PCAM_CR_STEP_0 )s5k4ecgx_set_contrast(s5k4ecgx_status.contrast);
		if(s5k4ecgx_status.need_configuration & CHECK_SATURATION && s5k4ecgx_status.saturation != PCAM_SA_STEP_0)s5k4ecgx_set_saturation(s5k4ecgx_status.saturation);
		if(s5k4ecgx_status.need_configuration & CHECK_SHARPNESS && s5k4ecgx_status.sharpness != PCAM_SP_STEP_0)s5k4ecgx_set_sharpness(s5k4ecgx_status.sharpness);
		if(s5k4ecgx_status.need_configuration & CHECK_WB && s5k4ecgx_status.whiteBalance != PCAM_WB_AUTO)s5k4ecgx_set_whitebalance(s5k4ecgx_status.whiteBalance);
		if(s5k4ecgx_status.need_configuration & CHECK_AFMODE && s5k4ecgx_status.camera_initailized == false)s5k4ecgx_set_af(s5k4ecgx_status.afmode);
		if(s5k4ecgx_status.need_configuration & CHECK_AUTOCONTRAST && s5k4ecgx_status.auto_contrast != PCAM_AUTO_CONTRAST_OFF)s5k4ecgx_set_auto_contrast(s5k4ecgx_status.auto_contrast);
	}
	else 
	{
		s5k4ecgx_set_scene(s5k4ecgx_status.scene);
	}
	if(s5k4ecgx_status.need_configuration & CHECK_FPS)s5k4ecgx_set_fps(s5k4ecgx_status.fps);
	if(s5k4ecgx_status.need_configuration & CHECK_ZOOM && s5k4ecgx_status.zoom != PCAM_ZOOM_STEP_0)s5k4ecgx_set_zoom(s5k4ecgx_status.zoom);
	if(s5k4ecgx_status.need_configuration & CHECK_JPEGQUALITY && s5k4ecgx_status.jpeg_quality != PCAM_JPEG_QUALITY_SUPERFINE)s5k4ecgx_set_jpeg_quality(s5k4ecgx_status.jpeg_quality);
	if(s5k4ecgx_status.need_configuration & CHECK_SNAPSHOT_SIZE && s5k4ecgx_status.snapshot_size !=PCAM_SNAPSHOT_SIZE_2560x1920_5M)s5k4ecgx_set_capture_size(s5k4ecgx_status.snapshot_size);

	/* reset status*/
	s5k4ecgx_status.preview_size = PCAM_PREVIEW_SIZE_640x480_VGA;
	s5k4ecgx_status.flash_exifinfo = false;
	s5k4ecgx_status.flash_status = FLASH_OFF;
	s5k4ecgx_status.camera_initailized = true;
	s5k4ecgx_status.lowcap_on= false;
	s5k4ecgx_status.nightcap_on= false;
	s5k4ecgx_status.afcanceled = false;
	s5k4ecgx_status.camera_status = PREVIEW;
}

void s5k4ecgx_check_REG_TC_GP_EnableCaptureChanged(void)
{
	int cnt = 0;
	int REG_TC_GP_EnableCaptureChanged = 0;

	while(cnt < 50)
	{
		s5k4ecgx_sensor_write(0x002C, 0x7000);
		s5k4ecgx_sensor_write(0x002E, 0x0244);	
		s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&REG_TC_GP_EnableCaptureChanged);
		if(!REG_TC_GP_EnableCaptureChanged)break;
		mdelay(10);
		cnt++;
	}	
	if(cnt)printk("[S5K4ECGX] wait time for capture frame : %dms\n",cnt*10);
	if(REG_TC_GP_EnableCaptureChanged)printk("[S5K4ECGX] take picture failed.\n");
}

void s5k4ecgx_set_capture(void)
{
	isPreviewReturnWrite = false;
	s5k4ecgx_status.camera_status = SNAPSHOT;
	
	/* Check current lux */
	if(s5k4ecgx_status.flash_status == FLASH_OFF)s5k4ecgx_get_lux(&s5k4ecgx_status.current_lux);

	/* CASE 1 : Capture with no flash (System lag will be measured in this condition)*/
	if(s5k4ecgx_status.flash_mode != PCAM_FLASH_ON && s5k4ecgx_status.current_lux > 0x0032)
	{
		S5K4ECGX_WRITE_LIST(s5k4ecgx_Capture_Start);
		s5k4ecgx_check_REG_TC_GP_EnableCaptureChanged();
		return;
	}
	/* CASE 2 : Flash turn on  in normal light condition*/
	else if(s5k4ecgx_status.flash_mode == PCAM_FLASH_ON && s5k4ecgx_status.current_lux > 0x0032)
	{
		/* ae/awb unlock */
		if(s5k4ecgx_status.ae_lock == PCAM_AE_LOCK)s5k4ecgx_set_ae_lock(PCAM_AE_UNLOCK);
		if(s5k4ecgx_status.awb_lock == PCAM_AWB_LOCK)s5k4ecgx_set_ae_lock(PCAM_AWB_UNLOCK);

		/* use torch mode on below condition */
		if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_FACTORY_TEST || s5k4ecgx_status.afmode == PCAM_AF_SET_MACRO)
			s5k4ecgx_set_flash(MACRO_FLASH);
		else
			s5k4ecgx_set_flash(FULL_FLASH);
		mdelay(200);
		
		S5K4ECGX_WRITE_LIST(s5k4ecgx_Capture_Start);
		s5k4ecgx_check_REG_TC_GP_EnableCaptureChanged();
		return;
	}
	/* Flash control in low light condition*/
	else
	{
		/* CASE 3 : flash off on below scene mode */
		if(s5k4ecgx_status.scene == PCAM_SCENE_NIGHTSHOT ||s5k4ecgx_status.scene == PCAM_SCENE_FIREWORK)
		{
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Night_Mode_On);
			mdelay(250);
			
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Capture_Start);
			s5k4ecgx_check_REG_TC_GP_EnableCaptureChanged();
			s5k4ecgx_status.nightcap_on= true;
			return;
		}
		else
		/* CASE 4 : flash auto mode & low light condition */
		{
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Low_Cap_On);
			if(s5k4ecgx_status.flash_mode != PCAM_FLASH_OFF)
			{
				/* ae/awb unlock */
				if(s5k4ecgx_status.ae_lock == PCAM_AE_LOCK)s5k4ecgx_set_ae_lock(PCAM_AE_UNLOCK);
				if(s5k4ecgx_status.awb_lock == PCAM_AWB_LOCK)s5k4ecgx_set_ae_lock(PCAM_AWB_UNLOCK);

				/* use torch mode on below condition */
				if(s5k4ecgx_status.camera_mode == PCAM_CAM_MODE_FACTORY_TEST || s5k4ecgx_status.afmode == PCAM_AF_SET_MACRO)
					s5k4ecgx_set_flash(MACRO_FLASH);
				else
					s5k4ecgx_set_flash(FULL_FLASH);
			}
			if(s5k4ecgx_status.flash_exifinfo == true)mdelay(200);
			else mdelay(120);

			S5K4ECGX_WRITE_LIST(s5k4ecgx_Capture_Start);
			s5k4ecgx_check_REG_TC_GP_EnableCaptureChanged();
			s5k4ecgx_status.lowcap_on= true;
			return;
		}
	}
}

void sensor_rough_control(void __user *arg)
{

	ioctl_pcam_info_8bit	ctrl_info;

	if(copy_from_user((void *)&ctrl_info, (const void *)arg, sizeof(ctrl_info)))
	{
		PCAM_DEBUG("%s fail copy_from_user!\n", __func__);
	}

/*
	PCAM_DEBUG("TEST %d %d %d %d %d \n", ctrl_info.mode, ctrl_info.address,\
	 ctrl_info.value_1, ctrl_info.value_2, ctrl_info.value_3);
*/
	switch(ctrl_info.mode)
	{
		case PCAM_AUTO_TUNNING:
			S5K4ECGX_WRITE_LIST(s5k4ecgx_Preview_Return);
			isPreviewReturnWrite = true;
		break;
		case PCAM_SDCARD_DETECT:
		break;
		case PCAM_GET_INFO:{
			unsigned short lsb, msb,a_gain,d_gain;
			s5k4ecgx_sensor_write(0xFCFC, 0xD000);
			s5k4ecgx_sensor_write(0x002C, 0x7000);
			if(s5k4ecgx_status.id == 0x0011)
				s5k4ecgx_sensor_write(0x002E, 0x2BC0);
			else
				s5k4ecgx_sensor_write(0x002E, 0x2AD8);
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&lsb);//8
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&msb);//A
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&a_gain);//C
			s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&d_gain);//E
			ctrl_info.value_1 = lsb;
			ctrl_info.value_2 = msb;
			ctrl_info.value_3 = a_gain;
			ctrl_info.address = d_gain;
			PCAM_DEBUG("exposure %x %x \n", lsb, msb);
			//PCAM_DEBUG("rough_iso %x \n", rough_iso);
			if(s5k4ecgx_status.nightcap_on== true)S5K4ECGX_WRITE_LIST(s5k4ecgx_Night_Mode_Off)
			else if(s5k4ecgx_status.lowcap_on== true)S5K4ECGX_WRITE_LIST(s5k4ecgx_Low_Cap_Off)
		}
		break;
		case PCAM_FLASH_INFO:
			if(s5k4ecgx_status.flash_mode != PCAM_FLASH_OFF 
				&& s5k4ecgx_status.flash_status != FLASH_OFF 
				&& s5k4ecgx_status.flash_status != PRE_FLASH_OFF)
			{
				s5k4ecgx_set_flash(FLASH_OFF);
			}
			PCAM_DEBUG("PCAM_FLASH_INFO %d \n", s5k4ecgx_status.flash_exifinfo);
			ctrl_info.value_1 = s5k4ecgx_status.flash_exifinfo;
		break;
		case PCAM_LUX_INFO:
			ctrl_info.value_1 = s5k4ecgx_status.current_lux;
		break;		
		case PCAM_FRAME_CONTROL:
			s5k4ecgx_status.fps = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_FPS;
			else
				s5k4ecgx_set_fps(s5k4ecgx_status.fps);
		break;
		case PCAM_AF_CONTROL:
			if(ctrl_info.value_1 ==  2|| ctrl_info.value_1 == 3)s5k4ecgx_status.afmode= ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_AFMODE;
			else
				ctrl_info.value_3 = s5k4ecgx_set_af(ctrl_info.value_1);
		break;
		case PCAM_EFFECT_CONTROL:
			s5k4ecgx_status.effect = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_EFFECT;
			else
				s5k4ecgx_set_effect(s5k4ecgx_status.effect);
		break;
		case PCAM_WB_CONTROL:
			s5k4ecgx_status.whiteBalance = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_WB;
			else
				s5k4ecgx_set_whitebalance(s5k4ecgx_status.whiteBalance);
		break;
		case PCAM_BR_CONTROL:
			s5k4ecgx_status.brightness = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_BRIGHTNESS;
			else
				s5k4ecgx_set_brightness(s5k4ecgx_status.brightness);
		break;
		case PCAM_ISO_CONTROL:
			s5k4ecgx_status.iso = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_ISO;
			else
				s5k4ecgx_set_iso(s5k4ecgx_status.iso);
		break;
		case PCAM_METERING_CONTROL:
			s5k4ecgx_status.auto_exposure = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_AE;
			else
				s5k4ecgx_set_metering(s5k4ecgx_status.auto_exposure);
		break;
		case PCAM_SCENE_CONTROL:
			s5k4ecgx_status.scene = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration|= CHECK_SCENE;
			else
				s5k4ecgx_set_scene(s5k4ecgx_status.scene);
		break;
		case PCAM_AE_AWB_CONTROL:
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration|= CHECK_AE_AWB_LOCK;
			else
				s5k4ecgx_set_ae_lock(ctrl_info.value_1);
		break;
		case PCAM_CR_CONTROL:
			s5k4ecgx_status.contrast = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration|= CHECK_CONTRAST;
			else
				s5k4ecgx_set_contrast(s5k4ecgx_status.contrast);
		break;
		case PCAM_SA_CONTROL:
			s5k4ecgx_status.saturation = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_SATURATION;
			else
				s5k4ecgx_set_saturation(s5k4ecgx_status.saturation);
		break;
		case PCAM_SP_CONTROL:
			s5k4ecgx_status.sharpness = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_SHARPNESS;
			else
				s5k4ecgx_set_sharpness(s5k4ecgx_status.sharpness);
		break;
		case PCAM_CPU_CONTROL:
			switch(ctrl_info.value_1)
			{
				case PCAM_CPU_CONSERVATIVE:
				PCAM_DEBUG("now conservative\n");
				cpufreq_direct_set_policy(0, "conservative");
				break;
				case PCAM_CPU_ONDEMAND:
				PCAM_DEBUG("now ondemand\n");
				cpufreq_direct_set_policy(0, "ondemand");
				break;
				case PCAM_CPU_PERFORMANCE:
				PCAM_DEBUG("now performance\n");
				cpufreq_direct_set_policy(0, "performance");
				break;
				default:
					printk("[S5K4ECGX] Unexpected CPU control on PCAM\n");
				break;
			}
		break;
		case PCAM_DTP_CONTROL:
			s5k4ecgx_status.dtp = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_DTP;
			else
				s5k4ecgx_set_DTP(s5k4ecgx_status.dtp);
			if(ctrl_info.value_1 == 0)
					ctrl_info.value_3 = 2;
			else if(ctrl_info.value_1 == 1)
					ctrl_info.value_3 = 3;
		break;
		case PCAM_SNAPSHOT_SIZE_CONTROL:
			s5k4ecgx_status.snapshot_size = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_SNAPSHOT_SIZE;
			else
				s5k4ecgx_set_capture_size(s5k4ecgx_status.snapshot_size);
		break;
		case PCAM_SET_CAPTURE_MODE:
			//s5k4ecgx_set_capture();
		break;
		case PCAM_SET_FLASH_MODE:
			if(ctrl_info.value_1 != PCAM_FLASH_TURN_ON && ctrl_info.value_1 != PCAM_FLASH_TURN_OFF)
			{
				s5k4ecgx_status.flash_mode = ctrl_info.value_1;
				//if(s5k4ecgx_status.scene == PCAM_SCENE_BACKLIGHT && s5k4ecgx_status.flash_mode != PCAM_FLASH_OFF)s5k4ecgx_set_metering(PCAM_METERING_CENTER);
				//else if(s5k4ecgx_status.scene == PCAM_SCENE_BACKLIGHT && s5k4ecgx_status.flash_mode == PCAM_FLASH_OFF)s5k4ecgx_set_metering(PCAM_METERING_SPOT);
			}
			else if(ctrl_info.value_1 == PCAM_FLASH_TURN_ON)
			{
				if(s5k4ecgx_status.flash_mode == PCAM_FLASH_AUTO)
				{
					s5k4ecgx_get_lux(&s5k4ecgx_status.current_lux);
					if(s5k4ecgx_status.current_lux < 0x0032)s5k4ecgx_set_flash(MOVIE_FLASH);
				}
				else
					s5k4ecgx_set_flash(MOVIE_FLASH);
			}
			else if(ctrl_info.value_1 == PCAM_FLASH_TURN_OFF)
				s5k4ecgx_set_flash(FLASH_OFF);
		break;
		case PCAM_JPEG_QUALITY_CONTROL:
			s5k4ecgx_status.jpeg_quality = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_JPEGQUALITY;
			else
				s5k4ecgx_set_jpeg_quality(ctrl_info.value_1);
		break;
		case PCAM_AUTO_CONTRAST_CONTROL:
			s5k4ecgx_status.auto_contrast = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_AUTOCONTRAST;
			else
				s5k4ecgx_set_auto_contrast(ctrl_info.value_1);
		break;
		case PCAM_ZOOM_CONTROL:
			s5k4ecgx_status.zoom = ctrl_info.value_1;
			if(!s5k4ecgx_status.camera_initailized)
				s5k4ecgx_status.need_configuration |= CHECK_ZOOM;
			else
				s5k4ecgx_set_zoom(ctrl_info.value_1);
		break;
		case PCAM_CAM_MODE_CONTROL:
			s5k4ecgx_status.camera_mode = ctrl_info.value_1;
			PCAM_DEBUG("CAMERA MODE : %d\n",s5k4ecgx_status.camera_mode);
		break;
		default :
			printk("[S5K4ECGX]Unexpected mode on sensor_rough_control : %d\n", ctrl_info.mode);
		break;
	}

	if(copy_to_user((void *)arg, (const void *)&ctrl_info, sizeof(ctrl_info)))
	{
		printk("[S5K4ECGX]%s fail on copy_to_user!\n", __func__);
	}
}

void cam_pw(int status)
{
	printk("[S5K4ECGX]We does not use this function anymore.\n");
}

void s5k4ecgx_set_power(int status)
{
	struct vreg *vreg_cam_out8;
	struct vreg *vreg_cam_out9;
	struct vreg *vreg_cam_out10;

	vreg_cam_out8 = vreg_get(NULL, "ldo8"); // VDDIO 2.8v
	vreg_cam_out9 = vreg_get(NULL, "ldo9"); // VDDS 2.8v
	vreg_cam_out10 = vreg_get(NULL, "ldo10"); // AF 2.8v

	if(status == 1) //POWER ON
	{
		printk("[S5K4ECGX]Camera Sensor Power ON [%d %d]\n",i2c_retry,probe_init_retry);
		
		/*initailize power control pin*/	
		gpio_set_value(0, 0);
		/* initailize flash IC */
		gpio_set_value(CAM_FLASH_ENSET,0);
		gpio_set_value(CAM_FLASH_FLEN,0);
		
		vreg_set_level(vreg_cam_out10, OUT2800mV);
		vreg_set_level(vreg_cam_out9,  OUT2800mV);
		vreg_set_level(vreg_cam_out8,  OUT2800mV);

		gpio_set_value(3,1); //VDDD
		msleep(1);
		vreg_enable(vreg_cam_out9);//VDDS
		msleep(1);
		vreg_enable(vreg_cam_out8);// VDDIO
		msleep(1);
		vreg_enable(vreg_cam_out10);//AF
	}
	else //POWER OFF
	{
		printk("[S5K4ECGX]Camera Sensor Power OFF\n");
		
		s5k4ecgx_status.power_on = false;
		vreg_disable(vreg_cam_out8);// VDDS
		udelay(1);
		vreg_disable(vreg_cam_out9); //VDDIO
		udelay(1);
		gpio_set_value(3,0); //VDDD
		udelay(1);
		vreg_disable(vreg_cam_out10); //AF
		
		/*initailize power control pin*/	
		gpio_set_value(0, 0);
		/* initailize flash IC */
		gpio_set_value(CAM_FLASH_ENSET,0);
		gpio_set_value(CAM_FLASH_FLEN,0);
		mdelay(1); // to enter a shutdown mode		
	}
}

static int s5k4ecgx_probe_init_sensor()
{
	int rc = 0;
	printk("[S5K4ECGX]s5k4ecgx_probe_init_sensor()\n");
	
	/* CAM RESET(GPIO 0) set to HIGH */
	gpio_set_value(0, 1);  
	msleep(15);

	s5k4ecgx_status.power_on = true;
	s5k4ecgx_status.id = 0x11;

#ifdef NOT_USE
	rc = s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x01A6);
	s5k4ecgx_sensor_read(0x0F12, (unsigned short*)&s5k4ecgx_status.id);
	PCAM_DEBUG("SENSOR FW VERSION : 0x%x \n",s5k4ecgx_status.id);
#endif

#ifndef CONFIG_LOAD_FILE
	//S5K4ECGX_WRITE_LIST(s5k4ecgx_init_reg1);
	rc = s5k4ecgx_sensor_burst_write(s5k4ecgx_init_reg1_EVT1,
			(sizeof(s5k4ecgx_init_reg1_EVT1) / sizeof(s5k4ecgx_init_reg1_EVT1[0])),
			"s5k4ecgx_init_reg1_EVT1");
	msleep(10);
#endif

#if 0//PGH I2C SPEED TEST
	unsigned int	before_time, after_time, i;//I2C SPEED TEST
	before_time = get_jiffies_64();
	for (i = 0; i < 3000; i++) 
	{
		s5k4ecgx_sensor_write(0x002E, 0x0040);
	}       
	after_time = get_jiffies_64();
	PCAM_DEBUG("Total Time 3000: %d\n",  jiffies_to_msecs(after_time-before_time));
#endif//PGH I2C SPEED TEST

#if 0
	unsigned short id = 0; //CAM FOR FW
	s5k4ecgx_sensor_write(0x002C, 0x7000);
	s5k4ecgx_sensor_write(0x002E, 0x01AC);
	s5k4ecgx_sensor_read(0x0F12, &id);
	PCAM_DEBUG("SENSOR FW VERSION : 0x%x \n", id);
#endif
	return rc;
}

#if 0//PGH
static long s5k4ecgx_reg_init(void)
{
	int32_t array_length;
	int32_t i;
	long rc;

	/* PLL Setup Start */
	rc = s5k4ecgx_i2c_write_table(&s5k4ecgx_regs.plltbl[0],
					s5k4ecgx_regs.plltbl_size);

	if (rc < 0)
		return rc;
	/* PLL Setup End   */

	array_length = s5k4ecgx_regs.prev_snap_reg_settings_size;

	/* Configure sensor for Preview mode and Snapshot mode */
	for (i = 0; i < array_length; i++) {
		rc = s5k4ecgx_i2c_write(s5k4ecgx_client->addr,
		  s5k4ecgx_regs.prev_snap_reg_settings[i].register_address,
		  s5k4ecgx_regs.prev_snap_reg_settings[i].register_value,
		  WORD_LEN);

		if (rc < 0)
			return rc;
	}

	/* Configure for Noise Reduction, Saturation and Aperture Correction */
	array_length = s5k4ecgx_regs.noise_reduction_reg_settings_size;

	for (i = 0; i < array_length; i++) {
		rc = s5k4ecgx_i2c_write(s5k4ecgx_client->addr,
			s5k4ecgx_regs.noise_reduction_reg_settings[i].register_address,
			s5k4ecgx_regs.noise_reduction_reg_settings[i].register_value,
			WORD_LEN);

		if (rc < 0)
			return rc;
	}

	/* Set Color Kill Saturation point to optimum value */
	rc =
	s5k4ecgx_i2c_write(s5k4ecgx_client->addr,
	0x35A4,
	0x0593,
	WORD_LEN);
	if (rc < 0)
		return rc;

	rc = s5k4ecgx_i2c_write_table(&s5k4ecgx_regs.stbl[0],
					s5k4ecgx_regs.stbl_size);
	if (rc < 0)
		return rc;

	rc = s5k4ecgx_set_lens_roll_off();
	if (rc < 0)
		return rc;

	return 0;
}
#endif//PGH

//static int16_t s5k4ecgx_effect = CAMERA_EFFECT_OFF;
static long s5k4ecgx_config_effect(int mode, int effect)
{
	return 0;
/*
	long rc = 0;
	switch (mode) {
		case SENSOR_PREVIEW_MODE:
			//PCAM_DEBUG("SENSOR_PREVIEW_MODE\n");
		break;
		case SENSOR_SNAPSHOT_MODE:
			//PCAM_DEBUG("SENSOR_SNAPSHOT_MODE\n");
		break;
		default:
			//PCAM_DEBUG("[PGH] %s default\n", __func__);
		break;
	}

	switch (effect) {
		case CAMERA_EFFECT_OFF: 
			//PCAM_DEBUG("CAMERA_EFFECT_OFF\n");
		break;
		case CAMERA_EFFECT_MONO: 
			//PCAM_DEBUG("CAMERA_EFFECT_MONO\n");
		break;
		case CAMERA_EFFECT_NEGATIVE:
			//PCAM_DEBUG("CAMERA_EFFECT_NEGATIVE\n");
		break;
		case CAMERA_EFFECT_SOLARIZE:
			//PCAM_DEBUG("CAMERA_EFFECT_SOLARIZE\n");
		break;
		case CAMERA_EFFECT_SEPIA:
			//PCAM_DEBUG("CAMERA_EFFECT_SEPIA\n");
		break;
		default: 
			//printk("[S5K4ECGX]unexpeceted effect  %s/%d\n", __func__, __LINE__);
		return -EINVAL;
	}
	s5k4ecgx_effect = effect;
	
	return rc;
	*/
}

static long s5k4ecgx_set_sensor_mode(int mode)
{
	PCAM_DEBUG("s5k4ecgx_set_sensor_mode : %d\n",mode);
	switch (mode) 
	{
		case SENSOR_PREVIEW_MODE:
			s5k4ecgx_set_preview();
		break;
		case SENSOR_SNAPSHOT_MODE:
		case SENSOR_RAW_SNAPSHOT_MODE:
			if(s5k4ecgx_status.camera_status != SNAPSHOT)s5k4ecgx_set_capture();
		break;
		default:
			return -EINVAL;
	}
	return 0;
}

#ifdef CONFIG_LOAD_FILE
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <asm/uaccess.h>

static char *s5k4ecgx_regs_table = NULL;
static int s5k4ecgx_regs_table_size;

void s5k4ecgx_regs_table_init(void)
{
	struct file *filp;
	char *dp;
	long l;
	loff_t pos;
	int ret;
	mm_segment_t fs = get_fs();

	PCAM_DEBUG("%s %d\n", __func__, __LINE__);
	set_fs(get_ds());
#if 0
	filp = filp_open("/data/camera/s5k4ecgx.h", O_RDONLY, 0);
#else
	if(s5k4ecgx_status.id == 0x0011)
		filp = filp_open("/mnt/sdcard/s5k4ecgx_evt1.h", O_RDONLY, 0);
	else
		filp = filp_open("/mnt/sdcard/s5k4ecgx.h", O_RDONLY, 0);
#endif
	if (IS_ERR(filp)) {
		printk("[S5K4ECGX]file open error\n");
		return;
	}
	l = filp->f_path.dentry->d_inode->i_size;	
	PCAM_DEBUG("l = %ld\n", l);
	dp = kmalloc(l, GFP_KERNEL);
	if (dp == NULL) {
		printk("[S5K4ECGX]Out of Memory\n");
		filp_close(filp, current->files);
	}
	pos = 0;
	memset(dp, 0, l);
	ret = vfs_read(filp, (char __user *)dp, l, &pos);
	if (ret != l) {
		printk("[S5K4ECGX]Failed to read file ret = %d\n", ret);
		kfree(dp);
		filp_close(filp, current->files);
		return;
	}
	filp_close(filp, current->files);
	set_fs(fs);

	s5k4ecgx_regs_table = dp;
	s5k4ecgx_regs_table_size = l;
	*((s5k4ecgx_regs_table + s5k4ecgx_regs_table_size) - 1) = '\0';
//	PCAM_DEBUG("s5k4ecgx_regs_table 0x%x, %ld\n", dp, l);
}

void s5k4ecgx_regs_table_exit(void)
{
	PCAM_DEBUG("%s %d\n", __func__, __LINE__);
	if (s5k4ecgx_regs_table) {
		kfree(s5k4ecgx_regs_table);
		s5k4ecgx_regs_table = NULL;
	}
}

static int s5k4ecgx_regs_table_write(char *name)
{
	char *start, *end, *reg;
	unsigned short addr, value;
	char reg_buf[7], data_buf[7];

	addr = value = 0;

	*(reg_buf + 6) = '\0';
	*(data_buf + 4) = '\0';

	start = strstr(s5k4ecgx_regs_table, name);
	end = strstr(start, "};");

	while (1) {
		/* Find Address */
		reg = strstr(start,"{0x");
		if (reg)
			start = (reg + 12);
		if ((reg == NULL) || (reg > end))
			break;
		/* Write Value to Address */
		if (reg != NULL) {
			memcpy(reg_buf, (reg + 1), 6);
			memcpy(data_buf, (reg + 7), 4);
			addr = (unsigned short)simple_strtoul(reg_buf, NULL, 16); 
			value = (unsigned short)simple_strtoul(data_buf, NULL, 16); 
			//PCAM_DEBUG("addr 0x%04x, value 0x%04x\n", addr, value);
			if (addr == 0xffff)
			{
				msleep(value);
				PCAM_DEBUG("delay 0x%04x, value 0x%04x\n", addr, value);
			}
			else
			{
				if( s5k4ecgx_sensor_write(addr, value) < 0 )
				{
					printk("[S5K4ECGX]%s fail on sensor_write\n", __func__);
				}
			}
		}
		else
			printk("[S5K4ECGX]EXCEPTION! reg value : %c  addr : 0x%x,  value : 0x%x\n", *reg, addr, value);
	}
	return 0;
}
#endif

int s5k4ecgx_sensor_init(const struct msm_camera_sensor_info *data)
{
	int rc = 0, i = 0;
	s5k4ecgx_set_power(true);
	s5k4ecgx_ctrl = kzalloc(sizeof(struct s5k4ecgx_ctrl), GFP_KERNEL);
	if (!s5k4ecgx_ctrl) {
		CDBG("s5k4ecgx_init failed!\n");
		rc = -ENOMEM;
		goto init_done;
	}

	if (data)
		s5k4ecgx_ctrl->sensordata = data;

	msm_camio_camif_pad_reg_reset();
	msleep(5);
	
	rc = s5k4ecgx_probe_init_sensor();
	if(rc < 0)
	{
		for(i=0;i<5;i++)
		{
			printk("[S5K4ECGX]cam_fw_init failed. try again.\n");
			s5k4ecgx_set_power(false);
			mdelay(50);
			s5k4ecgx_set_power(true);
			msm_camio_camif_pad_reg_reset();
			msleep(5);
			rc = s5k4ecgx_probe_init_sensor();
			probe_init_retry++;
			if(rc >= 0)break;
		}
		if(rc < 0)goto init_fail;
	}

#ifdef CONFIG_LOAD_FILE
	s5k4ecgx_regs_table_init();
#endif

init_done:
	return rc;

init_fail:
	kfree(s5k4ecgx_ctrl);
	return rc;
}

static int s5k4ecgx_init_client(struct i2c_client *client)
{
	/* Initialize the MSM_CAMI2C Chip */
	init_waitqueue_head(&s5k4ecgx_wait_queue);
	return 0;
}

int s5k4ecgx_sensor_config(void __user *argp)
{
	struct sensor_cfg_data cfg_data;
	long   rc = 0;

	if (copy_from_user(&cfg_data,
			(void *)argp,
			sizeof(struct sensor_cfg_data)))
		return -EFAULT;

	/* down(&s5k4ecgx_sem); */
	CDBG("s5k4ecgx_ioctl, cfgtype = %d, mode = %d\n",
		cfg_data.cfgtype, cfg_data.mode);
		switch (cfg_data.cfgtype)
		{
			case CFG_SET_MODE:
				rc = s5k4ecgx_set_sensor_mode(
						cfg_data.mode);
			break;
			case CFG_SET_EFFECT:
				rc = s5k4ecgx_config_effect(cfg_data.mode,
						cfg_data.cfg.effect);
			break;
			case CFG_GET_AF_MAX_STEPS:
			default:
				rc = -EINVAL;
			break;
		}
	/* up(&s5k4ecgx_sem); */
	
	return rc;
}

int s5k4ecgx_sensor_release(void)
{
	int rc = 0;

	/* down(&s5k4ecgx_sem); */
	/*
	PCAM_DEBUG("lens moving to Base before CAM OFF\n");
	s5k4ecgx_sensor_write(0x0028, 0x7000);
	s5k4ecgx_sensor_write(0x002A, 0x0254);
	s5k4ecgx_sensor_write(0x0F12, 0x0030); //Lens Pistion (0x00 ~ 0xfF) normally (0x30 ~ 0x80)
	*/

	s5k4ecgx_status.camera_initailized = false;
	s5k4ecgx_status.need_configuration = 0;
	s5k4ecgx_status.effect = 0;
	s5k4ecgx_status.brightness = 0;
	s5k4ecgx_status.contrast = 0;
	s5k4ecgx_status.saturation = 0;
	s5k4ecgx_status.sharpness = 0;
	s5k4ecgx_status.whiteBalance = 0;
	s5k4ecgx_status.iso = 0;
	s5k4ecgx_status.auto_exposure = 0;
	s5k4ecgx_status.scene = 0;
	s5k4ecgx_status.afmode = PCAM_AF_SET_NORMAL;
	s5k4ecgx_status.afcanceled = true;
	s5k4ecgx_status.dtp = 0;
	s5k4ecgx_status.flash_status = 0;
	
	s5k4ecgx_set_power(false);
	cpufreq_direct_set_policy(0, "ondemand");
	
	PCAM_DEBUG("s5k4ecgx_sensor_release\n");
	kfree(s5k4ecgx_ctrl);
	/* up(&s5k4ecgx_sem); */

#ifdef CONFIG_LOAD_FILE
	s5k4ecgx_regs_table_exit();
#endif

	return rc;
}

static int s5k4ecgx_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	int rc = 0;
	
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		rc = -ENOTSUPP;
		goto probe_failure;
	}

	s5k4ecgx_sensorw =
		kzalloc(sizeof(struct s5k4ecgx_work), GFP_KERNEL);

	if (!s5k4ecgx_sensorw) {
		rc = -ENOMEM;
		goto probe_failure;
	}

	i2c_set_clientdata(client, s5k4ecgx_sensorw);
	s5k4ecgx_init_client(client);
	s5k4ecgx_client = client;
	
#ifdef USE_FLASHOFF_TIMER
	init_timer(&flashoff_timer);
	flashoff_timer.function = s5k4ecgx_flashoff_timer_handler;
	flashoff_timer.expires = jiffies + 60*HZ;
#endif

	printk("[S5K4ECGX]s5k4ecgx_probe succeeded!\n");

	return 0;

probe_failure:
	kfree(s5k4ecgx_sensorw);
	s5k4ecgx_sensorw = NULL;
	printk("[S5K4ECGX]s5k4ecgx_probe failed!\n");
	return rc;
}

static const struct i2c_device_id s5k4ecgx_i2c_id[] = {
	{ "s5k4ecgx", 0},
	{ },
};

static struct i2c_driver s5k4ecgx_i2c_driver = {
	.id_table = s5k4ecgx_i2c_id,
	.probe  = s5k4ecgx_i2c_probe,
	.remove = __exit_p(s5k4ecgx_i2c_remove),
	.driver = {
		.name = "s5k4ecgx",
	},
};

static int s5k4ecgx_sensor_probe(const struct msm_camera_sensor_info *info,
				struct msm_sensor_ctrl *s)
{

	int rc = i2c_add_driver(&s5k4ecgx_i2c_driver);
	if (rc < 0){// || s5k4ecgx_client == NULL) {
		printk("[S5K4ECGX]s5k4ecgx_sensor_probe fail\n");
		rc = -ENOTSUPP;
		goto probe_done;
	}

	/*initailize states*/
	s5k4ecgx_status.camera_initailized = false;
	s5k4ecgx_status.need_configuration = 0;
	s5k4ecgx_status.camera_mode = PCAM_CAM_MODE_CAMERA;
	s5k4ecgx_status.effect = PCAM_EFFECT_NORMAL;
	s5k4ecgx_status.brightness = PCAM_BR_STEP_0;
	s5k4ecgx_status.contrast = PCAM_CR_STEP_0;
	s5k4ecgx_status.saturation = PCAM_SA_STEP_0;
	s5k4ecgx_status.sharpness = PCAM_SP_STEP_0;
	s5k4ecgx_status.whiteBalance = PCAM_WB_AUTO;
	s5k4ecgx_status.iso = PCAM_ISO_AUTO;
	s5k4ecgx_status.auto_exposure = PCAM_METERING_NORMAL;
	s5k4ecgx_status.scene = PCAM_SCENE_OFF;
	s5k4ecgx_status.afmode  = PCAM_AF_SET_NORMAL;
	s5k4ecgx_status.afcanceled = true;
	s5k4ecgx_status.dtp = PCAM_DTP_OFF;
	s5k4ecgx_status.snapshot_size = PCAM_SNAPSHOT_SIZE_2560x1920_5M;
	s5k4ecgx_status.preview_size = PCAM_PREVIEW_SIZE_640x480_VGA;
	s5k4ecgx_status.flash_mode = PCAM_FLASH_OFF;
	s5k4ecgx_status.flash_status = FLASH_OFF;
	s5k4ecgx_status.zoom = PCAM_ZOOM_STEP_0;
	s5k4ecgx_status.current_lux = 0;
	s5k4ecgx_status.jpeg_quality = PCAM_JPEG_QUALITY_SUPERFINE;
	s5k4ecgx_status.auto_contrast = PCAM_AUTO_CONTRAST_OFF;
	
	/*sensor on/off for vfe initailization */
	s5k4ecgx_set_power(true);
	/* Input MCLK = 24MHz */
	msm_camio_clk_rate_set(24000000);
	s5k4ecgx_probe_init_sensor();

	s->s_init = s5k4ecgx_sensor_init;
	s->s_release = s5k4ecgx_sensor_release;
	s->s_config  = s5k4ecgx_sensor_config;
	s5k4ecgx_set_power(false);
probe_done:
	CDBG("%s %s:%d\n", __FILE__, __func__, __LINE__);
	return rc;
}

static int __s5k4ecgx_probe(struct platform_device *pdev)
{
	return msm_camera_drv_start(pdev, s5k4ecgx_sensor_probe);
}

static struct platform_driver msm_camera_driver = {
	.probe = __s5k4ecgx_probe,
	.driver = {
		.name = "msm_camera_s5k4ecgx",
		.owner = THIS_MODULE,
	},
};

static int __init s5k4ecgx_init(void)
{
	return platform_driver_register(&msm_camera_driver);
}

module_init(s5k4ecgx_init);


