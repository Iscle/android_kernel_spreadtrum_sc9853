/*
 * Copyright (C) 2012--2016 Spreadtrum Communications Inc.
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
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sprd_iommu.h>
#include <linux/sprd_ion.h>
#include <linux/uaccess.h>
#include "vsp_common.h"

struct regmap *gpr_aon_apb;
struct regmap *gpr_mm_ahb;
struct regmap *gpr_pmu_apb;
struct regmap *gpr_com_pmu_apb;
unsigned int codec_instance_count[VSP_CODEC_INSTANCE_COUNT_MAX];

struct clk *vsp_get_clk_src_name(struct clock_name_map_t clock_name_map[],
				unsigned int freq_level,
				unsigned int max_freq_level)
{
	if (freq_level >= max_freq_level) {
		pr_info("set freq_level to 0\n");
		freq_level = 0;
	}

	pr_debug(" freq_level %d %s\n", freq_level,
		 clock_name_map[freq_level].name);
	return clock_name_map[freq_level].clk_parent;
}

int find_vsp_freq_level(struct clock_name_map_t clock_name_map[],
			unsigned long freq,
			unsigned int max_freq_level)
{
	int level = 0;
	int i;

	for (i = 0; i < max_freq_level; i++) {
		if (clock_name_map[i].freq == freq) {
			level = i;
			break;
		}
	}
	return level;
}

#ifdef CONFIG_COMPAT
static int compat_get_mmu_map_data(struct compat_vsp_iommu_map_data __user *
				   data32,
				   struct vsp_iommu_map_data __user *data)
{
	compat_int_t i;
	compat_size_t s;
	compat_ulong_t ul;
	int err;

	err = get_user(i, &data32->fd);
	err |= put_user(i, &data->fd);
	err |= get_user(s, &data32->size);
	err |= put_user(s, &data->size);
	err |= get_user(ul, &data32->iova_addr);
	err |= put_user(ul, &data->iova_addr);

	return err;
};

static int compat_put_mmu_map_data(struct compat_vsp_iommu_map_data __user *
				   data32,
				   struct vsp_iommu_map_data __user *data)
{
	compat_int_t i;
	compat_size_t s;
	compat_ulong_t ul;
	int err;

	err = get_user(i, &data->fd);
	err |= put_user(i, &data32->fd);
	err |= get_user(s, &data->size);
	err |= put_user(s, &data32->size);
	err |= get_user(ul, &data->iova_addr);
	err |= put_user(ul, &data32->iova_addr);

	return err;
};

long compat_vsp_ioctl(struct file *filp, unsigned int cmd,
			     unsigned long arg)
{
	long ret = 0;
	int err;
	struct compat_vsp_iommu_map_data __user *data32;
	struct vsp_iommu_map_data __user *data;

	struct vsp_fh *vsp_fp = filp->private_data;

	if (!filp->f_op->unlocked_ioctl)
		return -ENOTTY;

	if (vsp_fp == NULL) {
		pr_err("%s, vsp_ioctl error occurred, vsp_fp == NULL\n",
		       __func__);
		return -EINVAL;
	}

	switch (cmd) {
	case COMPAT_VSP_GET_IOVA:

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL) {
			pr_err("%s %d, compat_alloc_user_space failed",
				__func__, __LINE__);
			return -EFAULT;
		}

		err = compat_get_mmu_map_data(data32, data);
		if (err) {
			pr_err("%s %d, compat_get_mmu_map_data failed",
				__func__, __LINE__);
			return err;
		}
		ret = filp->f_op->unlocked_ioctl(filp, VSP_GET_IOVA,
						(unsigned long)data);
		err = compat_put_mmu_map_data(data32, data);
			return ret ? ret : err;

	case COMPAT_VSP_FREE_IOVA:

		data32 = compat_ptr(arg);
		data = compat_alloc_user_space(sizeof(*data));
		if (data == NULL) {
			pr_err("%s %d, compat_alloc_user_space failed",
				__func__, __LINE__);
			return -EFAULT;
		}

		err = compat_get_mmu_map_data(data32, data);
		if (err) {
			pr_err("%s %d, compat_get_mmu_map_data failed",
				__func__, __LINE__);
			return err;
		}
		ret = filp->f_op->unlocked_ioctl(filp, VSP_FREE_IOVA,
						(unsigned long)data);
		err = compat_put_mmu_map_data(data32, data);
		return ret ? ret : err;

	default:
		return filp->f_op->unlocked_ioctl(filp, cmd, (unsigned long)
						  compat_ptr(arg));
	}

	return ret;
}
#endif

int vsp_get_iova(struct vsp_dev_t *vsp_hw_dev,
		 struct vsp_iommu_map_data *mapdata, void __user *arg)
{
	int ret = 0;
	struct sprd_iommu_map_data iommu_map_data;

	vsp_clk_enable(vsp_hw_dev);
	sprd_iommu_resume(vsp_hw_dev->vsp_dev);
	ret = sprd_ion_get_buffer(mapdata->fd, NULL,
					&(iommu_map_data.buf),
					&iommu_map_data.iova_size);
	if (ret) {
		pr_err("get_sg_table failed, ret %d\n", ret);
		sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
		vsp_clk_disable(vsp_hw_dev);
		return ret;
	}

	iommu_map_data.ch_type = SPRD_IOMMU_FM_CH_RW;
	ret = sprd_iommu_map(vsp_hw_dev->vsp_dev, &iommu_map_data);
	if (!ret) {
		mapdata->iova_addr = iommu_map_data.iova_addr;
		mapdata->size = iommu_map_data.iova_size;
		pr_debug("vsp iommu map success iova addr 0x%lx size 0x%zx\n",
			mapdata->iova_addr, mapdata->size);
		ret = copy_to_user((void __user *)arg, (void *)mapdata,
					sizeof(struct vsp_iommu_map_data));
		if (ret) {
			pr_err("copy_to_user failed, ret %d\n", ret);
			sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
			vsp_clk_disable(vsp_hw_dev);
			return ret;
		}
	} else
		pr_err("vsp iommu map failed, ret %d, map size 0x%zx\n",
			ret, iommu_map_data.iova_size);
	sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
	vsp_clk_disable(vsp_hw_dev);
	return ret;
}
int vsp_free_iova(struct vsp_dev_t *vsp_hw_dev,
		  struct vsp_iommu_map_data *ummapdata)
{
	int ret = 0;
	struct sprd_iommu_unmap_data iommu_ummap_data;

	vsp_clk_enable(vsp_hw_dev);
	sprd_iommu_resume(vsp_hw_dev->vsp_dev);
	iommu_ummap_data.iova_addr = ummapdata->iova_addr;
	iommu_ummap_data.iova_size = ummapdata->size;
	iommu_ummap_data.ch_type = SPRD_IOMMU_FM_CH_RW;
	iommu_ummap_data.buf = NULL;

	ret = sprd_iommu_unmap(vsp_hw_dev->vsp_dev,
					&iommu_ummap_data);

	if (ret)
		pr_err("vsp iommu-unmap failed ret %d addr&size 0x%lx 0x%zx\n",
			ret, ummapdata->iova_addr, ummapdata->size);
	else
		pr_debug("vsp iommu-unmap success iova addr 0x%lx size 0x%zx\n",
			ummapdata->iova_addr, ummapdata->size);
	sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
	vsp_clk_disable(vsp_hw_dev);

	return ret;
}

int vsp_get_mm_clk(struct vsp_dev_t *vsp_hw_dev)
{
	int ret = 0;
	struct clk *clk_mm_eb;
	struct clk *clk_vsp_mq_ahb_eb;
	struct clk *clk_vsp_ahb_mmu_eb;
	struct clk *clk_vsp_ckg;
	struct clk *clk_axi_gate_vsp;
	struct clk *clk_ahb_gate_vsp_eb;
	struct clk *clk_vsp;
	struct clk *clk_ahb_vsp;
	struct clk *clk_emc_vsp;
	struct clk *clk_parent;

	if (vsp_hw_dev->version != SHARKLE) {
		clk_mm_eb = of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_mm_eb");

		if (IS_ERR_OR_NULL(clk_mm_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				"clk_mm_eb", clk_mm_eb);
			vsp_hw_dev->clk_mm_eb = NULL;
			ret = -EINVAL;
			goto errout0;
		} else
			vsp_hw_dev->clk_mm_eb = clk_mm_eb;

		clk_axi_gate_vsp = of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_axi_gate_vsp");
		if (IS_ERR_OR_NULL(clk_axi_gate_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n", "clk_axi_gate_vsp",
			clk_axi_gate_vsp);
			vsp_hw_dev->clk_axi_gate_vsp = NULL;
			vsp_hw_dev->light_sleep_en = false;
			goto errout0;
		} else {
			vsp_hw_dev->clk_axi_gate_vsp = clk_axi_gate_vsp;
			vsp_hw_dev->light_sleep_en = false;
		}
	}

	if (vsp_hw_dev->version == IWHALE2 || vsp_hw_dev->version == ISHARKL2)
		vsp_hw_dev->clk_axi_gate_vsp = NULL;

	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version) {
		clk_vsp_ckg =
		    of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_vsp_ckg");

		if (IS_ERR_OR_NULL(clk_vsp_ckg)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_vsp_ckg", clk_vsp_ckg);
			ret = -EINVAL;
			vsp_hw_dev->clk_vsp_ckg = NULL;
			goto errout1;
		} else
			vsp_hw_dev->clk_vsp_ckg = clk_vsp_ckg;
	}

	clk_ahb_gate_vsp_eb =
	    of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_ahb_gate_vsp_eb");

	if (IS_ERR_OR_NULL(clk_ahb_gate_vsp_eb)) {
		pr_err("Failed: Can't get clock [%s]! %p\n",
		       "clk_ahb_gate_vsp_eb", clk_ahb_gate_vsp_eb);
		ret = -EINVAL;
		vsp_hw_dev->clk_ahb_gate_vsp_eb = NULL;
		goto errout2;
	} else
		vsp_hw_dev->clk_ahb_gate_vsp_eb = clk_ahb_gate_vsp_eb;

	clk_vsp = of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_vsp");

	if (IS_ERR_OR_NULL(clk_vsp)) {
		pr_err("Failed: Can't get clock [%s]! %p\n", "clk_vsp",
		       clk_vsp);
		ret = -EINVAL;
		vsp_hw_dev->vsp_clk = NULL;
		goto errout3;
	} else
		vsp_hw_dev->vsp_clk = clk_vsp;

	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version) {
		clk_ahb_vsp =
		    of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_ahb_vsp");

		if (IS_ERR_OR_NULL(clk_ahb_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_ahb_vsp", clk_ahb_vsp);
			ret = -EINVAL;
			goto errout4;
		} else
			vsp_hw_dev->clk_ahb_vsp = clk_ahb_vsp;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_twpll_153m6");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_twpll_153m6");
			ret = -EINVAL;
			goto errout5;
		} else
			vsp_hw_dev->ahb_parent_clk = clk_parent;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev, "clk_twpll_96m");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_twpll_96m");
			ret = -EINVAL;
			goto errout5;
		} else
			vsp_hw_dev->ahb_parent_df_clk = clk_parent;
	} else if (vsp_hw_dev->version == SHARKL3) {
		clk_ahb_vsp =
		    of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_ahb_vsp");

		if (IS_ERR_OR_NULL(clk_ahb_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_ahb_vsp", clk_ahb_vsp);
			ret = -EINVAL;
			goto errout4;
		} else
			vsp_hw_dev->clk_ahb_vsp = clk_ahb_vsp;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_ahb_vsp_parent");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_ahb_vsp_parent");
			ret = -EINVAL;
			goto errout5;
		} else
			vsp_hw_dev->ahb_parent_clk = clk_parent;

		clk_emc_vsp =
			of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_emc_vsp");

		if (IS_ERR_OR_NULL(clk_emc_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				   "clk_emc_vsp", clk_emc_vsp);
			ret = -EINVAL;
			goto errout5;
		} else
			vsp_hw_dev->clk_emc_vsp = clk_emc_vsp;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_emc_vsp_parent");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_emc_vsp_parent");
			ret = -EINVAL;
			goto errout6;
		} else
			vsp_hw_dev->emc_parent_clk = clk_parent;
	}

	if (vsp_hw_dev->version == IWHALE2 || vsp_hw_dev->version == SHARKL3) {
		clk_vsp_ahb_mmu_eb =
			of_clk_get_by_name(vsp_hw_dev->dev_np,
				"clk_vsp_ahb_mmu_eb");

		if (IS_ERR_OR_NULL(clk_vsp_ahb_mmu_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_vsp_ahb_mmu_eb", clk_vsp_ahb_mmu_eb);
			ret = -EINVAL;
			vsp_hw_dev->clk_vsp_ahb_mmu_eb = NULL;
			goto errout6;
		} else
			vsp_hw_dev->clk_vsp_ahb_mmu_eb = clk_vsp_ahb_mmu_eb;
	}

	if (vsp_hw_dev->version == PIKE2) {
		clk_vsp_mq_ahb_eb =
		    of_clk_get_by_name(vsp_hw_dev->dev_np, "clk_vsp_mq_ahb_eb");

		if (IS_ERR_OR_NULL(clk_vsp_mq_ahb_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_vsp_mq_ahb_eb", clk_vsp_mq_ahb_eb);
			ret = -EINVAL;
			vsp_hw_dev->clk_vsp_mq_ahb_eb = NULL;
			goto errout7;
		} else
			vsp_hw_dev->clk_vsp_mq_ahb_eb = clk_vsp_mq_ahb_eb;
	}

	return 0;

errout7:
	if (vsp_hw_dev->clk_vsp_mq_ahb_eb)
		clk_put(vsp_hw_dev->clk_vsp_mq_ahb_eb);

errout6:
	if (vsp_hw_dev->clk_emc_vsp)
		clk_put(vsp_hw_dev->clk_emc_vsp);

errout5:
	if (vsp_hw_dev->clk_ahb_vsp)
		clk_put(vsp_hw_dev->clk_ahb_vsp);

errout4:
	if (vsp_hw_dev->vsp_clk)
		clk_put(vsp_hw_dev->vsp_clk);

errout3:
	if (clk_ahb_gate_vsp_eb)
		clk_put(vsp_hw_dev->clk_ahb_gate_vsp_eb);

errout2:
	if (vsp_hw_dev->clk_vsp_ckg)
		clk_put(vsp_hw_dev->clk_vsp_ckg);

errout1:
	if (vsp_hw_dev->version != SHARKLE) {
		if (vsp_hw_dev->clk_axi_gate_vsp)
			clk_put(vsp_hw_dev->clk_axi_gate_vsp);
	}

errout0:
	if (vsp_hw_dev->version != SHARKLE) {
		if (vsp_hw_dev->clk_mm_eb)
			clk_put(vsp_hw_dev->clk_mm_eb);
	}


	return ret;
}

int vsp_clk_enable(struct vsp_dev_t *vsp_hw_dev)
{
	int ret = 0;

	if (vsp_hw_dev->version != SHARKLE) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_mm_eb);
		if (ret) {
			pr_err("vsp clk_mm_eb: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("vsp clk_mm_eb: clk_prepare_enable ok.\n");
	}

	ret = clk_prepare_enable(vsp_hw_dev->clk_ahb_gate_vsp_eb);
	if (ret) {
		pr_err("clk_ahb_gate_vsp_eb: clk_prepare_enable failed!\n");
		return ret;
	}
	pr_debug("clk_ahb_gate_vsp_eb: clk_prepare_enable ok.\n");

	if (vsp_hw_dev->clk_vsp_mq_ahb_eb) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_vsp_mq_ahb_eb);
		if (ret) {
			pr_err("clk_vsp_mq_ahb_eb: clk_enable failed!\n");
			return ret;
		}
		pr_debug("clk_vsp_mq_ahb_eb: clk_prepare_enable ok.\n");
	}

	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version
		|| vsp_hw_dev->version == SHARKL3) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_vsp_ckg);
		if (ret) {
			pr_err("clk_vsp_ckg: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_vsp_ckg: clk_prepare_enable ok.\n");
	}

	if (vsp_hw_dev->version == IWHALE2 || SHARKL3 == vsp_hw_dev->version) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_vsp_ahb_mmu_eb);
		if (ret) {
			pr_err("clk_vsp_ahb_mmu_eb: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_vsp_ahb_mmu_eb: clk_prepare_enable ok.\n");
	}
	if (vsp_hw_dev->version != SHARKLE) {
		if (vsp_hw_dev->clk_axi_gate_vsp) {
			ret = clk_prepare_enable(vsp_hw_dev->clk_axi_gate_vsp);
			if (ret) {
				pr_err("clk_axi_gate_vsp: clk_prepare_enable failed!\n");
				return ret;
			}
			pr_debug("clk_axi_gate_vsp: clk_prepare_enable ok.\n");
		}
	}

	ret = clk_set_parent(vsp_hw_dev->vsp_clk,
		vsp_hw_dev->vsp_parent_df_clk);
	if (ret) {
		pr_err("clock[%s]: clk_set_parent default clock failed!",
			"vsp_parent_df_clk");
		return -EINVAL;
	}

	ret = clk_set_parent(vsp_hw_dev->vsp_clk, vsp_hw_dev->vsp_parent_clk);
	if (ret) {
		pr_err("clock[%s]: clk_set_parent() failed!", "clk_vsp");
		return -EINVAL;
	}

	ret = clk_prepare_enable(vsp_hw_dev->vsp_clk);
	if (ret) {
		pr_err("vsp_clk: clk_prepare_enable failed!\n");
		return ret;
	}
	pr_debug("vsp_clk: clk_prepare_enable ok.\n");

	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version) {
		ret = clk_set_parent(vsp_hw_dev->clk_ahb_vsp,
				   vsp_hw_dev->ahb_parent_df_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent default clock failed!",
			       "ahb_parent_df_clk");
			return -EINVAL;
		}

		ret = clk_set_parent(vsp_hw_dev->clk_ahb_vsp,
				   vsp_hw_dev->ahb_parent_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent() failed!",
			       "ahb_parent_clk");
			return -EINVAL;
		}

		ret = clk_prepare_enable(vsp_hw_dev->clk_ahb_vsp);
		if (ret) {
			pr_err("clk_ahb_vsp: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_ahb_vsp: clk_prepare_enable ok.\n");
	} else if (vsp_hw_dev->version == SHARKL3) {
		ret = clk_set_parent(vsp_hw_dev->clk_ahb_vsp,
				   vsp_hw_dev->ahb_parent_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent() failed!",
			       "ahb_parent_clk");
			return -EINVAL;
		}

		ret = clk_prepare_enable(vsp_hw_dev->clk_ahb_vsp);
		if (ret) {
			pr_err("clk_ahb_vsp: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_ahb_vsp: clk_prepare_enable ok.\n");

		ret = clk_set_parent(vsp_hw_dev->clk_emc_vsp,
				   vsp_hw_dev->emc_parent_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent() failed!",
				   "emc_parent_clk");
			return -EINVAL;
		}

		ret = clk_prepare_enable(vsp_hw_dev->clk_emc_vsp);
		if (ret) {
			pr_err("clk_emc_vsp: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_emc_vsp: clk_prepare_enable ok.\n");
	}

	return ret;
}

void vsp_clk_disable(struct vsp_dev_t *vsp_hw_dev)
{
	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version) {
		clk_disable_unprepare(vsp_hw_dev->clk_ahb_vsp);
	} else if (vsp_hw_dev->version == SHARKL3) {
		clk_disable_unprepare(vsp_hw_dev->clk_ahb_vsp);
		clk_disable_unprepare(vsp_hw_dev->clk_emc_vsp);
	}

	if ((vsp_hw_dev->version == IWHALE2 || vsp_hw_dev->version == SHARKL3)
		&& (vsp_hw_dev->clk_vsp_ahb_mmu_eb != NULL))
		clk_disable_unprepare(vsp_hw_dev->clk_vsp_ahb_mmu_eb);
	clk_disable_unprepare(vsp_hw_dev->vsp_clk);
	if (vsp_hw_dev->version != SHARKLE) {
		if (vsp_hw_dev->clk_axi_gate_vsp)
			clk_disable_unprepare(vsp_hw_dev->clk_axi_gate_vsp);
	}

	if (WHALE2 == vsp_hw_dev->version || IWHALE2 == vsp_hw_dev->version
		|| ISHARKL2 == vsp_hw_dev->version
		|| vsp_hw_dev->version == SHARKL3)
		clk_disable_unprepare(vsp_hw_dev->clk_vsp_ckg);
	if (vsp_hw_dev->clk_vsp_mq_ahb_eb)
		clk_disable_unprepare(vsp_hw_dev->clk_vsp_mq_ahb_eb);
	clk_disable_unprepare(vsp_hw_dev->clk_ahb_gate_vsp_eb);
	if (vsp_hw_dev->version != SHARKLE)
		clk_disable_unprepare(vsp_hw_dev->clk_mm_eb);
}

