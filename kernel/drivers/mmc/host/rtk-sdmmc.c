/* Read-only trial driver, derived from Qua stock 4.1.17 BSP. */
#include <linux/blkdev.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/utsname.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>
#include <linux/mmc/sdio.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/bitops.h>
#include <linux/regulator/consumer.h>

//#include <linux/syscalls.h>
//#include <uapi/asm-generic/unistd.h>

#include <linux/kthread.h>
#include <soc/realtek/rtd129x-rbus-guard.h>
#define rtk_lockapi_lock(flags, name) rtd129x_rbus_guard_lock(&(flags))
#define rtk_lockapi_unlock(flags, name) rtd129x_rbus_guard_unlock(flags)

#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/gpio/consumer.h>

#include "rtk-sdmmc-reg.h"
#include "rtk-sdmmc.h"

#ifdef MODULE
#define DRIVER_NAME    "rtk-sdmmc-trial"
#else
#define DRIVER_NAME    "rtk-sdmmc"
#endif
#define BANNER      "Realtek SD/MMC Host Driver"

#define SD_ALLOC_LENGTH    4096
#define SD_PINMUX_OFFSET   0x604
#define SD_PINMUX_MASK     0xffff0000
#define SD_PINMUX_SD_CARD  0x55550000

#define PM_SD_SUSPEND_ON           0
#define PM_SD_SUSPEND_FREEZE       1
#define PM_SD_SUSPEND_STANDBY      2
#define PM_SD_SUSPEND_MEM          3
#define PM_SD_SUSPEND_MIN          PM_SD_SUSPEND_FREEZE
#define PM_SD_SUSPEND_MAX          4

/*extern void set_gpio20_high(void);
extern void set_gpio20_low(void);
extern void set_gpio18_high(void);
extern void set_gpio18_low(void);

#define TOGGLE_GPIO_HIGH20    set_gpio20_high()
#define TOGGLE_GPIO_LOW20     set_gpio20_low()
#define TOGGLE_GPIO_HIGH18    set_gpio18_high()
#define TOGGLE_GPIO_LOW18     set_gpio18_low()

#ifndef TOGGLE_GPIO_HIGH20
        #define TOGGLE_GPIO_HIGH20
#endif
#ifndef TOGGLE_GPIO_LOW20
        #define TOGGLE_GPIO_LOW20
#endif

#ifndef TOGGLE_GPIO_HIGH18
        #define TOGGLE_GPIO_HIGH18
#endif
#ifndef TOGGLE_GPIO_LOW18
        #define TOGGLE_GPIO_LOW18
#endif*/

static int rtk_sdmmc_send_cmd_get_rsp(struct sdmmc_cmd_pkt *cmd_info);
static int rtk_sdmmc_stream(struct sdmmc_cmd_pkt *cmd_info);
static void rtk_sdmmc_hw_initial(struct rtk_sdmmc_host *rtk_host);
static void rtk_sdmmc_plug(struct work_struct *work);
static void rtk_sdmmc_set_access_mode(struct rtk_sdmmc_host *rtk_host,u8 level);
void rtk_sdmmc_sync(struct rtk_sdmmc_host *rtk_host);
static void rtk_sdmmc_speed(struct rtk_sdmmc_host *rtk_host, enum sdmmc_clock_speed sd_speed);
static unsigned int sdmmc_rca = 0;
static unsigned int mmc_detected = 0;
//static struct rw_semaphore cr_sd_rw_sem;


//int get_RTK_initial_flag(void);
bool INT4_flag=false;
//=============================================
//static struct workqueue_struct *cmd12_qu;
//static struct delayed_work rtk_cmd12_delayed_work;
//=============================================

static void rtk_sdmmc_reset(struct rtk_sdmmc_host *rtk_host)
{
    void __iomem *sdmmc_base = rtk_host->sdmmc;

    writel(0x00000000, sdmmc_base + CR_SD_DMA_CTL3);
    writeb(0x00, sdmmc_base + SD_TRANSFER);
    writeb(0xFF, sdmmc_base + CR_CARD_STOP); //SD Card module transfer stop and idle state.
    writeb(0x00, sdmmc_base + CR_CARD_STOP); //SD Card module transfer start.
}

void rtk_sdmmc_sync(struct rtk_sdmmc_host *rtk_host){
    writel(0x00000000, rtk_host->sysbrdg + SDMMC_SYNC);
}

static u8 rtk_sdmmc_get_rsp_type(struct mmc_command* cmd)
{
    u32 rsp_type = 0;
    u32 rsp_type_chk = mmc_resp_type(cmd);

    if(rsp_type_chk == MMC_RSP_R1)
       rsp_type = SD_R1;
    else if(rsp_type_chk == MMC_RSP_R1B)
       rsp_type = SD_R1b;
    else if(rsp_type_chk == MMC_RSP_R2)
       rsp_type = SD_R2;
    else if(rsp_type_chk == MMC_RSP_R3)
       rsp_type = SD_R3;
    else if(rsp_type_chk == MMC_RSP_R6)
       rsp_type = SD_R6;
    else if(rsp_type_chk == MMC_RSP_R7)
       rsp_type = SD_R7;
    else
       rsp_type = SD_R0;

    return rsp_type;
}

static u8 rtk_sdmmc_get_rsp_len(u8 rsp_para)
{
    switch(rsp_para & 0x03){
    case 0:
        return 0;
    case 1:
        return 6;
    case 2:
        return 16;
    default:
        return 0;
    }
}

static void rtk_sdmmc_set_access_mode(struct rtk_sdmmc_host *rtk_host,u8 level)
{
    void __iomem *sdmmc_base = rtk_host->sdmmc;
    u32 tmp_bits = 0;

    tmp_bits = readb(sdmmc_base + SD_CONFIGURE1) & ~MODE_SEL_MASK;

    if(level == ACCESS_MODE_SD20)
        tmp_bits |= MODE_SEL_SD20;
    else if(level == ACCESS_MODE_DDR)
        tmp_bits |= MODE_SEL_DDR;
    else if(level == ACCESS_MODE_SD30)
        tmp_bits |= (MODE_SEL_SD30 | SD30_ASYNC_FIFO_RST);
    else
        tmp_bits |= MODE_SEL_SD20;

    writeb(tmp_bits, sdmmc_base + SD_CONFIGURE1);
}

static void rtk_sdmmc_set_bits(struct rtk_sdmmc_host *rtk_host, u8 set_bit)
{
    void  *sdmmc_base = rtk_host->sdmmc;
    u32 tmp_bits = 0;

    tmp_bits = readb(sdmmc_base + SD_CONFIGURE1);
    if((tmp_bits & MASK_BUS_WIDTH) != set_bit){
        tmp_bits &= ~MASK_BUS_WIDTH;
        writeb(tmp_bits | set_bit, sdmmc_base + SD_CONFIGURE1);
    }
}

static void rtk_sdmmc_set_div(struct rtk_sdmmc_host *rtk_host, u32 set_div){
    void __iomem *sdmmc_base = rtk_host->sdmmc;
    u8 tmp_div = 0;

    rtk_sdmmc_sync(rtk_host);
    tmp_div = readb(sdmmc_base + SD_CONFIGURE1) & ~MASK_CLOCK_DIV;
    if(set_div != CLOCK_DIV_NON){
        writeb(tmp_div | set_div | SDCLK_DIV, sdmmc_base + SD_CONFIGURE1);
    }else{
        writeb(tmp_div, sdmmc_base + SD_CONFIGURE1);
    }
    rtk_sdmmc_sync(rtk_host);
}

static void rtk_sdmmc_set_speed(struct rtk_sdmmc_host *rtk_host, u8 level)
{
    void __iomem *sdmmc_base = rtk_host->sdmmc;

    switch(level){
        case 0: //ddr50 , highest speed
            rtk_sdmmc_debug("%s: speed 2100\n", __func__);
            writel(0x00002100, sdmmc_base + CR_SD_CKGEN_CTL);
            break;
        case 1:
            rtk_sdmmc_debug("%s: speed 2101\n", __func__);
            writel(0x00002101, sdmmc_base + CR_SD_CKGEN_CTL);
            break;
        case 2:
            rtk_sdmmc_debug("%s: speed 2102\n", __func__);
            writel(0x00002102, sdmmc_base + CR_SD_CKGEN_CTL);
            break;
        case 3:
            rtk_sdmmc_debug("%s: speed 2103\n", __func__);
            writel(0x00002103, sdmmc_base + CR_SD_CKGEN_CTL);
            break;
        default :
            rtk_sdmmc_debug("%s: default speed 2102\n", __func__);
            writel(0x00002102, sdmmc_base + CR_SD_CKGEN_CTL);
            break;
    }
    rtk_sdmmc_sync(rtk_host);
}

static void rtk_sdmmc_speed(struct rtk_sdmmc_host *rtk_host,
                            enum sdmmc_clock_speed speed)
{
    switch (speed) {
    case SDMMC_CLOCK_200KHZ:
        rtk_sdmmc_set_div(rtk_host, CLOCK_DIV_256);
        rtk_sdmmc_set_speed(rtk_host, 1);
        break;
    case SDMMC_CLOCK_6200KHZ:
        rtk_sdmmc_set_div(rtk_host, CLOCK_DIV_NON);
        rtk_sdmmc_set_speed(rtk_host, 3);
        break;
    case SDMMC_CLOCK_25000KHZ:
        rtk_sdmmc_set_access_mode(rtk_host, ACCESS_MODE_SD20);
        rtk_sdmmc_set_div(rtk_host, CLOCK_DIV_NON);
        rtk_sdmmc_set_speed(rtk_host, 1);
        break;
    case SDMMC_CLOCK_400KHZ:
    default:
        rtk_sdmmc_set_div(rtk_host, CLOCK_DIV_256);
        rtk_sdmmc_set_speed(rtk_host, 0);
        break;
    }
    rtk_sdmmc_sync(rtk_host);
}

static u32 rtk_sdmmc_swap_endian(u32 input)
{
    u32 output = 0;
    output = (input & 0xFF000000) >> 24 |
             (input & 0x00FF0000) >> 8 |
             (input & 0x0000FF00) << 8 |
             (input & 0x000000FF) << 24;
    return output;
}

static void rtk_sdmmc_read_rsp(struct rtk_sdmmc_host *rtk_host, u32 *rsp, int reg_count)
{
    void __iomem *sdmmc_base = rtk_host->sdmmc;

    if(reg_count == 6){
        rsp[0] = readb(sdmmc_base + SD_CMD1) << 24 |
                 readb(sdmmc_base + SD_CMD2) << 16 |
                 readb(sdmmc_base + SD_CMD3) << 8 |
                 readb(sdmmc_base + SD_CMD4);
    }else if(reg_count == 16){
        rsp[0] = rtk_sdmmc_swap_endian(rsp[0]);
        rsp[1] = rtk_sdmmc_swap_endian(rsp[1]);
        rsp[2] = rtk_sdmmc_swap_endian(rsp[2]);
        rsp[3] = rtk_sdmmc_swap_endian(rsp[3]);
    }
}

static int rtk_sdmmc_int_wait(char *name, struct rtk_sdmmc_host *host, u8 cmdcode)
{
    void __iomem *base = host->sdmmc;
    unsigned long flags, waited;
    u32 status;
    u8 trans;

    /* Drain a previous late interrupt before arming the next command. */
    writel(0, base + CR_SD_ISREN);
    synchronize_irq(host->irq);
    reinit_completion(&host->done);
    spin_lock_irqsave(&host->lock, flags);
    writel(0x16, base + CR_SD_ISR);
    host->irq_status = 0;
    host->expect_dma = INT4_flag;
    host->command_active = true;
    writel(0x17, base + CR_SD_ISREN);
    rtk_sdmmc_sync(host);
    writeb(cmdcode | START_EN, base + SD_TRANSFER);
    spin_unlock_irqrestore(&host->lock, flags);

    waited = wait_for_completion_timeout(&host->done, msecs_to_jiffies(3000));
    writel(0, base + CR_SD_ISREN);
    spin_lock_irqsave(&host->lock, flags);
    host->command_active = false;
    status = host->irq_status;
    spin_unlock_irqrestore(&host->lock, flags);
    synchronize_irq(host->irq);
    trans = readb(base + SD_TRANSFER);
    if (!waited || (status & ISRSTA_INT2) || (trans & ERR_STATUS) ||
        (trans & (END_STATE | IDLE_STATE)) != (END_STATE | IDLE_STATE)) {
        dev_warn(host->dev, "CMD%u %s: irq=%#x transfer=%#x status=%#x/%#x\n",
                 host->cmd_opcode, waited ? "failed" : "timeout", status, trans,
                 readb(base + SD_STATUS1), readb(base + SD_STATUS2));
        u32 dma_state;
        rtk_sdmmc_reset(host);
        if (readl_poll_timeout(base + CR_SD_DMA_CTL3, dma_state,
                               !(dma_state & DMA_XFER), 10, 100000)) {
            host->dma_failed = true;
            dev_err(host->dev, "DMA did not quiesce; refusing further commands\n");
        }
        writel(0x16, base + CR_SD_ISR);
        rtk_sdmmc_sync(host);
        return waited ? CR_TRANSFER_FAIL : CR_TRANSFER_TO;
    }
    return CR_TRANS_OK;
}

static int rtk_sdmmc_set_rspparam(struct sdmmc_cmd_pkt *cmd_info)
{
    rtk_sdmmc_debug("%s: opcode = %d\n", __func__, cmd_info->cmd->opcode);
    switch(cmd_info->cmd->opcode){
        case MMC_GO_IDLE_STATE: //cmd 0
            cmd_info->rsp_para1 = 0xD0; //SD1_R0;
            cmd_info->rsp_para2 = 0x74; //0x50 | SD_R0;
            cmd_info->rsp_para3 = 0x00;
            break;
        case MMC_SEND_OP_COND:
            if(mmc_detected == 1){
                cmd_info->rsp_para1 = SD1_R0;
                cmd_info->rsp_para2 = 0x45;
                cmd_info->rsp_para3 = 0x01;
                cmd_info->cmd->arg = MMC_VDD_30_31 | MMC_VDD_31_32 | MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_SECTOR_ADDR;
            }
            break;
        case MMC_ALL_SEND_CID: //cmd 2
            cmd_info->rsp_para1 = -1; //SD1_R0;
            cmd_info->rsp_para2 = 0x42; //SD_R2;
            cmd_info->rsp_para3 = 0x05; //SD2_R0;
            break;
        case MMC_SET_RELATIVE_ADDR: //cmd3
            cmd_info->rsp_para1 = -1; //SD1_R0;
            cmd_info->rsp_para2 = 0x41; //SD_R1 | CRC16_CAL_DIS;
            cmd_info->rsp_para3 = 0x05;
            break;
        case MMC_SET_DSR: //cmd4
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case SD_APP_SET_BUS_WIDTH: //cmd6
            if((cmd_info->cmd->flags & (0x3 << 5)) == MMC_CMD_ADTC){ //SD_SWITCH
                cmd_info->rsp_para1 = -1;
                cmd_info->rsp_para2 = 0x01; //SD_R1b | CRC16_CAL_DIS;
                cmd_info->rsp_para3 = 0x05;
            }else{
                cmd_info->rsp_para1 = -1;
                if(mmc_detected == 1){
                    cmd_info->rsp_para2 = 0x09; //SD_R1b | CRC16_CAL_DIS;
                    cmd_info->rsp_para3 = 0x05;
                }else{
                    cmd_info->rsp_para2 = 0x61; //SD_R1b | CRC16_CAL_DIS;
                    cmd_info->rsp_para3 = 0x0D;
                }
            }
            break;
        case MMC_SELECT_CARD: //cmd7
            if(cmd_info->cmd->flags == (MMC_RSP_R1 | MMC_CMD_AC)) {  //select
                cmd_info->rsp_para1 = -1;
                cmd_info->rsp_para2 = 0x41; //SD_R1b|CRC16_CAL_DIS;
                cmd_info->rsp_para3 = 0x05;
            }
            else {	//deselect
                cmd_info->rsp_para1 = -1;
                cmd_info->rsp_para2 = 0x40; //SD_R0|CRC16_CAL_DIS;
                cmd_info->rsp_para3 = 0x05;
            }
            break;
        case SD_SEND_IF_COND: //cmd8
            cmd_info->rsp_para1 = 0xD0;
            cmd_info->rsp_para2 = 0x41; //SD_R1;
            cmd_info->rsp_para3 = 0x0;
            if (cmd_info->rtk_host->rtflags & RTKCR_FCARD_SELECTED && mmc_detected == 1){
                cmd_info->rsp_para1 = 0x50;
                cmd_info->rsp_para2 = SD_R1;
                cmd_info->rsp_para3 = 0x05;
            }
            break;
        case MMC_SEND_CSD: //cmd9
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x42; //SD_R2;
            cmd_info->rsp_para3 = 0x05;
            break;
        case MMC_SEND_CID://cmd10
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x42; //SD_R2;
            cmd_info->rsp_para3 = 0x5;
            break;
        case SD_SWITCH_VOLTAGE: //cmd11
            cmd_info->rsp_para1 = -1; //0;
            cmd_info->rsp_para2 = 0x41; //don't update
            cmd_info->rsp_para3 = 0x00; //don't update
            break;
        case MMC_STOP_TRANSMISSION: //cmd12
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x7C; //SD_R1 | CRC16_CAL_DIS;
            cmd_info->rsp_para3 = 0x00;
            break;
        case SD_APP_SD_STATUS: //cmd13
            if((cmd_info->cmd->flags & (0x3<<5)) == MMC_CMD_ADTC){ //SD_APP_SD_STATUS
                cmd_info->rsp_para1 = -1;
                cmd_info->rsp_para2 = 0x01; //SD_R1 | CRC16_CAL_DIS;
                cmd_info->rsp_para3 = 0x05;
            }else{
                cmd_info->rsp_para1 = -1;
                cmd_info->rsp_para2 = 0x41; //SD_R1 | CRC16_CAL_DIS;
                cmd_info->rsp_para3 = -1;
            }
            break;
        case MMC_GO_INACTIVE_STATE: //cmd15
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_SET_BLOCKLEN: //cmd16
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x41;
            cmd_info->rsp_para3 = 0x05;
            break;
        case MMC_READ_SINGLE_BLOCK: //cmd17
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x1;
            cmd_info->rsp_para3 = -1;
            break;
        case MMC_READ_MULTIPLE_BLOCK: //cmd18
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            if(mmc_detected == 1){
                cmd_info->rsp_para3 = 0x04; //0x04;
            }else{
                cmd_info->rsp_para3 = 0x00;//0x02;  ----jim
            }
            break;
        case MMC_WRITE_DAT_UNTIL_STOP: //cmd20
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case SD_APP_SEND_NUM_WR_BLKS: //cmd22
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_SET_BLOCK_COUNT: //cmd 23
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_WRITE_BLOCK: //cmd24
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            cmd_info->rsp_para3 = -1;
            break;
        case MMC_WRITE_MULTIPLE_BLOCK: //cmd25
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            cmd_info->rsp_para3 = 0x00;//0x02;--------jim
            break;
        case MMC_PROGRAM_CSD: //cmd27
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_SET_WRITE_PROT: //cmd28
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_CLR_WRITE_PROT: //cmd29
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_SEND_WRITE_PROT: //cmd30
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case SD_ERASE_WR_BLK_START: //cmd32
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case SD_ERASE_WR_BLK_END: //cmd33
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_ERASE: //cmd38
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case SD_APP_OP_COND: //cmd41
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x45;
            cmd_info->rsp_para3 = 0x00;
            break;
        case MMC_LOCK_UNLOCK: //cmd42
            if((cmd_info->cmd->flags & (0x3<<5)) == MMC_CMD_ADTC){ //SD_APP_SD_STATUS
                cmd_info->rsp_para1 = -1; //don't update
                cmd_info->rsp_para3 = -1; //don't update
            }else{
                cmd_info->rsp_para1 = -1; //don't update
                cmd_info->rsp_para3 = -1; //don't update
            }
            break;
        case SD_APP_SEND_SCR: //cmd51
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x41;
            cmd_info->rsp_para3 = 0x05;
            break;
        case MMC_APP_CMD: //cmd55
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x41;
            cmd_info->rsp_para3 = 0x05;
            break;
        case MMC_GEN_CMD: //cmd56
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
        case MMC_EXT_READ_SINGLE: //cmd48
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            cmd_info->rsp_para3 = -1;
            break;
        case MMC_EXT_WRITE_SINGLE: //cmd49
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            cmd_info->rsp_para3 = -1;
            break;
        case MMC_EXT_READ_MULTIPLE: //cmd58
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            if(mmc_detected == 1)
                cmd_info->rsp_para3 = 0x04;
            else
                cmd_info->rsp_para3 = 0x02;
            break;
        case MMC_EXT_WRITE_MULTIPLE: //cmd59
            cmd_info->rsp_para1 = -1;
            cmd_info->rsp_para2 = 0x01;
            cmd_info->rsp_para3 = 0x02;
            break;
        default:
            cmd_info->rsp_para1 = -1; //don't update
            cmd_info->rsp_para3 = -1; //don't update
            break;
    }

    return 0;
}

/*static int rtk_scan_fun(struct rtk_sdmmc_host *rtk_host) {
        while(1) {
                u32 int_status = 0;

                rtk_sdmmc_sync(rtk_host);
                int_status = readl(rtk_host->sdmmc + CR_SD_ISR);
                if(int_status & (ISRSTA_INT1 | ISRSTA_INT2 | ISRSTA_INT4)) {
                        TOGGLE_GPIO_LOW18;
                        TOGGLE_GPIO_HIGH18;
                }
        }
}*/

static int rtk_sdmmc_stream_cmd(u16 cmdcode, struct sdmmc_cmd_pkt *cmd_info, u8 data_len)
{
    u8 cmd_idx = cmd_info->cmd->opcode;
    u32 sd_arg = 0;
    s8 rsp_para1 = 0;
    s8 rsp_para2 = 0;
    s8 rsp_para3 = 0;
    int rsp_len = cmd_info->rsp_len;
    int ret = 0;
    u32 data = cmd_info->dma_buffer;
    u32 sa = 0;
    u32 *rsp = (u32 *)&cmd_info->cmd->resp;
    u16 byte_count = cmd_info->byte_count;
    u16 block_count = cmd_info->block_count;

    INT4_flag=true;
    struct rtk_sdmmc_host *rtk_host = cmd_info->rtk_host;
    void __iomem *sdmmc_base = rtk_host->sdmmc;

    if ((data & 7) || !rsp)
        return CR_TRANSFER_FAIL;

    rtk_sdmmc_set_rspparam(cmd_info);
    sd_arg = cmd_info->cmd->arg;

    //if (rsp_para1 != -1)
    rsp_para1 = cmd_info->rsp_para1;

    rsp_para2 = cmd_info->rsp_para2;

    //if(rsp_para3 != -1)
    rsp_para3 = cmd_info->rsp_para3;

    sa = (data / 8);
    //sa = ((u32)data / 8);


    writeb(0x40 | cmd_idx, sdmmc_base + SD_CMD0);
    writeb((sd_arg >> 24) & 0xff, sdmmc_base + SD_CMD1);
    writeb((sd_arg >> 16) & 0xff, sdmmc_base + SD_CMD2);
    writeb((sd_arg >> 8) & 0xff, sdmmc_base + SD_CMD3);
    writeb(sd_arg & 0xff, sdmmc_base + SD_CMD4);
    writeb(0, sdmmc_base + SD_CMD5);

    if(rsp_para1 != -1){
        writeb(readb(sdmmc_base + SD_CONFIGURE1) | rsp_para1, sdmmc_base + SD_CONFIGURE1); //0x0C
    }

    writeb(rsp_para2, sdmmc_base + SD_CONFIGURE2); //0x0C

    if(rsp_para3 != -1){
        writeb(readb(sdmmc_base + SD_CONFIGURE3) | rsp_para3, sdmmc_base + SD_CONFIGURE3); //0x0C
    }

    writeb(byte_count, sdmmc_base + SD_BYTE_CNT_L); //0x24
    writeb(byte_count >> 8, sdmmc_base + SD_BYTE_CNT_H); //0x28
    writeb(block_count, sdmmc_base + SD_BLOCK_CNT_L); //0x2C
    writeb(block_count >> 8, sdmmc_base + SD_BLOCK_CNT_H); //0x30

    if(cmd_info->cmd->data->flags & MMC_DATA_READ){
        dma_wmb();
        writel((u32)sa, sdmmc_base + CR_SD_DMA_CTL1);
        writel(block_count, sdmmc_base + CR_SD_DMA_CTL2);

        if(data_len == RESP_LEN64){
            writel(RSP64_SEL | DDR_WR | DMA_XFER, sdmmc_base + CR_SD_DMA_CTL3);
        }else if(data_len == RESP_LEN17){
            writel(RSP17_SEL | DDR_WR | DMA_XFER, sdmmc_base + CR_SD_DMA_CTL3);
        }else
            writel(DDR_WR | DMA_XFER, sdmmc_base + CR_SD_DMA_CTL3);

    } else if (cmd_info->cmd->data->flags & MMC_DATA_WRITE) {
        dma_wmb();
        writel(sa, sdmmc_base + CR_SD_DMA_CTL1);
        writel(block_count, sdmmc_base + CR_SD_DMA_CTL2);
        writel(DMA_XFER, sdmmc_base + CR_SD_DMA_CTL3);
    }

    rtk_host->cmd_opcode = cmd_idx;
    ret = rtk_sdmmc_int_wait(DRIVER_NAME, rtk_host, cmdcode);
    if (ret == CR_TRANS_OK)
        rtk_sdmmc_read_rsp(rtk_host, rsp, rsp_len);

    return ret;
}

static int rtk_sdmmc_stream(struct sdmmc_cmd_pkt *pkt)
{
    struct rtk_sdmmc_host *host = pkt->rtk_host;
    struct mmc_data *data = pkt->data;
    unsigned int len = data->blksz * data->blocks;
    u8 opcode = pkt->cmd->opcode, data_len = 0;
    u16 cmdcode;
    int ret;

    data->bytes_xfered = 0;
    if (!(data->flags & (MMC_DATA_READ | MMC_DATA_WRITE)) || data->blocks != 1 ||
        !len || len > SD_ALLOC_LENGTH)
        return CR_TRANSFER_FAIL;

    memset(host->bounce, 0, SD_ALLOC_LENGTH);
    if ((data->flags & MMC_DATA_WRITE) &&
        (opcode != MMC_WRITE_BLOCK || len != 512 ||
         sg_copy_to_buffer(data->sg, data->sg_len, host->bounce, len) != len))
        return CR_TRANSFER_FAIL;
    pkt->dma_buffer = (u32)host->paddr;
    pkt->block_count = 1;
    if (opcode == SD_SWITCH || opcode == SD_APP_SD_STATUS ||
        opcode == SD_APP_SEND_SCR || opcode == SD_APP_SEND_NUM_WR_BLKS) {
        if (len > 64)
            return CR_TRANSFER_FAIL;
        pkt->byte_count = 64;
        data_len = RESP_LEN64;
    } else {
        if (len != 512)
            return CR_TRANSFER_FAIL;
        pkt->byte_count = 512;
    }
    cmdcode = host->ops->chk_cmdcode(pkt->cmd);
    ret = rtk_sdmmc_stream_cmd(cmdcode, pkt, data_len);
    if (ret != CR_TRANS_OK)
        return ret;
    if (data->flags & MMC_DATA_READ) {
        dma_rmb();
        if (sg_copy_from_buffer(data->sg, data->sg_len, host->bounce, len) != len)
            return CR_TRANSFER_FAIL;
    }
    data->bytes_xfered = len;
    return CR_TRANS_OK;
}

static void rtk_sdmmc_swap_data(u8 *buffer, void __iomem *sdmmc_base)
{
    buffer[3] = buffer[2];
    buffer[2] = buffer[1];
    buffer[1] = buffer[0];
    buffer[0] = buffer[3+4];
    buffer[3+4] = buffer[3+3];
    buffer[3+3] = buffer[3+2];
    buffer[3+2] = buffer[3+1];
    buffer[3+1] = buffer[7+4];
    buffer[7+4] = buffer[7+3];
    buffer[7+3] = buffer[7+2];
    buffer[7+2] = buffer[7+1];
    buffer[7+1] = buffer[11+4];
    buffer[11+4] = buffer[11+3];
    buffer[11+3] = buffer[11+2];
    buffer[11+2] = buffer[11+1];
    buffer[11+1] = readb(sdmmc_base + SD_CMD5);
}

static int rtk_sdmmc_send_cmd_get_rsp(struct sdmmc_cmd_pkt *cmd_info)
{
    u8 cmd_idx = cmd_info->cmd->opcode;
    u32 sd_arg = 0;
    s8 rsp_para1 = 0;
    s8 rsp_para2 = 0;
    s8 rsp_para3 = 0;
    u8 rsp_len = cmd_info->rsp_len;
    u32 *rsp = (u32 *)&cmd_info->cmd->resp;
    u32 dma_val = 0;
    u32 byte_count = 0x200;
    u32 block_count = 1;
    u64 sa = 0;
    u8 RESP17_buffer[16];
    int ret = 0;

    struct rtk_sdmmc_host *rtk_host = cmd_info->rtk_host;
    void __iomem *sdmmc_base = rtk_host->sdmmc;

    INT4_flag=false;

    rtk_sdmmc_set_rspparam(cmd_info);

    sd_arg = cmd_info->cmd->arg;
    rsp_para1 = cmd_info->rsp_para1;
    rsp_para2 = cmd_info->rsp_para2;
    rsp_para3 = cmd_info->rsp_para3;

    if(rsp == NULL){
        BUG_ON(1);
    }

    if ((cmd_idx == SD_IO_SEND_OP_COND)|(cmd_idx == SD_IO_RW_DIRECT)|(cmd_idx == SD_IO_RW_EXTENDED)){
        printk(KERN_INFO "%s : reject SDIO commands cmd:0x%02x \n", __func__, cmd_idx);
        return CR_TRANSFER_FAIL;
    }

    if (rsp_para1 != -1)
        writeb(rsp_para1, sdmmc_base + SD_CONFIGURE1);

    writeb(rsp_para2, sdmmc_base + SD_CONFIGURE2);

    if (rsp_para3 != -1)
        writeb(rsp_para3, sdmmc_base + SD_CONFIGURE3);

    writeb(0x40 | cmd_idx, sdmmc_base + SD_CMD0);
    writeb((sd_arg >> 24) & 0xff, sdmmc_base + SD_CMD1);
    writeb((sd_arg >> 16) & 0xff, sdmmc_base + SD_CMD2);
    writeb((sd_arg >> 8) & 0xff, sdmmc_base + SD_CMD3);
    writeb(sd_arg & 0xff, sdmmc_base + SD_CMD4);
    writeb(0, sdmmc_base + SD_CMD5);

    rtk_host->cmd_opcode = cmd_idx;


    if(RESP_TYPE_17B & rsp_para2){
        INT4_flag=true;
        /*remap the resp dst buffer to un-cache*/
        sa = rtk_host->paddr;
        sa = sa / 8;
        dma_val = RSP17_SEL | DDR_WR | DMA_XFER;
        writeb(byte_count, sdmmc_base + SD_BYTE_CNT_L); //0x24
        writeb(byte_count>>8, sdmmc_base + SD_BYTE_CNT_H); //0x28
        writeb(block_count, sdmmc_base + SD_BLOCK_CNT_L); //0x2C
        writeb(block_count>>8, sdmmc_base + SD_BLOCK_CNT_H); //0x30

        dma_wmb();
        writel(sa, sdmmc_base + CR_SD_DMA_CTL1); //espeical for R2
        writel(0x00000001, sdmmc_base + CR_SD_DMA_CTL2); //espeical for R2
        writel(dma_val, sdmmc_base + CR_SD_DMA_CTL3); //espeical for R2

    }else if(RESP_TYPE_6B & rsp_para2){
        //do nothing
    }
    ret = rtk_sdmmc_int_wait(DRIVER_NAME, rtk_host, SD_SENDCMDGETRSP);
    if(ret == CR_TRANS_OK){
        if (rsp_len == 16) {
            dma_rmb();
            rtk_sdmmc_read_rsp(rtk_host, rtk_host->bounce, rsp_len);
            memcpy(RESP17_buffer, rtk_host->bounce, 16);
            rtk_sdmmc_swap_data(RESP17_buffer, sdmmc_base);
            memcpy(rsp, RESP17_buffer, 16);
        } else {
            rtk_sdmmc_read_rsp(rtk_host, rsp, rsp_len);
        }

        rtk_sdmmc_sync(rtk_host);

        if (cmd_idx == MMC_SET_RELATIVE_ADDR){
                    printk(KERN_INFO "SD/MMC card init done.\n");
        }
    }

    memset(rtk_host->bounce, 0, SD_ALLOC_LENGTH);

    return ret;
}

static void rtk_sdmmc_send_command(struct rtk_sdmmc_host *host,
                                    struct mmc_command *cmd)
{
    struct sdmmc_cmd_pkt pkt = { 0 };
    int ret;

    if (cmd->opcode == MMC_SEND_OP_COND)
        mmc_detected = 1;
    pkt.cmd = cmd;
    pkt.rtk_host = host;
    pkt.rsp_para2 = rtk_sdmmc_get_rsp_type(cmd);
    pkt.rsp_len = rtk_sdmmc_get_rsp_len(pkt.rsp_para2);
    pkt.data = cmd->data;
    ret = cmd->data ? rtk_sdmmc_stream(&pkt) : rtk_sdmmc_send_cmd_get_rsp(&pkt);
    if (ret) {
        cmd->error = ret == CR_TRANSFER_TO ? -ETIMEDOUT : -EIO;
        if (cmd->data)
            cmd->data->error = cmd->error;
        return;
    }
    if (cmd->opcode == MMC_SELECT_CARD && cmd->arg) {
        host->rtflags |= RTKCR_FCARD_SELECTED;
        rtk_sdmmc_speed(host, SDMMC_CLOCK_6200KHZ);
    } else if (cmd->opcode == SD_SEND_RELATIVE_ADDR &&
               (cmd->flags & (0x3 << 5)) == MMC_CMD_BCR) {
        sdmmc_rca = cmd->resp[0] >> RCA_SHIFTER;
    }
}

static void rtk_sdmmc_request_work(struct work_struct *work)
{
    struct rtk_sdmmc_host *host = container_of(work, struct rtk_sdmmc_host, request_work);
    struct mmc_request *mrq = host->mrq;

    mutex_lock(&host->io_mutex);
    if (host->stopping || !(host->rtflags & RTKCR_FCARD_DETECTED))
        mrq->cmd->error = -ENOMEDIUM;
    else if (host->dma_failed)
        mrq->cmd->error = -EIO;
    else if (mrq->cmd->opcode == MMC_SET_WRITE_PROT ||
             mrq->cmd->opcode == MMC_CLR_WRITE_PROT ||
             mrq->cmd->opcode == SD_ERASE_WR_BLK_START ||
             mrq->cmd->opcode == SD_ERASE_WR_BLK_END ||
             mrq->cmd->opcode == MMC_ERASE)
        mrq->cmd->error = -EROFS;
    else if (mrq->sbc || mrq->stop || (mrq->data && mrq->data->blocks != 1))
        mrq->cmd->error = -EOPNOTSUPP;
    else
        rtk_sdmmc_send_command(host, mrq->cmd);
    if (mrq->data && mrq->cmd->error && !mrq->data->error)
        mrq->data->error = mrq->cmd->error;
    host->mrq = NULL;
    mutex_unlock(&host->io_mutex);
    mmc_request_done(host->mmc, mrq);
}

static void rtk_sdmmc_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
    struct rtk_sdmmc_host *host = mmc_priv(mmc);
    if (WARN_ON(host->mrq)) {
        mrq->cmd->error = -EBUSY;
        mmc_request_done(mmc, mrq);
        return;
    }
    host->mrq = mrq;
    schedule_work(&host->request_work);
}

static int rtk_sdmmc_get_ro(struct mmc_host *host)
{
    struct rtk_sdmmc_host *rtk_host = mmc_priv(host);

    return !!(readb(rtk_host->sdmmc + CARD_EXIST) & SD_WRITE_PROTECT);
}

static int rtk_sdmmc_one_block(struct mmc_card *card, unsigned int dir, int blocks)
{
    return 1;
}

static int rtk_sdmmc_get_cd(struct mmc_host *host)
{
    struct rtk_sdmmc_host *rtk_host = mmc_priv(host);
    void __iomem *sdmmc_base = rtk_host->sdmmc;
    u32 reginfo = 0;

    reginfo = readb(sdmmc_base + CARD_EXIST);
    return !!(reginfo & SD_EXISTENCE);
}

static void rtk_sdmmc_set_ios(struct mmc_host *host, struct mmc_ios *ios)
{
    struct rtk_sdmmc_host *rtk_host = mmc_priv(host);
    unsigned int clock = min_t(unsigned int, ios->clock, 25000000);

    mutex_lock(&rtk_host->io_mutex);
    if (ios->bus_mode == MMC_BUSMODE_OPENDRAIN) {
        rtk_sdmmc_set_bits(rtk_host, BUS_WIDTH_1);
        rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_400KHZ);
    } else {
        if (ios->bus_width == MMC_BUS_WIDTH_4)
            rtk_sdmmc_set_bits(rtk_host, BUS_WIDTH_4);
        else
            rtk_sdmmc_set_bits(rtk_host, BUS_WIDTH_1);
        if (clock >= 25000000) {
            rtk_sdmmc_set_access_mode(rtk_host, ACCESS_MODE_SD20);
            rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_25000KHZ);
        } else if (rtk_host->rtflags & RTKCR_FCARD_SELECTED) {
            rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_6200KHZ);
        } else {
            rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_400KHZ);
        }
    }
    if (ios->power_mode == MMC_POWER_UP)
        rtk_host->ops->card_power(rtk_host, 1);
    else if (ios->power_mode == MMC_POWER_OFF)
        rtk_host->ops->card_power(rtk_host, 0);
    mutex_unlock(&rtk_host->io_mutex);
}

static void rtk_sdmmc_hw_initial(struct rtk_sdmmc_host *rtk_host)
{
    void __iomem *sdmmc_base = rtk_host->sdmmc;
    void __iomem *pll_base = rtk_host->pll;
    void __iomem *sysbrdg_base = rtk_host->sysbrdg;
    int bErrorRetry_1 = 0;

    /*SD PLL Initialization, jamestai20150721*/
    writel(readl(pll_base + CR_PLL_SD4) | 0x00000004, pll_base + CR_PLL_SD4);
    writel(readl(pll_base + CR_PLL_SD4) | 0x00000007, pll_base + CR_PLL_SD4);

    writel(0x00002003, pll_base + CR_PLL_SD1); //PLL_SD1

    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x40000000,sdmmc_base+0x2c);
    udelay(100);
    writel(0x00000006, pll_base + 0x01EC);   //JIM modified, 1AC->1EC, we modified this part because 1AC is to change SDIO PLL instead of SD card PLL
    writel(readl(sdmmc_base + CR_SD_CKGEN_CTL) | 0x00070000, sdmmc_base + CR_SD_CKGEN_CTL); //Switch SD source clock to 4MHz by Hsin-yin
    mdelay(2);
    writel(0x00000007, pll_base + 0x01EC);    //JIM modified, 1AC->1EC
    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x00000000,sdmmc_base+0x2c);

    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x40000000,sdmmc_base+0x2c);
    udelay(100);
    writel(0x00000006, pll_base + 0x01EC);     //JIM modified, 1AC->1EC
    writel(0x04517893, pll_base + CR_PLL_SD2); //Reduce the impact of spectrum by Hsin-yin, jamestai20150302
    writel(0x00564388, pll_base + CR_PLL_SD3); //Set PLL clock rate, default clock 100MHz
        //ritel(0x00324388,  pll_base + CR_PLL_SD3);
    mdelay(2);
    writel(0x00000007, pll_base + 0x01EC);      //JIM modified, 1AC->1EC
    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x00000000,sdmmc_base+0x2c);
    /* Enable SD host clock*/
    //writel(readl(pll_base + 0x04) | (0x1 << 10), pll_base + 0x04);        //jim comment for fear that it will reset the SD CRT
    /* CR and SD_IP gates are held through the shared clock provider. */

    //writel(readl(sdmmc_base + 0x20) | 0x00000002, sdmmc_base + 0x20);    //add by jim for test, change the state to reset the SD IP
    //writel(readl(sdmmc_base + 0x20) | 0x00000003, sdmmc_base + 0x20);

    writel(readl(sdmmc_base + 0x20) |0x02, sdmmc_base + 0x20);    //Disable L4 gate

    writel(readl(sdmmc_base + 0x20) & (~0x01), sdmmc_base + 0x20);
    writel(readl(sdmmc_base + 0x20) | 0x00000001, sdmmc_base + 0x20);	//reset the DMA

    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x40000000,sdmmc_base+0x2c);
    udelay(100);
    writel(0x00000006, pll_base + 0x01EC);       //JIM modified, 1AC->1EC
    writel(readl(sdmmc_base + CR_SD_CKGEN_CTL) & 0xFFF8FFFF, sdmmc_base + CR_SD_CKGEN_CTL); //Switch SD source clock to normal clock source by Hsin-yin
    udelay(100);
    writel(0x00000007, pll_base + 0x01EC);        //JIM modified, 1AC->1EC
    if(readl(sysbrdg_base+0x204)!=0x0)
        writel(0x00000000,sdmmc_base+0x2c);

    writeb(readb(sdmmc_base + SD_CONFIGURE1) & 0x000000EF, sdmmc_base + SD_CONFIGURE1); //Reset FIFO pointer by Hsin-yin
    writel(0x00000007, pll_base + CR_PLL_SD4); //PLL_SD4
    udelay(100);
    writeb(0xD0, sdmmc_base + SD_CONFIGURE1);

    rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_400KHZ);

    writel(0x00000000, sdmmc_base + CR_SD_PAD_CTL); //change to 3.3v

    writel(0x00000016, sdmmc_base + CR_SD_ISR); //enable interrupt
    writel(0x00000017, sdmmc_base + CR_SD_ISREN);
    rtk_sdmmc_sync(rtk_host);

    writeb(0x02, sdmmc_base + CARD_SELECT); //for emmc, select SD ip
    if(bErrorRetry_1){
        writeb(0x08, sdmmc_base + SD_SAMPLE_POINT_CTL); //sample point = SDCLK / 4
        writeb(0x10, sdmmc_base + SD_PUSH_POINT_CTL); //output ahead SDCLK /4
    }else{
        writeb(0x00, sdmmc_base + SD_SAMPLE_POINT_CTL); //sample point = SDCLK / 4
        writeb(0x00, sdmmc_base + SD_PUSH_POINT_CTL); //output ahead SDCLK /4
    }
}

static const struct mmc_host_ops rtk_sdmmc_ops ={
    .request = rtk_sdmmc_request,
    .get_ro = rtk_sdmmc_get_ro,
    .multi_io_quirk = rtk_sdmmc_one_block,
    .get_cd = rtk_sdmmc_get_cd,
    .set_ios = rtk_sdmmc_set_ios,
};

static void rtk_sdmmc_plug(struct work_struct *work)
{
    u32 reginfo = 0;
    u32 det_time = 0;
    unsigned long timeout = 0;
    unsigned long flags2;

    struct rtk_sdmmc_host *rtk_host = container_of(to_delayed_work(work), struct rtk_sdmmc_host, plug_timer);
    void __iomem *sdmmc_base = rtk_host->sdmmc;


    mutex_lock(&rtk_host->io_mutex);
    if (rtk_host->stopping) {
        mutex_unlock(&rtk_host->io_mutex);
        return;
    }
    reginfo = readb(sdmmc_base + CARD_EXIST);
    if((reginfo & SD_EXISTENCE) ^ (rtk_host->int_status_old & SD_EXISTENCE)){
        rtk_host->rtflags &= ~RTKCR_FCARD_DETECTED;
        if(reginfo & SD_EXISTENCE){
            if (rtk_host->ops->card_power(rtk_host, 1))
                goto requeue;

            rtk_host->rtflags |= RTKCR_FCARD_DETECTED;
            det_time = 1;


            writel(0x00000000, sdmmc_base + CR_SD_DMA_CTL3); //stop dma control
            writeb(0x00, sdmmc_base + CR_CARD_STOP); //SD Card module transfer no stop
            writeb(0x02, sdmmc_base + CARD_SELECT); //Specify the current active card module for the coming data transfer, bit 2:0 = 010
            writeb(0x04, sdmmc_base + CR_CARD_OE); //arget module is SD/MMC card module, bit 2 =1
            writeb(0x04, sdmmc_base + CARD_CLOCK_EN_CTL); //SD Card Module Clock Enable, bit 2 = 1
            writeb(0xD0, sdmmc_base + SD_CONFIGURE1);
            rtk_sdmmc_speed(rtk_host, SDMMC_CLOCK_400KHZ);
            writeb(0x00, sdmmc_base + SD_STATUS2);
            printk(KERN_ERR "SD card is being inserted now...!!!\n");
        }else{
            rtk_host->rtflags &= ~RTKCR_FCARD_DETECTED;
            det_time = 1;
            /*reset*/
            timeout = jiffies + msecs_to_jiffies(100);
            while(time_before(jiffies, timeout)){
                rtk_lockapi_lock(flags2, __FUNCTION__);
                if((!(readl(rtk_host->emmc + EMMC_DMA_CTL3) & 0x01)) /*&&
                    (readb(rtk_host->sdio + SDIO_NORML_INT_STA) & 0x02)*/){ //;  skip this comment first and need to check if this is a mistake
                        rtk_lockapi_unlock(flags2, __FUNCTION__);
                        break;
                }
                rtk_lockapi_unlock(flags2, __FUNCTION__);
            }
            //writel(readl(pll_base + 0x0C) | (0x1 << 25) | (0x1 << 31), pll_base + 0x0C);
            //writel(readl(sdmmc_base + 0x20) | 0x00000002, sdmmc_base + 0x20);    //add by jim for test, change the state to reset the SD IP
            //writel(readl(sdmmc_base + 0x20) | 0x00000003, sdmmc_base + 0x20);

            writel(0x0, sdmmc_base + CR_SD_DMA_CTL3); //stop dma control
            writeb(0xff, sdmmc_base + CR_CARD_STOP); //SD Card module transfer stop and idle state

            writeb(0x3b, sdmmc_base + CARD_CLOCK_EN_CTL);

            rtk_host->ops->card_power(rtk_host, 0); //power off, Jim add
            printk(KERN_ERR "SD card is being removed now...!!!\n");
        }
        rtk_sdmmc_sync(rtk_host);
        rtk_lockapi_lock(flags2, __FUNCTION__);
        printk(KERN_INFO "[SD] SD card power register=%x\n",readl(rtk_host->emmc + CR_PFUNC_CR));
        rtk_lockapi_unlock(flags2, __FUNCTION__);

        rtk_host->rtflags &= ~RTKCR_FCARD_SELECTED;
        sdmmc_rca = 0;
        mmc_detect_change(rtk_host->mmc, msecs_to_jiffies(det_time));
    }
    rtk_host->int_status_old = reginfo;
requeue:
    if (!rtk_host->stopping)
        mod_delayed_work(system_wq, &rtk_host->plug_timer, HZ/10);
    mutex_unlock(&rtk_host->io_mutex);
}

static int rtk_sdmmc_card_power(struct rtk_sdmmc_host *host, u8 on)
{
    unsigned long flags;
    int ret;

    if (on == host->power_status)
        return 0;
    ret = on ? gpiod_direction_output_raw(host->sdmmc_gpio, 0) :
               gpiod_direction_input(host->sdmmc_gpio);
    if (ret) {
        dev_err(host->dev, "SD power %s failed: %d\n", on ? "on" : "off", ret);
        return ret;
    }
    rtk_lockapi_lock(flags, __FUNCTION__);
    writel(on ? 0x33333323 : 0x22223322, host->emmc + CR_PFUNC_CR);
    rtk_lockapi_unlock(flags, __FUNCTION__);
    host->power_status = on;
    msleep(10);
    return 0;
}

static u8 rtk_sdmmc_chk_cmdcode(struct mmc_command *cmd)
{
    if (WARN_ON_ONCE(cmd->data == NULL) || cmd->opcode >= ARRAY_SIZE(rtk_sdmmc_cmdcode))
        return SD_CMD_UNKNOW;
    if (cmd->opcode == 8 && mmc_detected)
        return SD_NORMALREAD;
    return rtk_sdmmc_cmdcode[cmd->opcode][0];
}

static irqreturn_t rtk_sdmmc_irq(int irq, void *data)
{
    struct rtk_sdmmc_host *host = data;
    unsigned long flags;
    u32 status = readl(host->sdmmc + CR_SD_ISR) & 0x16;

    if (!status)
        return IRQ_NONE;
    writel(status, host->sdmmc + CR_SD_ISR);
    rtk_sdmmc_sync(host);
    spin_lock_irqsave(&host->lock, flags);
    if (host->command_active) {
        host->irq_status |= status;
        if ((host->irq_status & ISRSTA_INT2) ||
            ((host->irq_status & ISRSTA_INT1) &&
             (!host->expect_dma || (host->irq_status & ISRSTA_INT4))))
            complete(&host->done);
    }
    spin_unlock_irqrestore(&host->lock, flags);
    return IRQ_HANDLED;
}

static struct rtk_sdmmc_host_ops sdmmc_ops = {
    .card_power = rtk_sdmmc_card_power,
    .chk_cmdcode = rtk_sdmmc_chk_cmdcode,
};

static const struct of_device_id rtk_sdmmc_match[] = {
    { .compatible = "Realtek,rtk1295-sdmmc", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, rtk_sdmmc_match);

static void rtk_sdmmc_restore_pads(struct rtk_sdmmc_host *host)
{
    unsigned long flags;
    int ret;

    if (!host->pads_saved)
        return;
    rtk_lockapi_lock(flags, __FUNCTION__);
    writel((readl(host->emmc + SD_PINMUX_OFFSET) & ~SD_PINMUX_MASK) |
           host->pinmux_saved, host->emmc + SD_PINMUX_OFFSET);
    writel(host->pfunc_saved, host->emmc + CR_PFUNC_CR);
    rtk_lockapi_unlock(flags, __FUNCTION__);
    ret = host->gpio_direction_saved ? gpiod_direction_input(host->sdmmc_gpio) :
          gpiod_direction_output_raw(host->sdmmc_gpio, host->gpio_value_saved);
    if (ret)
        dev_err(host->dev, "cannot restore SD power GPIO: %d\n", ret);
    host->pads_saved = false;
}

static int rtk_sdmmc_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *node = dev->of_node;
    struct mmc_host *mmc;
    struct rtk_sdmmc_host *host;
    int ret;
    unsigned long flags;

    mmc = mmc_alloc_host(sizeof(*host), dev);
    if (!mmc)
        return -ENOMEM;
    host = mmc_priv(mmc);
    mmc_detected = 0;
    sdmmc_rca = 0;
    host->mmc = mmc;
    host->dev = dev;
    host->ops = &sdmmc_ops;
    mutex_init(&host->io_mutex);
    spin_lock_init(&host->lock);
    init_completion(&host->done);
    INIT_WORK(&host->request_work, rtk_sdmmc_request_work);
    INIT_DELAYED_WORK(&host->plug_timer, rtk_sdmmc_plug);

    host->pll = of_iomap(node, 0);
    host->sdmmc = of_iomap(node, 1);
    host->sysbrdg = of_iomap(node, 2);
    host->emmc = of_iomap(node, 3);
    host->sdio = of_iomap(node, 4);
    if (!host->pll || !host->sdmmc || !host->sysbrdg || !host->emmc || !host->sdio) {
        ret = -ENOMEM;
        goto unmap;
    }
    host->sdmmc_gpio = devm_gpiod_get(dev, "power", GPIOD_ASIS);
    if (IS_ERR(host->sdmmc_gpio)) {
        ret = PTR_ERR(host->sdmmc_gpio);
        goto unmap;
    }
    /* The CR gate is shared with eMMC; acquire both through CCF. */
    {
        struct clk *clk = devm_clk_get_enabled(dev, "cr");
        if (IS_ERR(clk)) {
            ret = PTR_ERR(clk);
            goto unmap;
        }
        clk = devm_clk_get_enabled(dev, "sd-ip");
        if (IS_ERR(clk)) {
            ret = PTR_ERR(clk);
            goto unmap;
        }
    }
    ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(30));
    if (ret)
        goto unmap;
    host->bounce = dma_alloc_coherent(dev, SD_ALLOC_LENGTH, &host->paddr, GFP_KERNEL);
    if (!host->bounce) {
        ret = -ENOMEM;
        goto unmap;
    }
    memset(host->bounce, 0, SD_ALLOC_LENGTH);
    if (!IS_ALIGNED(host->paddr, 8) || upper_32_bits(host->paddr)) {
        ret = -ERANGE;
        goto free_dma;
    }
    host->irq = platform_get_irq(pdev, 0);
    if (host->irq < 0) {
        ret = host->irq;
        goto free_dma;
    }
    /* Keep the line masked until the handler and all state exist. */
    writel(0, host->sdmmc + CR_SD_ISREN);
    ret = request_irq(host->irq, rtk_sdmmc_irq, IRQF_SHARED, DRIVER_NAME, host);
    if (ret)
        goto free_dma;
    host->gpio_direction_saved = gpiod_get_direction(host->sdmmc_gpio);
    if (host->gpio_direction_saved < 0) {
        ret = host->gpio_direction_saved;
        goto free_irq;
    }
    if (!host->gpio_direction_saved) {
        host->gpio_value_saved = gpiod_get_raw_value_cansleep(host->sdmmc_gpio);
        if (host->gpio_value_saved < 0) {
            ret = host->gpio_value_saved;
            goto free_irq;
        }
    }
    /* Force the first power transition even if firmware left the pin driven. */
    host->power_status = U32_MAX;
    rtk_lockapi_lock(flags, __FUNCTION__);
    host->pinmux_saved = readl(host->emmc + SD_PINMUX_OFFSET) & SD_PINMUX_MASK;
    host->pfunc_saved = readl(host->emmc + CR_PFUNC_CR);
    writel((readl(host->emmc + SD_PINMUX_OFFSET) & ~SD_PINMUX_MASK) |
           SD_PINMUX_SD_CARD, host->emmc + SD_PINMUX_OFFSET);
    rtk_lockapi_unlock(flags, __FUNCTION__);
    host->pads_saved = true;
    rtk_sdmmc_hw_initial(host);
    mmc->ops = &rtk_sdmmc_ops;
    mmc->ocr_avail = MMC_VDD_30_31 | MMC_VDD_31_32 | MMC_VDD_32_33 | MMC_VDD_33_34;
    mmc->caps = MMC_CAP_4_BIT_DATA;
    mmc->caps2 = MMC_CAP2_NO_SDIO | MMC_CAP2_NO_MMC;
    mmc->f_min = 400000;
    mmc->f_max = 25000000;
    mmc->max_segs = 8;
    mmc->max_blk_size = 512;
    mmc->max_blk_count = 8;
    mmc->max_seg_size = 4096;
    mmc->max_req_size = 4096;
    platform_set_drvdata(pdev, mmc);
    ret = mmc_add_host(mmc);
    if (ret)
        goto free_irq;
    mod_delayed_work(system_wq, &host->plug_timer, 0);
    dev_info(dev, "RTD1295 SD trial: 25 MHz, single-block I/O, 30-bit DMA\n");
    return 0;

free_irq:
    writel(0, host->sdmmc + CR_SD_ISREN);
    free_irq(host->irq, host);
    rtk_sdmmc_restore_pads(host);
free_dma:
    dma_free_coherent(dev, SD_ALLOC_LENGTH, host->bounce, host->paddr);
unmap:
    if (host->sdio) iounmap(host->sdio);
    if (host->emmc) iounmap(host->emmc);
    if (host->sysbrdg) iounmap(host->sysbrdg);
    if (host->sdmmc) iounmap(host->sdmmc);
    if (host->pll) iounmap(host->pll);
    mmc_free_host(mmc);
    return ret;
}

static void rtk_sdmmc_remove(struct platform_device *pdev)
{
    struct mmc_host *mmc = platform_get_drvdata(pdev);
    struct rtk_sdmmc_host *host = mmc_priv(mmc);

    mutex_lock(&host->io_mutex);
    host->stopping = true;
    mutex_unlock(&host->io_mutex);
    cancel_delayed_work_sync(&host->plug_timer);
    mmc_remove_host(mmc);
    cancel_work_sync(&host->request_work);
    writel(0, host->sdmmc + CR_SD_ISREN);
    free_irq(host->irq, host);
    rtk_sdmmc_reset(host);
    {
        u32 dma_state;
        if (readl_poll_timeout(host->sdmmc + CR_SD_DMA_CTL3, dma_state,
                               !(dma_state & DMA_XFER), 10, 100000))
            dev_err(host->dev, "DMA did not stop; retaining bounce buffer until reboot\n");
        else
            dma_free_coherent(host->dev, SD_ALLOC_LENGTH, host->bounce, host->paddr);
    }
    rtk_sdmmc_restore_pads(host);
    iounmap(host->sdio);
    iounmap(host->emmc);
    iounmap(host->sysbrdg);
    iounmap(host->sdmmc);
    iounmap(host->pll);
    mmc_free_host(mmc);
}

static int rtk_sdmmc_suspend(struct device *dev)
{
    /* Trial has no verified state restoration after system suspend. */
    return -EBUSY;
}
static DEFINE_SIMPLE_DEV_PM_OPS(rtk_sdmmc_pm, rtk_sdmmc_suspend, NULL);

static struct platform_driver rtk_sdmmc_driver = {
    .probe = rtk_sdmmc_probe,
    .remove = rtk_sdmmc_remove,
    .driver = {
        .name = DRIVER_NAME,
        .pm = pm_sleep_ptr(&rtk_sdmmc_pm),
        .of_match_table = rtk_sdmmc_match,
    },
};

module_platform_driver(rtk_sdmmc_driver);

MODULE_DESCRIPTION("Realtek SD/MMC host driver");
MODULE_AUTHOR("James Tai");
MODULE_LICENSE("GPL");
