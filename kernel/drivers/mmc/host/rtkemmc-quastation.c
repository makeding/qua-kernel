// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * KDDI Qua Station RTD1295 native eMMC host.
 *
 * The RTD1295 wrapper does not follow the generic DW-MMC IDMAC completion
 * contract.  This driver keeps the descriptor and completion sequence used by
 * the Qua Station 4.1.17 BSP while presenting a small modern mmc_host.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mmc/core.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/scatterlist.h>
#include <linux/sizes.h>
#include <linux/workqueue.h>

#include <asm/barrier.h>

#include <soc/realtek/rtd129x-rbus-guard.h>

#define QUA_BOUNCE_SIZE		SZ_512K
#define QUA_DESC_BYTES		4096
#define QUA_DESC_COUNT		(QUA_BOUNCE_SIZE / QUA_DESC_BYTES)

#define QUA_CTRL		0x000
#define QUA_PWREN		0x004
#define QUA_CLKDIV		0x008
#define QUA_CLKSRC		0x00c
#define QUA_CLKENA		0x010
#define QUA_TMOUT		0x014
#define QUA_CTYPE		0x018
#define QUA_BLKSIZ		0x01c
#define QUA_BYTCNT		0x020
#define QUA_INTMASK		0x024
#define QUA_CMDARG		0x028
#define QUA_CMD			0x02c
#define QUA_RESP0		0x030
#define QUA_RESP1		0x034
#define QUA_RESP2		0x038
#define QUA_RESP3		0x03c
#define QUA_RINTSTS		0x044
#define QUA_STATUS		0x048
#define QUA_FIFOTH		0x04c
#define QUA_TCBCNT		0x05c
#define QUA_TBBCNT		0x060
#define QUA_BMOD		0x080
#define QUA_DBADDR		0x088
#define QUA_IDSTS		0x08c
#define QUA_IDINTEN		0x090
#define QUA_CARD_THR_CTL	0x100
#define QUA_DDR_REG		0x10c
#define QUA_UHSREG		0x074
#define QUA_CP			0x41c
#define QUA_WRAPPER_ISR		0x424
#define QUA_DQS_CTRL1		0x498
#define QUA_SWC_SEL		0x4d4

#define QUA_SB2_SYNC		0x020

#define QUA_CTRL_INT_ENABLE	BIT(4)
#define QUA_CTRL_USE_IDMAC	BIT(25)

#define QUA_CMD_START		BIT(31)
#define QUA_CMD_USE_HOLD	BIT(29)
#define QUA_CMD_UPDATE_CLK	BIT(21)
#define QUA_CMD_WAIT_PRV	BIT(13)
#define QUA_CMD_AUTO_STOP	BIT(12)
#define QUA_CMD_WRITE		BIT(10)
#define QUA_CMD_DATA_EXP	BIT(9)
#define QUA_CMD_RESP_CRC	BIT(8)
#define QUA_CMD_RESP_LONG	BIT(7)
#define QUA_CMD_RESP_EXP	BIT(6)

#define QUA_INT_EBE		BIT(15)
#define QUA_INT_ACD		BIT(14)
#define QUA_INT_SBE		BIT(13)
#define QUA_INT_HLE		BIT(12)
#define QUA_INT_FRUN		BIT(11)
#define QUA_INT_HTO		BIT(10)
#define QUA_INT_DRTO		BIT(9)
#define QUA_INT_RTO		BIT(8)
#define QUA_INT_DCRC		BIT(7)
#define QUA_INT_RCRC		BIT(6)
#define QUA_INT_RXDR		BIT(5)
#define QUA_INT_TXDR		BIT(4)
#define QUA_INT_DTO		BIT(3)
#define QUA_INT_CMD_DONE	BIT(2)
#define QUA_INT_RE		BIT(1)
#define QUA_INT_ERRORS		(QUA_INT_EBE | QUA_INT_SBE | QUA_INT_HLE | \
				 QUA_INT_FRUN | QUA_INT_HTO | QUA_INT_DRTO | \
				 QUA_INT_RTO | QUA_INT_DCRC | QUA_INT_RCRC | \
				 QUA_INT_RE)

#define QUA_STATUS_BUSY		BIT(9)
#define QUA_WRAPPER_DMA_DONE	BIT(1)

#define QUA_DES0_DIC		BIT(1)
#define QUA_DES0_LD		BIT(2)
#define QUA_DES0_FD		BIT(3)
#define QUA_DES0_CH		BIT(4)
#define QUA_DES0_OWN		BIT(31)

struct qua_desc {
	__le32 des0;
	__le32 des1;
	__le32 des2;
	__le32 des3;
};

struct qua_rtkemmc {
	struct device *dev;
	struct mmc_host *mmc;
	void __iomem *regs;
	void __iomem *crt;
	void __iomem *sb2;
	void __iomem *misc;
	struct clk *biu_clk;
	struct clk *ciu_clk;
	struct workqueue_struct *wq;
	struct work_struct request_work;
	struct mmc_request *mrq;
	struct qua_desc *descs;
	dma_addr_t desc_dma;
	void *bounce;
	dma_addr_t bounce_dma;
	unsigned int clock;
};

static u32 qua_readl(struct qua_rtkemmc *host, u32 reg)
{
	unsigned long flags;
	u32 value;

	rtd129x_rbus_guard_lock(&flags);
	value = readl(host->regs + reg);
	rtd129x_rbus_guard_unlock(flags);
	return value;
}

static void qua_writel(struct qua_rtkemmc *host, u32 value, u32 reg)
{
	unsigned long flags;

	rtd129x_rbus_guard_lock(&flags);
	dmb(sy);
	writel_relaxed(0, host->sb2 + QUA_SB2_SYNC);
	dmb(sy);
	writel(value, host->regs + reg);
	rtd129x_rbus_guard_unlock(flags);
}

static int qua_wait_bits(struct qua_rtkemmc *host, u32 reg, u32 mask,
			 u32 value, unsigned int timeout_ms)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(timeout_ms);

	do {
		if ((qua_readl(host, reg) & mask) == value)
			return 0;
		usleep_range(10, 30);
	} while (time_before(jiffies, timeout));

	return -ETIMEDOUT;
}

static void qua_read_response(struct qua_rtkemmc *host,
			      struct mmc_command *cmd)
{
	if (cmd->flags & MMC_RSP_136) {
		/* Native BSP ordering, matching the MMC core's response layout. */
		cmd->resp[3] = qua_readl(host, QUA_RESP0);
		cmd->resp[2] = qua_readl(host, QUA_RESP1);
		cmd->resp[1] = qua_readl(host, QUA_RESP2);
		cmd->resp[0] = qua_readl(host, QUA_RESP3);
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		cmd->resp[0] = qua_readl(host, QUA_RESP0);
	}
}

static int qua_rint_error(u32 status)
{
	if (status & (QUA_INT_RTO | QUA_INT_DRTO | QUA_INT_HTO))
		return -ETIMEDOUT;
	if (status & (QUA_INT_RCRC | QUA_INT_DCRC))
		return -EILSEQ;
	return -EIO;
}

static u32 qua_command_flags(struct mmc_command *cmd)
{
	u32 flags = QUA_CMD_START | QUA_CMD_USE_HOLD | cmd->opcode;

	if (cmd->flags & MMC_RSP_PRESENT)
		flags |= QUA_CMD_RESP_EXP;
	if (cmd->flags & MMC_RSP_136)
		flags |= QUA_CMD_RESP_LONG;
	if (cmd->flags & MMC_RSP_CRC)
		flags |= QUA_CMD_RESP_CRC;

	if (!cmd->data)
		return flags;

	flags |= QUA_CMD_DATA_EXP;
	if (cmd->data->flags & MMC_DATA_WRITE)
		flags |= QUA_CMD_WRITE;

	/* Preserve the Qua BSP command words for its streaming commands. */
	switch (cmd->opcode) {
	case MMC_READ_SINGLE_BLOCK:
		flags |= QUA_CMD_WAIT_PRV;
		break;
	case MMC_READ_MULTIPLE_BLOCK:
		flags |= QUA_CMD_WAIT_PRV | QUA_CMD_AUTO_STOP;
		break;
	case MMC_WRITE_MULTIPLE_BLOCK:
		flags |= QUA_CMD_AUTO_STOP;
		break;
	default:
		break;
	}

	return flags;
}

static void qua_prepare_irq_state(struct qua_rtkemmc *host)
{
	/* BSP sequence: mask wrapper sources, W1C DMA/IP status, expose IP. */
	qua_writel(host, 0x1d, QUA_WRAPPER_ISR);
	qua_writel(host, QUA_WRAPPER_DMA_DONE, QUA_WRAPPER_ISR);
	qua_writel(host, 0xffffffff, QUA_RINTSTS);
	qua_writel(host, 0x10, QUA_WRAPPER_ISR);
}

static int qua_build_descriptors(struct qua_rtkemmc *host, size_t length)
{
	unsigned int count = DIV_ROUND_UP(length, QUA_DESC_BYTES);
	unsigned int i;
	size_t remaining = length;

	if (!count || count > QUA_DESC_COUNT)
		return -EINVAL;

	memset(host->descs, 0, QUA_DESC_COUNT * sizeof(*host->descs));
	for (i = 0; i < count; i++) {
		size_t bytes = min_t(size_t, remaining, QUA_DESC_BYTES);
		u32 des0 = QUA_DES0_OWN | QUA_DES0_DIC | QUA_DES0_CH;

		if (!i)
			des0 |= QUA_DES0_FD;
		if (i == count - 1)
			des0 |= QUA_DES0_LD;

		host->descs[i].des0 = cpu_to_le32(des0);
		host->descs[i].des1 = cpu_to_le32(bytes);
		host->descs[i].des2 = cpu_to_le32(host->bounce_dma +
						       i * QUA_DESC_BYTES);
		host->descs[i].des3 = cpu_to_le32(host->desc_dma +
						       (i + 1) * sizeof(*host->descs));
		remaining -= bytes;
	}

	dma_wmb();
	return 0;
}

static int qua_wait_command(struct qua_rtkemmc *host, bool data,
			    bool multi, bool read, size_t bytes)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(3000);
	u32 rint = 0, wrapper = 0, tcb = 0, tbb = 0;
	bool dto = !data, acd = !multi, dma_done = !read;
	bool complete = false;

	do {
		rint = qua_readl(host, QUA_RINTSTS);
		if (rint & QUA_INT_ERRORS)
			return qua_rint_error(rint);
		if (rint & QUA_INT_DTO)
			dto = true;
		if (rint & QUA_INT_ACD)
			acd = true;
		if (!data && (rint & QUA_INT_CMD_DONE)) {
			complete = true;
			break;
		}

		if (data) {
			wrapper = qua_readl(host, QUA_WRAPPER_ISR);
			/* BSP wrapper DMA_DONE is a read-side completion only. */
			dma_done = !read || (wrapper & QUA_WRAPPER_DMA_DONE);
			tcb = qua_readl(host, QUA_TCBCNT);
			tbb = qua_readl(host, QUA_TBBCNT);
			if (dto && acd && dma_done && tcb == bytes && tbb == bytes) {
				complete = true;
				break;
			}
		}
		usleep_range(10, 30);
	} while (time_before(jiffies, timeout));

	if (!complete)
		return -ETIMEDOUT;
	if (qua_wait_bits(host, QUA_CMD, QUA_CMD_START, 0, 1000))
		return -ETIMEDOUT;
	if (qua_wait_bits(host, QUA_STATUS, QUA_STATUS_BUSY, 0, 3000))
		return -ETIMEDOUT;

	if (data && (!dto || !acd || !dma_done || tcb != bytes || tbb != bytes))
		return -EIO;

	return 0;
}

static int qua_issue_command(struct qua_rtkemmc *host,
			     struct mmc_command *cmd)
{
	struct mmc_data *data = cmd->data;
	size_t bytes = data ? (size_t)data->blksz * data->blocks : 0;
	bool multi = data && data->blocks > 1;
	int ret;

	cmd->error = 0;
	if (data) {
		data->error = 0;
		data->bytes_xfered = 0;
		if (!bytes || bytes > QUA_BOUNCE_SIZE || bytes % 512)
			return data->error = -EINVAL;
		if (data->flags & MMC_DATA_WRITE) {
			if (sg_copy_to_buffer(data->sg, data->sg_len,
					      host->bounce, bytes) != bytes)
				return data->error = -EFAULT;
		}
		ret = qua_build_descriptors(host, bytes);
		if (ret)
			return data->error = ret;
	}

	qua_prepare_irq_state(host);
	qua_writel(host, 0, QUA_SWC_SEL);
	qua_writel(host, 0, QUA_CP);
	if (data) {
		qua_writel(host, data->blksz, QUA_BLKSIZ);
		qua_writel(host, bytes, QUA_BYTCNT);
		qua_writel(host, host->desc_dma, QUA_DBADDR);
	}
	qua_writel(host, cmd->arg, QUA_CMDARG);
	qua_writel(host, qua_command_flags(cmd), QUA_CMD);

	ret = qua_wait_command(host, data, multi,
			       data && (data->flags & MMC_DATA_READ), bytes);
	if (ret) {
		dev_err(host->dev,
			"CMD%u arg=%#x bytes=%zu failed %d: RINTSTS=%#x wrapperISR=%#x STATUS=%#x TBBCNT=%#x TCBCNT=%#x BYTCNT=%#x\n",
			cmd->opcode, cmd->arg, bytes, ret,
			qua_readl(host, QUA_RINTSTS),
			qua_readl(host, QUA_WRAPPER_ISR),
			qua_readl(host, QUA_STATUS),
			qua_readl(host, QUA_TBBCNT),
			qua_readl(host, QUA_TCBCNT),
			qua_readl(host, QUA_BYTCNT));
		cmd->error = ret;
		if (data)
			data->error = ret;
		return ret;
	}

	qua_read_response(host, cmd);
	if (data) {
		dma_rmb();
		if (data->flags & MMC_DATA_READ) {
			if (sg_copy_from_buffer(data->sg, data->sg_len,
						host->bounce, bytes) != bytes) {
				data->error = -EFAULT;
				return data->error;
			}
		}
		data->bytes_xfered = bytes;
		qua_writel(host, QUA_WRAPPER_DMA_DONE, QUA_WRAPPER_ISR);
	}
	qua_writel(host, qua_readl(host, QUA_RINTSTS), QUA_RINTSTS);
	return 0;
}

static void qua_hw_init(struct qua_rtkemmc *host)
{
	qua_writel(host, 0, QUA_DDR_REG);
	qua_writel(host, 0, QUA_DQS_CTRL1);
	qua_writel(host, 0x00000081, QUA_BMOD);
	qua_writel(host, QUA_CTRL_USE_IDMAC, QUA_CTRL);
	qua_writel(host, 1, QUA_PWREN);
	/* Poll RINTSTS directly; no unregistered GIC IRQ may be raised. */
	qua_writel(host, 0, QUA_INTMASK);
	qua_writel(host, 0xffffffff, QUA_RINTSTS);
	qua_writel(host, 0, QUA_CLKSRC);
	qua_writel(host, 0xffffff40, QUA_TMOUT);
	qua_writel(host, 0, QUA_CTYPE);
	qua_writel(host, 0x0001007f, QUA_FIFOTH);
	qua_writel(host, QUA_CTRL_USE_IDMAC | QUA_CTRL_INT_ENABLE, QUA_CTRL);
	qua_writel(host, 0x00000080, QUA_BMOD);
	qua_writel(host, 0, QUA_IDINTEN);
	qua_writel(host, 1, QUA_UHSREG);
	qua_writel(host, 0x02000001, QUA_CARD_THR_CTL);
	qua_prepare_irq_state(host);
}

static void qua_set_ios(struct mmc_host *mmc, struct mmc_ios *ios);

static void qua_request_work(struct work_struct *work)
{
	struct qua_rtkemmc *host = container_of(work, struct qua_rtkemmc,
					       request_work);
	struct mmc_request *mrq = host->mrq;
	int ret = 0;

	if (mrq->sbc)
		ret = qua_issue_command(host, mrq->sbc);
	if (!ret)
		ret = qua_issue_command(host, mrq->cmd);

	if (mrq->stop) {
		/* CMD18/CMD25 use the BSP's hardware auto-stop sequence. */
		mrq->stop->error = ret;
	}
	if (ret) {
		dev_err(host->dev, "request CMD%u failed: %d\n",
			mrq->cmd->opcode, ret);
		qua_hw_init(host);
		qua_set_ios(host->mmc, &host->mmc->ios);
	}

	host->mrq = NULL;
	mmc_request_done(host->mmc, mrq);
}

static void qua_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct qua_rtkemmc *host = mmc_priv(mmc);

	if (WARN_ON(host->mrq)) {
		mrq->cmd->error = -EBUSY;
		mmc_request_done(mmc, mrq);
		return;
	}
	host->mrq = mrq;
	queue_work(host->wq, &host->request_work);
}

static void qua_update_clock(struct qua_rtkemmc *host, unsigned int clock)
{
	unsigned long rate = clk_get_rate(host->ciu_clk);
	u32 div;

	if (!clock || !rate) {
		qua_writel(host, 0, QUA_CLKENA);
		host->clock = 0;
		return;
	}

	div = DIV_ROUND_UP(rate, 2 * clock);
	div = clamp_t(u32, div, 1, 0xff);
	qua_writel(host, 0, QUA_CLKENA);
	qua_writel(host, QUA_CMD_START | QUA_CMD_USE_HOLD |
			 QUA_CMD_UPDATE_CLK, QUA_CMD);
	qua_wait_bits(host, QUA_CMD, QUA_CMD_START, 0, 1000);
	qua_writel(host, div, QUA_CLKDIV);
	qua_writel(host, QUA_CMD_START | QUA_CMD_USE_HOLD |
			 QUA_CMD_UPDATE_CLK, QUA_CMD);
	qua_wait_bits(host, QUA_CMD, QUA_CMD_START, 0, 1000);
	qua_writel(host, 0x00010001, QUA_CLKENA);
	qua_writel(host, QUA_CMD_START | QUA_CMD_USE_HOLD |
			 QUA_CMD_UPDATE_CLK, QUA_CMD);
	qua_wait_bits(host, QUA_CMD, QUA_CMD_START, 0, 1000);
	host->clock = rate / (2 * div);
}

static void qua_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct qua_rtkemmc *host = mmc_priv(mmc);
	u32 ctype = 0;

	if (ios->power_mode == MMC_POWER_OFF)
		qua_writel(host, 0, QUA_PWREN);
	else
		qua_writel(host, 1, QUA_PWREN);

	if (ios->bus_width == MMC_BUS_WIDTH_8)
		ctype = BIT(16);
	else if (ios->bus_width == MMC_BUS_WIDTH_4)
		ctype = BIT(0);
	qua_writel(host, ctype, QUA_CTYPE);

	if (ios->clock != host->clock)
		qua_update_clock(host, ios->clock);
}

static int qua_get_cd(struct mmc_host *mmc)
{
	return 1;
}

static int qua_get_ro(struct mmc_host *mmc)
{
	return 0;
}

static int qua_card_busy(struct mmc_host *mmc)
{
	struct qua_rtkemmc *host = mmc_priv(mmc);

	return !!(qua_readl(host, QUA_STATUS) & QUA_STATUS_BUSY);
}

static const struct mmc_host_ops qua_mmc_ops = {
	.request = qua_request,
	.set_ios = qua_set_ios,
	.get_cd = qua_get_cd,
	.get_ro = qua_get_ro,
	.card_busy = qua_card_busy,
};

static int qua_rtkemmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qua_rtkemmc *host;
	struct mmc_host *mmc;
	struct resource *res;
	int ret;

	mmc = mmc_alloc_host(sizeof(*host), dev);
	if (!mmc)
		return -ENOMEM;
	host = mmc_priv(mmc);
	host->dev = dev;
	host->mmc = mmc;

	host->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(host->regs)) {
		ret = PTR_ERR(host->regs);
		goto free_host;
	}
	/* CRT, SB2 and MISC are shared with clock/reset/GPIO providers. */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	host->crt = res ? devm_ioremap(dev, res->start, resource_size(res)) : NULL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	host->sb2 = res ? devm_ioremap(dev, res->start, resource_size(res)) : NULL;
	res = platform_get_resource(pdev, IORESOURCE_MEM, 3);
	host->misc = res ? devm_ioremap(dev, res->start, resource_size(res)) : NULL;
	if (!host->crt || !host->sb2 || !host->misc) {
		ret = -ENOMEM;
		goto free_host;
	}

	host->biu_clk = devm_clk_get(dev, "biu");
	host->ciu_clk = devm_clk_get(dev, "ciu");
	if (IS_ERR(host->biu_clk)) {
		ret = PTR_ERR(host->biu_clk);
		goto free_host;
	}
	if (IS_ERR(host->ciu_clk)) {
		ret = PTR_ERR(host->ciu_clk);
		goto free_host;
	}
	ret = clk_prepare_enable(host->biu_clk);
	if (ret)
		goto free_host;
	ret = clk_prepare_enable(host->ciu_clk);
	if (ret)
		goto disable_biu;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(30));
	if (ret)
		goto disable_ciu;
	host->bounce = dma_alloc_coherent(dev, QUA_BOUNCE_SIZE,
					  &host->bounce_dma, GFP_KERNEL);
	host->descs = dma_alloc_coherent(dev,
					 QUA_DESC_COUNT * sizeof(*host->descs),
					 &host->desc_dma, GFP_KERNEL);
	if (!host->bounce || !host->descs || upper_32_bits(host->bounce_dma) ||
	    upper_32_bits(host->desc_dma)) {
		ret = -ENOMEM;
		goto free_dma;
	}

	host->wq = alloc_ordered_workqueue("qua-rtkemmc", WQ_MEM_RECLAIM);
	if (!host->wq) {
		ret = -ENOMEM;
		goto free_dma;
	}
	INIT_WORK(&host->request_work, qua_request_work);

	mmc->ops = &qua_mmc_ops;
	mmc->f_min = 300000;
	mmc->f_max = min_t(unsigned long, 25000000,
			   clk_get_rate(host->ciu_clk) / 2);
	mmc->ocr_avail = MMC_VDD_30_31 | MMC_VDD_31_32 |
			 MMC_VDD_32_33 | MMC_VDD_33_34 | MMC_VDD_165_195;
	mmc->caps = MMC_CAP_NONREMOVABLE | MMC_CAP_4_BIT_DATA |
		    MMC_CAP_8_BIT_DATA;
	mmc->max_segs = 128;
	mmc->max_blk_size = 512;
	mmc->max_blk_count = QUA_BOUNCE_SIZE / 512;
	mmc->max_seg_size = QUA_BOUNCE_SIZE;
	mmc->max_req_size = QUA_BOUNCE_SIZE;
	ret = mmc_of_parse(mmc);
	if (ret)
		goto destroy_wq;
	mmc->f_max = min(mmc->f_max, 25000000U);

	qua_hw_init(host);
	platform_set_drvdata(pdev, mmc);
	ret = mmc_add_host(mmc);
	if (ret)
		goto destroy_wq;

	dev_info(dev, "RTD1295 native eMMC host, 512 KiB coherent DMA bounce\n");
	return 0;

destroy_wq:
	destroy_workqueue(host->wq);
free_dma:
	if (host->descs)
		dma_free_coherent(dev, QUA_DESC_COUNT * sizeof(*host->descs),
				  host->descs, host->desc_dma);
	if (host->bounce)
		dma_free_coherent(dev, QUA_BOUNCE_SIZE, host->bounce,
				  host->bounce_dma);
disable_ciu:
	clk_disable_unprepare(host->ciu_clk);
disable_biu:
	clk_disable_unprepare(host->biu_clk);
free_host:
	mmc_free_host(mmc);
	return ret;
}

static void qua_rtkemmc_remove(struct platform_device *pdev)
{
	struct mmc_host *mmc = platform_get_drvdata(pdev);
	struct qua_rtkemmc *host = mmc_priv(mmc);

	mmc_remove_host(mmc);
	destroy_workqueue(host->wq);
	dma_free_coherent(host->dev, QUA_DESC_COUNT * sizeof(*host->descs),
			  host->descs, host->desc_dma);
	dma_free_coherent(host->dev, QUA_BOUNCE_SIZE, host->bounce,
			  host->bounce_dma);
	clk_disable_unprepare(host->ciu_clk);
	clk_disable_unprepare(host->biu_clk);
	mmc_free_host(mmc);
}

static const struct of_device_id qua_rtkemmc_of_match[] = {
	{ .compatible = "kddi,quastation-rtkemmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, qua_rtkemmc_of_match);

#ifdef QUA_LIVE_DRIVER
#define QUA_DRIVER_NAME "qua-rtkemmc-live"
#else
#define QUA_DRIVER_NAME "qua-rtkemmc"
#endif

static struct platform_driver qua_rtkemmc_driver = {
	.probe = qua_rtkemmc_probe,
	.remove = qua_rtkemmc_remove,
	.driver = {
		.name = QUA_DRIVER_NAME,
		.of_match_table = qua_rtkemmc_of_match,
	},
};
module_platform_driver(qua_rtkemmc_driver);

MODULE_DESCRIPTION("KDDI Qua Station RTD1295 native eMMC host");
MODULE_LICENSE("GPL");
