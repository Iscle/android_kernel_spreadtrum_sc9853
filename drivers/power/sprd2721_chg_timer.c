/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/platform_device.h>
#include <linux/types.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/hrtimer.h>
#include <linux/irq.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/regmap.h>
#include "sprd_battery.h"
#include "sprd_charge_helper.h"

#define TIMER_LOAD_LOW		(0x0004)
#define TIMER_LOAD_HIGH		 (0x0008)
#define TIMER_CTL			(0x000c)
#define TIMER_INT				(0x0010)
#define TIMER_SHDW_LOW				0x14
#define TIMER_SHDW_HIGH				0x18

#define CHG_PERIOD_MODE			BIT(0)
#define CHG_TIMER_ENABLE		BIT(1)
#define CHG_TIMER_INT_EN			BIT(0)
#define CHG_TIMER_INT_STS		BIT(2)
#define CHG_TIMER_INT_CLR		BIT(3)

static uint32_t irq_chg_timer;
static unsigned long base_addr;
static struct regmap *reg_map;
static irq_handler_t sprdchg_tm_cb;

#define SPRD2721_T_DEBUG(format, arg...) pr_info("2721 time: " format, ## arg)

static int chgtimer_adi_read(unsigned long reg)
{
	unsigned int val;

	regmap_read(reg_map, base_addr + reg, &val);
	return val;
}

static int chgtimer_adi_write(unsigned long reg, unsigned int or_val,
				 unsigned int clear_msk)
{
	return regmap_update_bits(reg_map, base_addr + reg,
		clear_msk, or_val);
}

static void sprdchg_timer_dump_regs(void)
{
	u32 timer_load_low;
	u32 timer_load_high;
	u32 timer_load_ctl;
	u32 timer_load_int;
	u32 timer_load_shdw_low;
	u32 timer_load_shdw_high;

	timer_load_low = chgtimer_adi_read(TIMER_LOAD_LOW);
	timer_load_high = chgtimer_adi_read(TIMER_LOAD_HIGH);
	timer_load_ctl = chgtimer_adi_read(TIMER_CTL);
	timer_load_int = chgtimer_adi_read(TIMER_INT);
	timer_load_shdw_low = chgtimer_adi_read(TIMER_SHDW_LOW);
	timer_load_shdw_high = chgtimer_adi_read(TIMER_SHDW_HIGH);
	SPRD2721_T_DEBUG("timer_load_low=%x,timer_load_high=%x\n",
		timer_load_low, timer_load_high);
	SPRD2721_T_DEBUG("timer_load_ctl=%x,timer_load_int=%x\n",
		timer_load_ctl, timer_load_int);
	SPRD2721_T_DEBUG("timer_load_shdw_low=%x,timer_load_shdw_high=%x\n",
		timer_load_shdw_low, timer_load_shdw_high);
}

static void sprdchg_timer_enable(uint32_t cycles)
{
	unsigned int value = 32768U * cycles;

	chgtimer_adi_write(TIMER_CTL, CHG_PERIOD_MODE,
		CHG_PERIOD_MODE);
	chgtimer_adi_write(TIMER_LOAD_LOW,
		(uint16_t)(value & 0xffff), 0xffff);
	chgtimer_adi_write(TIMER_LOAD_HIGH,
		(uint16_t)(value >> 16), 0xffff);
	chgtimer_adi_write(TIMER_CTL,
		CHG_TIMER_ENABLE,  CHG_TIMER_ENABLE);
	chgtimer_adi_write(TIMER_INT,
		CHG_TIMER_INT_EN, CHG_TIMER_INT_EN);
}

static void sprdchg_timer_disable(void)
{
	chgtimer_adi_write(TIMER_CTL, 0, CHG_TIMER_ENABLE);
}

static irqreturn_t  sprdchg_timer_interrupt(int irq, void *dev_id)
{
	chgtimer_adi_write(TIMER_INT,
		CHG_TIMER_INT_CLR, CHG_TIMER_INT_CLR);
	sprdchg_timer_dump_regs();
	return sprdchg_tm_cb(irq, dev_id);
}

static int sprdchg_timer_request(irq_handler_t fn_cb, void *data)
{
	int ret = 0;

	glb_adi_write(ANA_REG_GLB_MODULE_EN0,
		BIT_TMR_EN, BIT_TMR_EN);
	glb_adi_write(ANA_REG_GLB_RTC_CLK_EN,
		BIT_RTC_TMR_EN, BIT_RTC_TMR_EN);

	sprdchg_timer_disable();
	sprdchg_tm_cb = fn_cb;
	ret = request_threaded_irq(irq_chg_timer,
		NULL, sprdchg_timer_interrupt,
		IRQF_NO_SUSPEND | __IRQF_TIMER | IRQF_ONESHOT,
		"chg_timer", data);

	if (ret)
		pr_err("request charge timer failed\n");

	return ret;
}

static struct sprd_chg_timer_operations sprd_chg_timer_ops = {
	.timer_enable = sprdchg_timer_enable,
	.timer_disable = sprdchg_timer_disable,
	.timer_request = sprdchg_timer_request,
};

static int sprdchg_timer_2721_probe(struct platform_device *pdev)
{
	int ret;
	u32 value = 0;
	struct device_node *np = pdev->dev.of_node;

	reg_map = dev_get_regmap(pdev->dev.parent, NULL);
	if (!reg_map)
		dev_err(&pdev->dev, "%s :NULL regmap for chgtimer 2721\n",
			__func__);

	irq_chg_timer = irq_of_parse_and_map(np, 0);
	if (irq_chg_timer <= 0)
		pr_err("%s can't parse timer irq\n", __func__);

	ret = of_property_read_u32(np, "reg", &value);
	if (ret)
		dev_err(&pdev->dev, "%s :no reg of property for pmic fgu\n",
			__func__);
	base_addr = (unsigned long)value;

	sprdbat_register_timer_ops(&sprd_chg_timer_ops);

	pr_info("2721 charge timer probe ok\n");
	return 0;
}

static int sprdchg_timer_2721_remove(struct platform_device *pdev)
{
	sprdbat_unregister_timer_ops();
	return 0;
}

static const struct of_device_id sprdchg_timer_2721_of_match[] = {
	{.compatible = "sprd,sc2721-chg-timer",},
	{.compatible = "sprd,sc2720-chg-timer",},
	{}
};

static struct platform_driver sprdchg_timer_2721_driver = {
	.driver = {
		   .name = "chg-timer",
		   .of_match_table = of_match_ptr(sprdchg_timer_2721_of_match),
		   },
	.probe = sprdchg_timer_2721_probe,
	.remove = sprdchg_timer_2721_remove
};

static int __init sprdchg_timer_2721_driver_init(void)
{
	return platform_driver_register(&sprdchg_timer_2721_driver);
}

static void __exit sprdchg_timer_2721_driver_exit(void)
{
	platform_driver_unregister(&sprdchg_timer_2721_driver);
}

subsys_initcall_sync(sprdchg_timer_2721_driver_init);
module_exit(sprdchg_timer_2721_driver_exit);

