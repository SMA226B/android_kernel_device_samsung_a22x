/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/clk.h>

#include "seninf_clk.h"

#ifdef IMGSENSOR_DFS_CTRL_ENABLE
struct pm_qos_request imgsensor_qos;

int imgsensor_dfs_ctrl(enum DFS_OPTION option, void *pbuff)
{
	int i4RetValue = 0;

	/*pr_info("%s\n", __func__);*/

	switch (option) {
	case DFS_CTRL_ENABLE:
		pm_qos_add_request(&imgsensor_qos, PM_QOS_CAM_FREQ, 0);
		pr_debug("seninf PMQoS turn on\n");
		break;
	case DFS_CTRL_DISABLE:
		pm_qos_remove_request(&imgsensor_qos);
		pr_debug("seninf PMQoS turn off\n");
		break;
	case DFS_UPDATE:
		pr_debug(
			"seninf Set isp clock level:%d\n",
			*(unsigned int *)pbuff);
		pm_qos_update_request(&imgsensor_qos, *(unsigned int *)pbuff);

		break;
	case DFS_RELEASE:
		pr_debug(
			"seninf release and set isp clk request to 0\n");
		pm_qos_update_request(&imgsensor_qos, 0);
		break;
	case DFS_SUPPORTED_ISP_CLOCKS:
	{
		int result = 0;
		uint64_t freq_steps[ISP_CLK_LEVEL_CNT] = {0};
		struct IMAGESENSOR_GET_SUPPORTED_ISP_CLK *pIspclks;
		unsigned int lv = 0;

		pIspclks = (struct IMAGESENSOR_GET_SUPPORTED_ISP_CLK *) pbuff;

		/* Call mmdvfs_qos_get_freq_steps
		 * to get supported frequency
		 */
		result = mmdvfs_qos_get_freq_steps(
			PM_QOS_CAM_FREQ,
			freq_steps, (u32 *)&pIspclks->clklevelcnt);

		if (result < 0) {
			PK_PR_ERR(
				"ERR: get MMDVFS freq steps failed, result: %d\n",
				result);
			i4RetValue = -EFAULT;
			break;
		}

		if (pIspclks->clklevelcnt > ISP_CLK_LEVEL_CNT) {
			PK_PR_ERR("ERR: clklevelcnt is exceeded");
			i4RetValue = -EFAULT;
			break;
		}

		for (lv = 0; lv < pIspclks->clklevelcnt; lv++) {
			/* Save clk from low to high */
			pIspclks->clklevel[lv] = freq_steps[lv];
			/*pr_debug("DFS Clk level[%d]:%d",
			 *	lv, pIspclks->clklevel[lv]);
			 */
		}
	}
		break;
	case DFS_CUR_ISP_CLOCK:
	{
		unsigned int *pGetIspclk;

		pGetIspclk = (unsigned int *) pbuff;
		*pGetIspclk = (u32)mmdvfs_qos_get_freq(PM_QOS_CAM_FREQ);
		/*pr_debug("current isp clock:%d", *pGetIspclk);*/
	}
		break;
	default:
		pr_info("None\n");
		break;
	}
	return i4RetValue;
}
#endif

static inline void seninf_clk_check(struct SENINF_CLK *pclk)
{
	int i;

	for (i = 0; i < SENINF_CLK_IDX_MAX_NUM; i++)
		WARN_ON(IS_ERR(pclk->mclk_sel[i]));
}

/**********************************************************
 *Common Clock Framework (CCF)
 **********************************************************/
enum SENINF_RETURN seninf_clk_init(struct SENINF_CLK *pclk)
{
	int i;

	if (pclk->pplatform_device == NULL) {
		PK_PR_ERR("[%s] pdev is null\n", __func__);
		return SENINF_RETURN_ERROR;
	}
	/* get all possible using clocks */
	for (i = 0; i < SENINF_CLK_IDX_MAX_NUM; i++) {
		pclk->mclk_sel[i] = devm_clk_get(&pclk->pplatform_device->dev,
						gseninf_mclk_name[i].pctrl);
		atomic_set(&pclk->enable_cnt[i], 0);

		if (IS_ERR(pclk->mclk_sel[i])) {
			PK_PR_ERR("cannot get %d clock\n", i);
			return SENINF_RETURN_ERROR;
		}
	}
#ifdef CONFIG_PM_SLEEP
	wakeup_source_init(&pclk->seninf_wake_lock, "seninf_lock_wakelock");
#endif
	atomic_set(&pclk->wakelock_cnt, 0);

	return SENINF_RETURN_SUCCESS;
}

int seninf_clk_set(struct SENINF_CLK *pclk,
					struct ACDK_SENSOR_MCLK_STRUCT *pmclk)
{
	int i, ret = 0;
	unsigned int idx_tg, idx_freq = 0;

	if (pmclk->TG >= SENINF_CLK_TG_MAX_NUM ||
	    pmclk->freq > SENINF_CLK_MCLK_FREQ_MAX ||
		pmclk->freq < SENINF_CLK_MCLK_FREQ_MIN) {
		PK_PR_ERR(
	"[CAMERA SENSOR]kdSetSensorMclk out of range, tg = %d, freq = %d\n",
		  pmclk->TG, pmclk->freq);
		return -EFAULT;
	}

	PK_DBG("[CAMERA SENSOR] CCF kdSetSensorMclk on= %d, freq= %d, TG= %d\n",
	       pmclk->on, pmclk->freq, pmclk->TG);

	seninf_clk_check(pclk);

	for (i = 0; ((i < SENINF_CLK_IDX_FREQ_IDX_NUM) &&
			(pmclk->freq != gseninf_clk_freq[i])); i++)
		;

	if (i >= SENINF_CLK_IDX_FREQ_IDX_NUM)
		return -EFAULT;

	idx_tg = pmclk->TG + SENINF_CLK_IDX_TG_MIN_NUM;
	idx_freq = i + SENINF_CLK_IDX_FREQ_MIN_NUM;

	if (pmclk->on) {
#ifdef TG1_ALWAYS_ON
		/* Workaround for timestamp: TG1 always ON */
		if (clk_prepare_enable(
			pclk->mclk_sel[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]))
			PK_PR_ERR("[CAMERA SENSOR] failed tg=%d\n",
				  SENINF_CLK_IDX_TG_TOP_MUX_CAMTG);
		else
			atomic_inc(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
#endif

		if (clk_prepare_enable(pclk->mclk_sel[idx_tg]))
			PK_PR_ERR("[CAMERA SENSOR] failed tg=%d\n", pmclk->TG);
		else
			atomic_inc(&pclk->enable_cnt[idx_tg]);

		if (clk_prepare_enable(pclk->mclk_sel[idx_freq]))
			PK_PR_ERR("[CAMERA SENSOR] failed freq idx= %d\n", i);
		else
			atomic_inc(&pclk->enable_cnt[idx_freq]);

		ret = clk_set_parent(
			pclk->mclk_sel[idx_tg], pclk->mclk_sel[idx_freq]);
	} else {
		if (atomic_read(&pclk->enable_cnt[idx_freq]) > 0) {
			clk_disable_unprepare(pclk->mclk_sel[idx_freq]);
			atomic_dec(&pclk->enable_cnt[idx_freq]);
		}

		if (atomic_read(&pclk->enable_cnt[idx_tg]) > 0) {
			clk_disable_unprepare(pclk->mclk_sel[idx_tg]);
			atomic_dec(&pclk->enable_cnt[idx_tg]);
		}
#ifdef TG1_ALWAYS_ON
		/* Workaround for timestamp: TG1 always ON */
		if (atomic_read(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG])
			> 0) {
			clk_disable_unprepare(
			pclk->mclk_sel[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
			atomic_dec(
			&pclk->enable_cnt[SENINF_CLK_IDX_TG_TOP_MUX_CAMTG]);
		}
#endif
	}

	return ret;
}

void seninf_clk_open(struct SENINF_CLK *pclk)
{
	MINT32 i;

	PK_DBG("open\n");

	if (atomic_inc_return(&pclk->wakelock_cnt) == 1) {
#ifdef CONFIG_PM_SLEEP
		__pm_stay_awake(&pclk->seninf_wake_lock);
#endif
	}

	for (i = SENINF_CLK_IDX_SYS_MIN_NUM;
		i < SENINF_CLK_IDX_SYS_MAX_NUM;
		i++) {
		if (clk_prepare_enable(pclk->mclk_sel[i]))
			PK_PR_ERR("[CAMERA SENSOR] failed sys idx= %d\n", i);
		else
			atomic_inc(&pclk->enable_cnt[i]);
	}
}

void seninf_clk_release(struct SENINF_CLK *pclk)
{
	MINT32 i = SENINF_CLK_IDX_MAX_NUM;

	PK_DBG("release\n");

	do {
		i--;
		for (; atomic_read(&pclk->enable_cnt[i]) > 0;) {
			clk_disable_unprepare(pclk->mclk_sel[i]);
			atomic_dec(&pclk->enable_cnt[i]);
		}
	} while (i);

	if (atomic_dec_and_test(&pclk->wakelock_cnt)) {
#ifdef CONFIG_PM_SLEEP
		__pm_relax(&pclk->seninf_wake_lock);
#endif
	}
}

unsigned int seninf_clk_get_meter(struct SENINF_CLK *pclk, unsigned int clk)
{
	unsigned int i = 0;

	if (clk == 0xff) {
		for (i = SENINF_CLK_IDX_SYS_MIN_NUM; i < SENINF_CLK_IDX_SYS_MAX_NUM; ++i) {
			PK_DBG("[sensor_dump][mclk]index=%u freq=%lu HW enable=%d enable_cnt=%u\n",
				i,
				clk_get_rate(pclk->mclk_sel[i]),
				__clk_is_enabled(pclk->mclk_sel[i]),
				__clk_get_enable_count(pclk->mclk_sel[i])
			);
		}
	}

#if 0//SENINF_CLK_CONTROL
	/* workaround */
	mt_get_ckgen_freq(1);

	if (clk == 4) {
		PK_DBG("CAMSYS_SENINF_CGPDN = %lu\n",
		clk_get_rate(
		pclk->mclk_sel[SENINF_CLK_IDX_SYS_CAMSYS_SENINF_CGPDN]));
		PK_DBG("TOP_MUX_SENINF = %lu\n",
			clk_get_rate(
			pclk->mclk_sel[SENINF_CLK_IDX_SYS_TOP_MUX_SENINF]));
	}
	if (clk < 64)
		return mt_get_ckgen_freq(clk);
	else
		return 0;
#else
	if (clk < 64)
		return mt_get_ckgen_freq(clk);
	else
		return 0;
#endif
}

