// SPDX-License-Identifier: GPL-2.0-or-later
/* Diagnostic limit for the unbound Qua GPU platform device. */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/module.h>
#include <linux/platform_device.h>

static struct device *gpu;
static u64 previous_limit;

static int __init qua_gpu_dma_limit_init(void)
{
	gpu = bus_find_device_by_name(&platform_bus_type, NULL, "98050000.gpu");
	if (!gpu)
		return -ENODEV;
	if (gpu->driver) {
		put_device(gpu);
		return -EBUSY;
	}
	previous_limit = gpu->bus_dma_limit;
	gpu->bus_dma_limit = DMA_BIT_MASK(30);
	pr_info("qua-gpu-dma-limit: old=%#llx new=%#llx\n", previous_limit, gpu->bus_dma_limit);
	return 0;
}

static void __exit qua_gpu_dma_limit_exit(void)
{
	gpu->bus_dma_limit = previous_limit;
	put_device(gpu);
}
module_init(qua_gpu_dma_limit_init);
module_exit(qua_gpu_dma_limit_exit);
MODULE_LICENSE("GPL");
