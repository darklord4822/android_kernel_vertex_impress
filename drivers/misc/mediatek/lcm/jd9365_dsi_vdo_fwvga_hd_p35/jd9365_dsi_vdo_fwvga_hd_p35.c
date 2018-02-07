/*****************************************************************************
*  Copyright Statement:
*  --------------------
*  This software is protected by Copyright and the information contained
*  herein is confidential. The software may not be copied and the information
*  contained herein may not be used or disclosed except with the written
*  permission of MediaTek Inc. (C) 2008
*
*  BY OPENING THIS FILE, BUYER HEREBY UNEQUIVOCALLY ACKNOWLEDGES AND AGREES
*  THAT THE SOFTWARE/FIRMWARE AND ITS DOCUMENTATIONS ("MEDIATEK SOFTWARE")
*  RECEIVED FROM MEDIATEK AND/OR ITS REPRESENTATIVES ARE PROVIDED TO BUYER ON
*  AN "AS-IS" BASIS ONLY. MEDIATEK EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES,
*  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE IMPLIED WARRANTIES OF
*  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE OR NONINFRINGEMENT.
*  NEITHER DOES MEDIATEK PROVIDE ANY WARRANTY WHATSOEVER WITH RESPECT TO THE
*  SOFTWARE OF ANY THIRD PARTY WHICH MAY BE USED BY, INCORPORATED IN, OR
*  SUPPLIED WITH THE MEDIATEK SOFTWARE, AND BUYER AGREES TO LOOK ONLY TO SUCH
*  THIRD PARTY FOR ANY WARRANTY CLAIM RELATING THERETO. MEDIATEK SHALL ALSO
*  NOT BE RESPONSIBLE FOR ANY MEDIATEK SOFTWARE RELEASES MADE TO BUYER'S
*  SPECIFICATION OR TO CONFORM TO A PARTICULAR STANDARD OR OPEN FORUM.
*
*  BUYER'S SOLE AND EXCLUSIVE REMEDY AND MEDIATEK'S ENTIRE AND CUMULATIVE
*  LIABILITY WITH RESPECT TO THE MEDIATEK SOFTWARE RELEASED HEREUNDER WILL BE,
*  AT MEDIATEK'S OPTION, TO REVISE OR REPLACE THE MEDIATEK SOFTWARE AT ISSUE,
*  OR REFUND ANY SOFTWARE LICENSE FEES OR SERVICE CHARGE PAID BY BUYER TO
*  MEDIATEK FOR SUCH MEDIATEK SOFTWARE AT ISSUE.
*
*  THE TRANSACTION CONTEMPLATED HEREUNDER SHALL BE CONSTRUED IN ACCORDANCE
*  WITH THE LAWS OF THE STATE OF CALIFORNIA, USA, EXCLUDING ITS CONFLICT OF
*  LAWS PRINCIPLES.  ANY DISPUTES, CONTROVERSIES OR CLAIMS ARISING THEREOF AND
*  RELATED THERETO SHALL BE SETTLED BY ARBITRATION IN SAN FRANCISCO, CA, UNDER
*  THE RULES OF THE INTERNATIONAL CHAMBER OF COMMERCE (ICC).
*
*****************************************************************************/
#ifdef BUILD_LK
#else
#include <linux/string.h>
#endif

#include "lcm_drv.h"

//#include <prj/prj_config.h>


// ---------------------------------------------------------------------------
//  Local Constants
// ---------------------------------------------------------------------------

#define FRAME_WIDTH  										(720) 
#define FRAME_HEIGHT 										(1280)

#define REGFLAG_DELAY             							0XFE
#define REGFLAG_END_OF_TABLE      							0xFF   // END OF REGISTERS MARKER
#define LCM_DSI_CMD_MODE									0

#define LCM_ID_JD9365										(0x9365)

// ---------------------------------------------------------------------------
//  Local Variables
// ---------------------------------------------------------------------------

static LCM_UTIL_FUNCS lcm_util = {0};

#define SET_RESET_PIN(v)    (lcm_util.set_reset_pin((v)))

#define UDELAY(n) (lcm_util.udelay(n))
#define MDELAY(n) (lcm_util.mdelay(n))

// ---------------------------------------------------------------------------
//  Local Functions
// ---------------------------------------------------------------------------

#define dsi_set_cmdq_V2(cmd, count, ppara, force_update)	lcm_util.dsi_set_cmdq_V2(cmd, count, ppara, force_update)
#define dsi_set_cmdq(pdata, queue_size, force_update)		lcm_util.dsi_set_cmdq(pdata, queue_size, force_update)
#define wrtie_cmd(cmd)										lcm_util.dsi_write_cmd(cmd)
#define write_regs(addr, pdata, byte_nums)					lcm_util.dsi_write_regs(addr, pdata, byte_nums)
#define read_reg(cmd)											lcm_util.dsi_dcs_read_lcm_reg(cmd)
#define read_reg_v2(cmd, buffer, buffer_size)				lcm_util.dsi_dcs_read_lcm_reg_v2(cmd, buffer, buffer_size)

// ---------------------------------------------------------------------------
//  LCM Driver Implementations
// ---------------------------------------------------------------------------

struct LCM_setting_table {
    unsigned cmd;
    unsigned char count;
    unsigned char para_list[64];
};

static struct LCM_setting_table lcm_initialization_setting[] = {
    //Page0
    {0xE0,1,{0x00}},
    
    //--- PASSWORD  ----//
    {0xE1,1,{0x93}},
    {0xE2,1,{0x65}},
    {0xE3,1,{0xF8}},
//    
    //--- Sequence Ctrl ----//
    {0x70,1,{0x10}},
    {0x71,1,{0x13}},
    {0x72,1,{0x06}},
    //{0x75,1,{0x03}},
    
    {0xE0,1,{0x04}},//Í¨µÀÑ¡Ôñ
    {0x2D,1,{0x03}},
    {0xE0,1,{0x00}},
    {0x80,1,{0x02}},//3L
    
	{0xE6,1,{0x02}},	//Watch dog
	{0xE7,1,{0x02}},	//Watch dog     
//    
    //--- Page1  ----//
    {0xE0,1,{0x01}},
    
    //Set VCOM
    {0x00,1,{0x00}},
    {0x01,1,{0x48}},
    {0x03,1,{0x00}},
    {0x04,1,{0x66}},
    
    //Set Gamma Power,1,{ VGMP,1,{VGMN,1,{VGSP,1,{VGSN
    {0x17,1,{0x00}},
    {0x18,1,{0xBF}},
    {0x19,1,{0x03}},
    {0x1A,1,{0x00}},
    {0x1B,1,{0xBF}},
    {0x1C,1,{0x0C}},
    
    //Set Gate Power
	{0x1F,1,{0x79}}, //VGH_REG=-12.4V
	{0x20,1,{0x2D}}, //VGL_REG=-12.4V
	{0x21,1,{0x2D}},
	{0x22,1,{0x4F}},// VGH_REG=VGH
//    {0x24,1,{0xF8}},
    {0x26,1,{0xF1}},
    
    //add source op
   // {0x35,1,{0x38}},	
   // {0x36,1,{0x58}},
    
    //SET RGBCYC
    {0x37,1,{0x09}},	//SS=1,1,{ BGR=1
    {0x38,1,{0x04}},	//JDT=100 column inversion
    {0x39,1,{0x0C}},
    {0x3A,1,{0x18}},
    {0x3C,1,{0x78}},
    
    //Set TCON
    {0x40,1,{0x04}},	//RSO 04h=720,1,{ 05h=768,1,{ 06h=800
    {0x41,1,{0xA0}},	//LN=640->1280 line
    
    //--- power voltage  ----//
    {0x55,1,{0x01}},	//DCDCM=0011,1,{ HX5186
    {0x56,1,{0x01}},
    {0x57,1,{0x6D}},
    {0x58,1,{0x0A}},
    {0x59,1,{0x1A}},
    {0x5A,1,{0x65}},
    {0x5B,1,{0x14}},
    {0x5C,1,{0x15}},
    
    //--- Gamma  ----//
	{0x5D,1,{0x70}},
	{0x5E,1,{0x58}},
	{0x5F,1,{0x48}},
	{0x60,1,{0x3B}},
	{0x61,1,{0x35}},
	{0x62,1,{0x25}},
	{0x63,1,{0x29}},
	{0x64,1,{0x13}},
	{0x65,1,{0x2C}},
	{0x66,1,{0x2C}},
	{0x67,1,{0x2C}},
	{0x68,1,{0x4B}},
	{0x69,1,{0x3B}},
	{0x6A,1,{0x43}},
	{0x6B,1,{0x35}},
	{0x6C,1,{0x2F}},
	{0x6D,1,{0x25}},
	{0x6E,1,{0x13}},
	{0x6F,1,{0x02}},
		
	{0x70,1,{0x70}},
	{0x71,1,{0x58}},
	{0x72,1,{0x48}},
	{0x73,1,{0x3b}},
	{0x74,1,{0x35}},
	{0x75,1,{0x25}},
	{0x76,1,{0x29}},
	{0x77,1,{0x13}},
	{0x78,1,{0x2C}},
	{0x79,1,{0x2C}},
	{0x7A,1,{0x2C}},
	{0x7B,1,{0x4B}},
	{0x7C,1,{0x3B}},
	{0x7D,1,{0x43}},
	{0x7E,1,{0x35}},
	{0x7F,1,{0x2F}},
	{0x80,1,{0x25}},
	{0x81,1,{0x13}},
	{0x82,1,{0x02}},    
    
	//Page2,1,{ for GIP
	{0xE0,1,{0x02}},    
		
	//GIP_L Pin mapping   	
	{0x00,1,{0x13}},	
	{0x01,1,{0x11}},	
	{0x02,1,{0x0B}},	
	{0x03,1,{0x09}},	
	{0x04,1,{0x07}},	
	{0x05,1,{0x05}},	
	{0x06,1,{0x1F}},	
	{0x07,1,{0x1F}},	
	{0x08,1,{0x1F}},	
	{0x09,1,{0x1F}},	
	{0x0A,1,{0x1F}},	
	{0x0B,1,{0x1F}},	
	{0x0C,1,{0x1F}},	
	{0x0D,1,{0x1F}},	
	{0x0E,1,{0x1F}},	
	{0x0F,1,{0x1F}},	
	{0x10,1,{0x1F}},	
	{0x11,1,{0x1F}},	
	{0x12,1,{0x01}},	
	{0x13,1,{0x03}},	
	{0x14,1,{0x1F}},	
	{0x15,1,{0x1F}},	
		
	//GIP_R Pin mapping
	{0x16,1,{0x12}},	
	{0x17,1,{0x10}},	
	{0x18,1,{0x0A}},	
	{0x19,1,{0x08}},	
	{0x1A,1,{0x06}},	
	{0x1B,1,{0x04}},	
	{0x1C,1,{0x1F}},	
	{0x1D,1,{0x1F}},	
	{0x1E,1,{0x1F}},	
	{0x1F,1,{0x1F}},	
	{0x20,1,{0x1F}},	
	{0x21,1,{0x1F}},	
	{0x22,1,{0x1F}},	
	{0x23,1,{0x1F}},	
	{0x24,1,{0x1F}},	
	{0x25,1,{0x1F}},	
	{0x26,1,{0x1F}},	
	{0x27,1,{0x1F}},	
	{0x28,1,{0x00}},	
	{0x29,1,{0x02}},	
	{0x2A,1,{0x1F}},	
	{0x2B,1,{0x1F}},			
		
	//GIP_L_GS Pin mapping
	{0x2C,1,{0x00}},	
	{0x2D,1,{0x02}},	
	{0x2E,1,{0x08}},	
	{0x2F,1,{0x0A}},	
	{0x30,1,{0x04}},	
	{0x31,1,{0x06}},	
	{0x32,1,{0x1F}},	
	{0x33,1,{0x1F}},	
	{0x34,1,{0x1F}},	
	{0x35,1,{0x1F}},	
	{0x36,1,{0x1F}},	
	{0x37,1,{0x1F}},	
	{0x38,1,{0x1F}},	
	{0x39,1,{0x1F}},	
	{0x3A,1,{0x1F}},	
	{0x3B,1,{0x1F}},	
	{0x3C,1,{0x1F}},	
	{0x3D,1,{0x1F}},	
	{0x3E,1,{0x12}},	
	{0x3F,1,{0x10}},	
	{0x40,1,{0x1F}},	
	{0x41,1,{0x1F}},		
		
	//GIP_R_GS Pin mapping
	{0x42,1,{0x01}}, 	
	{0x43,1,{0x03}}, 	
	{0x44,1,{0x09}}, 	
	{0x45,1,{0x0B}}, 	
	{0x46,1,{0x05}}, 	
	{0x47,1,{0x07}}, 	
	{0x48,1,{0x1F}}, 	
	{0x49,1,{0x1F}}, 	
	{0x4A,1,{0x1F}}, 	
	{0x4B,1,{0x1F}}, 	
	{0x4C,1,{0x1F}}, 	
	{0x4D,1,{0x1F}}, 	
	{0x4E,1,{0x1F}}, 	
	{0x4F,1,{0x1F}}, 	
	{0x50,1,{0x1F}}, 	
	{0x51,1,{0x1F}}, 	
	{0x52,1,{0x1F}}, 	
	{0x53,1,{0x1F}}, 	
	{0x54,1,{0x13}}, 	
	{0x55,1,{0x11}}, 	
	{0x56,1,{0x1F}}, 	
	{0x57,1,{0x1F}},			
		
	//GIP Timing
	{0x58,1,{0x40}},
	{0x59,1,{0x00}},
	{0x5A,1,{0x00}},
	{0x5B,1,{0x30}},
	{0x5C,1,{0x09}},
	{0x5D,1,{0x30}},
	{0x5E,1,{0x01}},
	{0x5F,1,{0x02}},
	{0x60,1,{0x30}},
	{0x61,1,{0x01}},
	{0x62,1,{0x02}},
	{0x63,1,{0x03}},
	{0x64,1,{0x64}}, //SETV_OFF,1,{ modify 20140806
	{0x65,1,{0x75}},
	{0x66,1,{0x0D}},
	{0x67,1,{0x72}},
	{0x68,1,{0x0C}},
	{0x69,1,{0x10}},
	{0x6A,1,{0x64}}, //CKV_OFF,1,{ modify 20140806
	{0x6B,1,{0x08}},
	{0x6C,1,{0x00}},
	{0x6D,1,{0x00}},
	{0x6E,1,{0x00}},
	{0x6F,1,{0x00}},
	{0x70,1,{0x00}},
	{0x71,1,{0x00}},
	{0x72,1,{0x06}},
	{0x73,1,{0x86}},
	{0x74,1,{0x00}},
	{0x75,1,{0x07}},
	{0x76,1,{0x00}},
	{0x77,1,{0x5D}},
	{0x78,1,{0x19}},
	{0x79,1,{0x00}},
	{0x7A,1,{0x05}},
	{0x7B,1,{0x05}},
	{0x7C,1,{0x00}},
	{0x7D,1,{0x03}},
	{0x7E,1,{0x86}},
    
    //Page4
    {0xE0,1,{0x04}}, //ESD
	{0x09,1,{0x10}}, //ESD
    {0x2B,1,{0x2B}}, 
    {0x2E,1,{0x44}}, 
    //Page0
    {0xE0,1,{0x00}},
    {0xE6,1,{0x02}}, 
    {0xE7,1,{0x02}}, 
    
    {0x11,1,{0x00}},                 // Sleep-Out
    
    {REGFLAG_DELAY,120,{}},
    
    {0x29,1,{0x00}},                 // Display On
    
    {REGFLAG_DELAY,20,{}},
    {REGFLAG_END_OF_TABLE,0x00,{}}
};

/*
static struct LCM_setting_table lcm_sleep_out_setting[] = {
	// Sleep Out
	{0x11, 1, {0x00}},
	{REGFLAG_DELAY, 120, {}},

	// Display ON
	{0x29, 1, {0x00}},
	{REGFLAG_DELAY, 10, {}},

	{REGFLAG_END_OF_TABLE, 0x00, {}}
};
*/

static struct LCM_setting_table lcm_sleep_in_setting[] = {
{0xe0, 1, {0x00,0x01,0x00,0x00,0x00}},
{0xe6, 1, {0x02}},
{0xe7, 1, {0x02}},
{0xe0, 1, {0x00}},
{0x11, 0, {0x00,0x00}},
{0xfe, 120, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
{0x29, 0, {0x00,0x00}},
{0xfe, 20, {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}},
{0x35, 1, {0x00}},
{REGFLAG_END_OF_TABLE, 0x00, {}}
};

static void push_table(struct LCM_setting_table *table, unsigned int count,
		unsigned char force_update)
{
	unsigned int i;

	for (i = 0; i < count; i++) {

		unsigned cmd;
		cmd = table[i].cmd;

		switch (cmd) {

		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;

		case REGFLAG_END_OF_TABLE:
			break;

		default:
			dsi_set_cmdq_V2(cmd, table[i].count,
					table[i].para_list, force_update);
		}
	}

}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS * params)
{
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type = LCM_TYPE_DSI;

	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;

	// enable tearing-free
	params->dbi.te_mode = LCM_DBI_TE_MODE_DISABLED;
	params->dbi.te_edge_polarity = LCM_POLARITY_RISING;

#if (LCM_DSI_CMD_MODE)
	params->dsi.mode = CMD_MODE;
#else
	params->dsi.mode   = SYNC_PULSE_VDO_MODE;//SYNC_EVENT_VDO_MODE;//BURST_VDO_MODE;////
#endif

	// DSI
	/* Command mode setting */
		params->dsi.LANE_NUM				= LCM_FOUR_LANE;
	
	//The following defined the fomat for data coming from LCD engine.
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq   = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding     = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format      = LCM_DSI_FORMAT_RGB888;
	
	
	params->dsi.PS=LCM_PACKED_PS_24BIT_RGB888;
	
#if (LCM_DSI_CMD_MODE)
	params->dsi.intermediat_buffer_num = 0;//because DSI/DPI HW design change, this parameters should be 0 when video mode in MT658X; or memory leakage
	params->dsi.word_count=FRAME_WIDTH*3;	//DSI CMD mode need set these two bellow params, different to 6577
#else
	params->dsi.intermediat_buffer_num = 0;	//because DSI/DPI HW design change, this parameters should be 0 when video mode in MT658X; or memory leakage
#endif

	// Video mode setting
	params->dsi.packet_size=256;

	params->dsi.vertical_sync_active				=  2;//4
	params->dsi.vertical_backporch					= 16;//50;
	params->dsi.vertical_frontporch					= 8;//50;
	params->dsi.vertical_active_line				= FRAME_HEIGHT; 

		params->dsi.horizontal_sync_active				= 20;
		params->dsi.horizontal_backporch				= 20;
		params->dsi.horizontal_frontporch				= 20;
		params->dsi.horizontal_active_pixel 			= FRAME_WIDTH;

	// Bit rate calculation
	//1 Every lane speed
/*
		params->dsi.esd_check_enable = 1;
		params->dsi.customization_esd_check_enable = 1;
        params->dsi.lcm_esd_check_table[0].cmd          = 0x0A;
        params->dsi.lcm_esd_check_table[0].count        = 1;
        params->dsi.lcm_esd_check_table[0].para_list[0] = 0x9C;
		
		params->dsi.cont_clock=1;
*/
		params->dsi.PLL_CLOCK=215;
}


static void lcm_init(void)
{
	SET_RESET_PIN(1);
	MDELAY(10);

	SET_RESET_PIN(0);
	MDELAY(20);
	SET_RESET_PIN(1);
	MDELAY(120);
	
	push_table(lcm_initialization_setting,sizeof(lcm_initialization_setting) /sizeof(struct LCM_setting_table), 1);
}

static void lcm_suspend(void)
{
	push_table(lcm_sleep_in_setting,sizeof(lcm_sleep_in_setting) /sizeof(struct LCM_setting_table), 1);
}

static void lcm_resume(void)
{
	
	lcm_init();
	
}

static unsigned int lcm_compare_id(void)
{
    return 1;
}

LCM_DRIVER jd9365_dsi_vdo_fwvga_hd_p35_lcm_drv = 
{
	.name		= "jd9365_dsi_vdo_fwvga_hd_p35",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params     = lcm_get_params,
	.init           = lcm_init,
	.suspend        = lcm_suspend,
	.resume         = lcm_resume,
	.compare_id     = lcm_compare_id,
};
