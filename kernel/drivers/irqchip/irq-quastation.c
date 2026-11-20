// SPDX-License-Identifier: GPL-2.0-or-later
/* RTD1295 interrupt mux. Register routing table from Realtek's 2017 BSP. */
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/slab.h>
#include "irq-rtd129x.h"
struct qua_mux;
struct qua_bank { struct qua_mux *mux; void __iomem *base; u32 status, enable; unsigned int index, parent; };
struct qua_mux { struct irq_domain *domain; raw_spinlock_t lock; struct qua_bank bank[2]; };
static void qua_gate(struct irq_data *d, bool on)
{
 struct qua_mux *m = irq_data_get_irq_chip_data(d);
 struct qua_bank *b = &m->bank[d->hwirq / 32];
 unsigned int bit = irq_map_tab[b->index][d->hwirq % 32];
 unsigned long flags; u32 v;
 if (bit >= 32) return;
 raw_spin_lock_irqsave(&m->lock, flags);
 v = readl(b->base + b->enable);
 writel(on ? v | BIT(bit) : v & ~BIT(bit), b->base + b->enable);
 raw_spin_unlock_irqrestore(&m->lock, flags);
}
static void qua_mask(struct irq_data *d) { qua_gate(d, false); }
static void qua_unmask(struct irq_data *d) { qua_gate(d, true); }
static void qua_ack(struct irq_data *d)
{
 struct qua_mux *m = irq_data_get_irq_chip_data(d);
 struct qua_bank *b = &m->bank[d->hwirq / 32];
 writel(BIT(d->hwirq % 32), b->base + b->status);
}
static struct irq_chip qua_chip = { .name = "rtd1295-mux", .irq_mask = qua_mask, .irq_unmask = qua_unmask, .irq_ack = qua_ack };
static int qua_map(struct irq_domain *d, unsigned int irq, irq_hw_number_t hw)
{
 irq_set_chip_data(irq, d->host_data);
 irq_set_chip_and_handler(irq, &qua_chip, handle_level_irq);
 irq_set_noprobe(irq);
 return 0;
}
static int qua_xlate(struct irq_domain *d, struct device_node *np, const u32 *spec, unsigned int n, unsigned long *hw, unsigned int *type)
{
 if (n != 2 || spec[0] > 1 || spec[1] > 31) return -EINVAL;
 *hw = spec[0] * 32 + spec[1]; *type = IRQ_TYPE_LEVEL_HIGH;
 return 0;
}
static const struct irq_domain_ops qua_ops = { .map = qua_map, .xlate = qua_xlate };
static void qua_handle(struct irq_desc *desc)
{
 struct qua_bank *b = irq_desc_get_handler_data(desc);
 struct irq_chip *chip = irq_desc_get_chip(desc);
 u32 status, enabled; unsigned int i, bit;
 chained_irq_enter(chip, desc);
 status = readl(b->base + b->status); enabled = readl(b->base + b->enable);
 for (i = 0; i < 32; i++) {
  if (!(status & BIT(i))) continue;
  bit = irq_map_tab[b->index][i];
  if (bit == MISC_INT_FAIL || (bit < 32 && !(enabled & BIT(bit)))) continue;
  if (generic_handle_domain_irq(b->mux->domain, b->index * 32 + i))
   writel(BIT(i), b->base + b->status);
 }
 chained_irq_exit(chip, desc);
}
static int __init qua_init(struct device_node *np, struct device_node *parent)
{
 struct qua_mux *m = kzalloc(sizeof(*m), GFP_KERNEL); unsigned int i;
 if (!m) return -ENOMEM;
 raw_spin_lock_init(&m->lock);
 for (i = 0; i < 2; i++) {
  m->bank[i] = (struct qua_bank){ .mux=m, .index=i, .status=i ? 0 : 0xc, .enable=i ? 0x40 : 0x80 };
  m->bank[i].base = of_iomap(np, i);
  m->bank[i].parent = irq_of_parse_and_map(np, i);
  if (!m->bank[i].base || !m->bank[i].parent) goto fail;
 }
 m->domain = irq_domain_create_linear(of_fwnode_handle(np), 64, &qua_ops, m);
 if (!m->domain) goto fail;
 for (i = 0; i < 2; i++) irq_set_chained_handler_and_data(m->bank[i].parent, qua_handle, &m->bank[i]);
 return 0;
fail:
 for (i = 0; i < 2; i++) {
  if (m->bank[i].base) iounmap(m->bank[i].base);
  if (m->bank[i].parent) irq_dispose_mapping(m->bank[i].parent);
 }
 kfree(m); return -ENODEV;
}
IRQCHIP_DECLARE(quastation_mux, "kddi,quastation-irq-mux", qua_init);
