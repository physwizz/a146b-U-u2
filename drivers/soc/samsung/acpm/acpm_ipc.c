/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/list.h>
#include <linux/wait.h>
#include <linux/slab.h>
#include <soc/samsung/exynos/debug-snapshot.h>
#include <linux/sched/clock.h>
#include <linux/module.h>
#include <linux/workqueue.h>
#include <linux/kdebug.h>

#include "acpm.h"
#include "acpm_ipc.h"
#include "fw_header/framework.h"

static struct acpm_ipc_info *acpm_ipc = NULL;
static struct acpm_ipc_info *esca_ipc = NULL;
static struct workqueue_struct *update_log_wq;
static struct acpm_debug_info *acpm_debug = NULL;
static bool is_acpm_stop_log = false;
static bool is_acpm_ramdump = false;
static bool acpm_stop_log_req = false;
static u32 acpm_period = APM_PERITIMER_NS_PERIOD;

static u32 last_acpm_peri_timer;

#if defined(CONFIG_SOC_S5E8835)
unsigned int acpm_nfc_log_offset, acpm_nfc_log_len;
#endif

extern struct atomic_notifier_head panic_notifier_list;

#if defined(CONFIG_SOC_S5E8835)
int acpm_get_nfc_log_buf(struct nfc_clk_req_log **buf, u32 *last_ptr, u32 *len)
{
	if (!acpm_nfc_log_offset || !acpm_nfc_log_len)
		return -ENOENT;

	*last_ptr = __raw_readl(acpm_ipc->sram_base + acpm_nfc_log_offset);
	*len = acpm_nfc_log_len;
	*buf = (struct nfc_clk_req_log *)(acpm_ipc->sram_base + acpm_nfc_log_offset + 4);

	return 0;
}
#endif

void __iomem *get_acpm_sram_base(void)
{
	return acpm_ipc->sram_base;
}

#if defined(CONFIG_EXYNOS_RGT) || defined(CONFIG_EXYNOS_RGT_MODULE)
extern void exynos_rgt_dbg_snapshot_regulator(u32 val, unsigned long long time);
#else
static inline void exynos_rgt_dbg_snapshot_regulator(u32 val, unsigned long long time)
{
	return ;
}
#endif

void acpm_fw_log_level(unsigned int on)
{
	acpm_debug->debug_log_level = on;
}

void acpm_ramdump(void)
{
	unsigned int rear = 0;
	unsigned int id;
	unsigned int index;
	unsigned int count;
	unsigned char str[9] = {0,};
	unsigned int val;
	unsigned int log_header;
	unsigned long long time;

	if (!is_acpm_ramdump)
		is_acpm_ramdump = true;
	else
		return;

	do {
		log_header = __raw_readl(acpm_debug->log_buff_base + acpm_debug->log_buff_size * rear);

		/* log header information
		 * id: [31:28]
		 * log level : [27]
		 * index: [26:22]
		 * apm systick count: [15:0]
		 */
		id = (log_header & (0xF << LOG_ID_SHIFT)) >> LOG_ID_SHIFT;
		index = (log_header & (0x1f << LOG_TIME_INDEX)) >> LOG_TIME_INDEX;
		count = log_header & 0xffff;

		/* string length: log_buff_size - header(4) - integer_data(4) */
		memcpy_align_4(str, acpm_debug->log_buff_base + (acpm_debug->log_buff_size * rear) + 4,
				acpm_debug->log_buff_size - 8);

		val = __raw_readl(acpm_debug->log_buff_base + acpm_debug->log_buff_size * rear +
				acpm_debug->log_buff_size - 4);

		time = acpm_debug->timestamps[index];

		/* peritimer period: (1 * 256) / 24.576MHz*/
		time += count * acpm_period;

		/* speedy channel: [31:28] addr : [23:12], data : [11:4]*/
		if (id == REGULATOR_INFO_ID)
			exynos_rgt_dbg_snapshot_regulator(val, time);

		dbg_snapshot_acpm(time, str, val);

		if (acpm_debug->log_buff_len == (rear + 1))
			rear = 0;
		else
			rear++;

	} while (rear != 0);

	if (acpm_debug->dump_size)
		memcpy(acpm_debug->dump_dram_base, acpm_debug->dump_base, acpm_debug->dump_size);
}

void timestamp_write(void)
{
	unsigned long long cur_clk;
	unsigned long long sys_tick;
	unsigned long flags;
	unsigned int tmp_index;

	spin_lock_irqsave(&acpm_debug->lock, flags);

	tmp_index = __raw_readl(acpm_debug->time_index);

	sys_tick = exynos_get_peri_timer_icvra();
	last_acpm_peri_timer = sys_tick;
	sys_tick = acpm_debug->timestamps[tmp_index] + sys_tick * acpm_period;
	cur_clk = sched_clock();

	tmp_index++;

	if (tmp_index == acpm_debug->num_timestamps)
		tmp_index = 0;

	acpm_debug->timestamps[tmp_index] = cur_clk;
	acpm_debug->ipc_info->initdata->timestamps[tmp_index] = cur_clk;

	__raw_writel(tmp_index, acpm_debug->time_index);
	exynos_acpm_timer_clear();

	if (sys_tick > cur_clk)
		acpm_period--;
	else
		acpm_period++;

	spin_unlock_irqrestore(&acpm_debug->lock, flags);
}

static void acpm_log_idx_update(void)
{
	unsigned int front;
	unsigned int rear;

	if (acpm_stop_log_req)
		return ;
	/* ACPM Log data dequeue & print */
	front = __raw_readl(acpm_debug->log_buff_front);
	rear = __raw_readl(acpm_debug->log_buff_rear);

	if (rear != front)
		__raw_writel(front, acpm_debug->log_buff_rear);
}

void acpm_log_print(void)
{
	unsigned int front;
	unsigned int rear;
	unsigned int id;
	unsigned int index;
	unsigned int count;
	unsigned char str[9] = {0,};
	unsigned int val;
	unsigned int log_header;
	unsigned long long time;
	unsigned int log_level;

	if (is_acpm_stop_log)
		return ;
	/* ACPM Log data dequeue & print */
	front = __raw_readl(acpm_debug->log_buff_front);
	rear = __raw_readl(acpm_debug->log_buff_rear);

	while (rear != front) {
		log_header = __raw_readl(acpm_debug->log_buff_base + acpm_debug->log_buff_size * rear);

		/* log header information
		 * id: [31:28]
		 * log level : [27]
		 * index: [26:22]
		 * apm systick count: [15:0]
		 */
		id = (log_header & (0xF << LOG_ID_SHIFT)) >> LOG_ID_SHIFT;
		log_level = (log_header & (0x1 << LOG_LEVEL)) >> LOG_LEVEL;
		index = (log_header & (0x1f << LOG_TIME_INDEX)) >> LOG_TIME_INDEX;
		count = log_header & 0xffff;

		/* string length: log_buff_size - header(4) - integer_data(4) */
		memcpy_align_4(str, acpm_debug->log_buff_base + (acpm_debug->log_buff_size * rear) + 4,
				acpm_debug->log_buff_size - 8);

		val = __raw_readl(acpm_debug->log_buff_base + acpm_debug->log_buff_size * rear +
				acpm_debug->log_buff_size - 4);

		time = acpm_debug->timestamps[index];

		/* peritimer period: (1 * 256) / 24.576MHz*/
		time += count * acpm_period;

		/* speedy channel: [31:28] addr : [23:12], data : [11:4]*/
		if (id == REGULATOR_INFO_ID)
			exynos_rgt_dbg_snapshot_regulator(val, time);

		dbg_snapshot_acpm(time, str, val);

		if (acpm_debug->debug_log_level == 1 || !log_level)
			pr_info("[ACPM_FW] : %llu id:%u, %s, %x\n", time, id, str, val);

		if (acpm_debug->log_buff_len == (rear + 1))
			rear = 0;
		else
			rear++;

		__raw_writel(rear, acpm_debug->log_buff_rear);
		front = __raw_readl(acpm_debug->log_buff_front);
	}

	if (acpm_stop_log_req) {
		is_acpm_stop_log = true;
		acpm_ramdump();
	}
}

ktime_t acpm_time_calc(u32 start, u32 end)
{
	u32 interval;

	if (start > end)
		interval = last_acpm_peri_timer - start + end;
	else
		interval = end - start;

	return interval * acpm_period;
}
EXPORT_SYMBOL_GPL(acpm_time_calc);

u32 acpm_get_peri_timer(void)
{
	return exynos_get_peri_timer_icvra();
}
EXPORT_SYMBOL_GPL(acpm_get_peri_timer);

void acpm_stop_log(void)
{
	acpm_stop_log_req = true;
	acpm_log_print();
}
EXPORT_SYMBOL_GPL(acpm_stop_log);

static void acpm_update_log(struct work_struct *work)
{
	acpm_log_print();
}

static void acpm_debug_logging(struct work_struct *work)
{
	if (acpm_debug->debug_log_level)
		acpm_log_print();

	timestamp_write();

	queue_delayed_work_on(0, update_log_wq, &acpm_debug->periodic_work,
			msecs_to_jiffies(acpm_debug->period));
}

static unsigned int __acpm_ipc_request_channel(struct acpm_ipc_info *ipc,
					struct device_node *np,
					ipc_callback handler,
					unsigned int *id, unsigned int *size)
{
	struct callback_info *cb;
	int i, len, req_ch_id;
	const __be32 *prop;

	if (!np)
		return -ENODEV;

	prop = of_get_property(np, "acpm-ipc-channel", &len);
	if (!prop)
		return -ENOENT;
	req_ch_id = be32_to_cpup(prop);

	for(i = 0; i < ipc->num_channels; i++) {
		if (ipc->channel[i].id == req_ch_id) {
			*id = ipc->channel[i].id;
			*size = ipc->channel[i].tx_ch.size;

			if (handler) {
				cb = devm_kzalloc(ipc->dev, sizeof(struct callback_info),
						GFP_KERNEL);
				if (cb == NULL)
					return -ENOMEM;
				cb->ipc_callback = handler;
				cb->client = np;

				spin_lock(&ipc->channel[i].ch_lock);
				list_add(&cb->list, &ipc->channel[i].list);
				spin_unlock(&ipc->channel[i].ch_lock);
			}

			return 0;
		}
	}

	return -ENODEV;
}

unsigned int acpm_ipc_request_channel(struct device_node *np, ipc_callback handler,
		unsigned int *id, unsigned int *size)
{
	return __acpm_ipc_request_channel(acpm_ipc, np, handler, id, size);
}
EXPORT_SYMBOL_GPL(acpm_ipc_request_channel);

#if IS_ENABLED(CONFIG_EXYNOS_ESCA)
unsigned int esca_ipc_request_channel(struct device_node *np, ipc_callback handler,
		unsigned int *id, unsigned int *size)
{
	if (esca_ipc)
		return __acpm_ipc_request_channel(esca_ipc, np, handler, id, size);
	else
		return __acpm_ipc_request_channel(acpm_ipc, np, handler, id, size);
}
EXPORT_SYMBOL_GPL(esca_ipc_request_channel);
#endif

static unsigned int __acpm_ipc_release_channel(struct acpm_ipc_info *ipc,
					     struct device_node *np,
					     unsigned int channel_id)
{
	struct acpm_ipc_ch *channel = &ipc->channel[channel_id];
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;

	list_for_each_entry(cb, cb_list, list) {
		if (cb->client == np) {
			spin_lock(&channel->ch_lock);
			list_del(&cb->list);
			spin_unlock(&channel->ch_lock);
			devm_kfree(ipc->dev, cb);
			break;
		}
	}

	return 0;
}

unsigned int acpm_ipc_release_channel(struct device_node *np, unsigned int channel_id)
{
	return __acpm_ipc_release_channel(acpm_ipc, np, channel_id);
}
EXPORT_SYMBOL_GPL(acpm_ipc_release_channel);

unsigned int esca_ipc_release_channel(struct device_node *np, unsigned int channel_id)
{
	if (esca_ipc)
		return __acpm_ipc_release_channel(esca_ipc, np, channel_id);
	else
		return __acpm_ipc_release_channel(acpm_ipc, np, channel_id);
}
EXPORT_SYMBOL_GPL(esca_ipc_release_channel);

static void acpm_interrupt_gen(struct acpm_ipc_info *ipc, unsigned int id)
{
	/* APM NVIC INTERRUPT GENERATE */
	if (ipc->is_mailbox_master)
		writel((1 << id) << 16, ipc->intr + INTGR0);
	else
		writel((1 << id), ipc->intr + INTGR1);
}

static void acpm_interrupt_self_gen(struct acpm_ipc_info *ipc, unsigned int id)
{
	/* APM NVIC INTERRUPT SELF GENERATE */
	if (ipc->is_mailbox_master)
		writel((1 << id), ipc->intr + INTGR1);
	else
		writel((1 << id) << 16, ipc->intr + INTGR0);
}

static void acpm_interrupt_clr(struct acpm_ipc_info *ipc, unsigned int id)
{
	/* APM NVIC INTERRUPT Clear */
	if (ipc->is_mailbox_master)
		writel((1 << id), ipc->intr + INTCR1);
	else
		writel((1 << id) << 16, ipc->intr + INTCR0);
}

static unsigned int get_acpm_interrupt_status(struct acpm_ipc_info *ipc)
{
	/* APM NVIC INTERRUPT Clear */
	if (ipc->is_mailbox_master)
		return __raw_readl(ipc->intr + INTSR1);
	else
		return __raw_readl(ipc->intr + INTSR0) >> 16;
}

static void acpm_interrupt_mask(struct acpm_ipc_info *ipc, unsigned int mask)
{
	/* APM NVIC INTERRUPT Mask */
	if (ipc->is_mailbox_master)
		writel(mask, ipc->intr + INTMR1);
	else
		writel(mask << 16, ipc->intr + INTMR0);
}

static bool check_response(struct acpm_ipc_info *ipc,
			   struct acpm_ipc_ch *channel,
			   struct ipc_config *cfg)
{
	unsigned int front;
	unsigned int rear;
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;
	unsigned int tmp_seq_num;
	bool ret = true;
	unsigned int i;
	unsigned long flags;

	spin_lock_irqsave(&channel->rx_lock, flags);
	/* IPC command dequeue */
	front = __raw_readl(channel->rx_ch.front);
	rear = __raw_readl(channel->rx_ch.rear);

	i = rear;

	while (i != front) {
		tmp_seq_num = __raw_readl(channel->rx_ch.base + channel->rx_ch.size * i);
		tmp_seq_num = (tmp_seq_num >> ACPM_IPC_PROTOCOL_SEQ_NUM) & 0x3f;
		if (tmp_seq_num == ((cfg->cmd[0] >> ACPM_IPC_PROTOCOL_SEQ_NUM) & 0x3f)) {
			memcpy_align_4(cfg->cmd, channel->rx_ch.base + channel->rx_ch.size * i,
					channel->rx_ch.size);
			memcpy_align_4(channel->cmd, channel->rx_ch.base + channel->rx_ch.size * i,
					channel->rx_ch.size);

			/* i: target command, rear: another command
			 * 1. i index command dequeue
			 * 2. rear index command copy to i index position
			 * 3. incresed rear index
			 */
			if (i != rear)
				memcpy_align_4(channel->rx_ch.base + channel->rx_ch.size * i,
						channel->rx_ch.base + channel->rx_ch.size * rear,
						channel->rx_ch.size);

			list_for_each_entry(cb, cb_list, list)
				if (cb && cb->ipc_callback)
					cb->ipc_callback(channel->cmd, channel->rx_ch.size);

			rear++;
			rear = rear % channel->rx_ch.len;

			__raw_writel(rear, channel->rx_ch.rear);
			front = __raw_readl(channel->rx_ch.front);

			if (!channel->interrupt && rear == front) {
				acpm_interrupt_clr(ipc, channel->id);
				if (rear != __raw_readl(channel->rx_ch.front))
					acpm_interrupt_self_gen(ipc, channel->id);
			}
			ret = false;
			channel->seq_num_flag[tmp_seq_num] = 0;
			break;
		}
		i++;
		i = i % channel->rx_ch.len;
	}

	spin_unlock_irqrestore(&channel->rx_lock, flags);

	return ret;
}

static void dequeue_policy(struct acpm_ipc_ch *channel)
{
	unsigned int front;
	unsigned int rear;
	struct list_head *cb_list = &channel->list;
	struct callback_info *cb;
	unsigned long flags;

	spin_lock_irqsave(&channel->rx_lock, flags);

	if (channel->type == TYPE_BUFFER) {
		memcpy_align_4(channel->cmd, channel->rx_ch.base, channel->rx_ch.size);
		spin_unlock_irqrestore(&channel->rx_lock, flags);
		list_for_each_entry(cb, cb_list, list)
			if (cb && cb->ipc_callback)
				cb->ipc_callback(channel->cmd, channel->rx_ch.size);

		return;
	}

	/* IPC command dequeue */
	front = __raw_readl(channel->rx_ch.front);
	rear = __raw_readl(channel->rx_ch.rear);

	while (rear != front) {
		memcpy_align_4(channel->cmd, channel->rx_ch.base + channel->rx_ch.size * rear,
				channel->rx_ch.size);

		list_for_each_entry(cb, cb_list, list)
			if (cb && cb->ipc_callback)
				cb->ipc_callback(channel->cmd, channel->rx_ch.size);

		rear++;
		rear = rear % channel->rx_ch.len;

		if (!channel->polling)
			complete(&channel->wait);

		__raw_writel(rear, channel->rx_ch.rear);
		front = __raw_readl(channel->rx_ch.front);
	}

	acpm_log_idx_update();
	spin_unlock_irqrestore(&channel->rx_lock, flags);
}

static irqreturn_t acpm_ipc_irq_handler(int irq, void *data)
{
	struct acpm_ipc_info *ipc = data;
	unsigned int status;
	int i;

	/* ACPM IPC INTERRUPT STATUS REGISTER */
	status = get_acpm_interrupt_status(ipc);
	ipc->intr_status = 0;

	for (i = 0; i < ipc->num_channels; i++) {
		if (status & (0x1 << ipc->channel[i].id)) {
			if (ipc->channel[i].interrupt) {
				acpm_interrupt_clr(ipc, ipc->channel[i].id);
				complete(&ipc->channel[i].wait);
			} else if(!ipc->channel[i].polling) {
				acpm_interrupt_clr(ipc, ipc->channel[i].id);
				ipc->intr_status = (1 << i);
				dequeue_policy(&ipc->channel[i]);
			}
		}
	}

	/*
	 * Threaded IRQ wake is unused.
	if (ipc->intr_status)
		return IRQ_WAKE_THREAD;
	*/

	return IRQ_HANDLED;
}

static irqreturn_t acpm_ipc_irq_handler_thread(int irq, void *data)
{
	struct acpm_ipc_info *ipc = data;
	int i;

	for (i = 0; i < ipc->num_channels; i++)
		if (!ipc->channel[i].polling && (ipc->intr_status & (1 << i)))
			dequeue_policy(&ipc->channel[i]);

	return IRQ_HANDLED;
}

static int enqueue_indirection_cmd(struct acpm_ipc_ch *channel,
		struct ipc_config *cfg)
{
	unsigned int front;
	unsigned int rear;
	unsigned int buf;
	bool timeout_flag = 0;

	if (cfg->indirection) {
		front = __raw_readl(channel->tx_ch.front);
		rear = __raw_readl(channel->tx_ch.rear);

		/* another indirection command check */
		while (rear != front) {
			buf = __raw_readl(channel->tx_ch.base + channel->tx_ch.size * rear);

			if (buf & (1 << ACPM_IPC_PROTOCOL_INDIRECTION)) {

				UNTIL_EQUAL(true, rear != __raw_readl(channel->tx_ch.rear),
						timeout_flag);

				if (timeout_flag) {
					acpm_log_print();
					return -ETIMEDOUT;
				} else {
					rear = __raw_readl(channel->tx_ch.rear);
				}

			} else {
				if (channel->tx_ch.len == (rear + 1))
					rear = 0;
				else
					rear++;
			}
		}

		if (cfg->indirection_base)
			memcpy_align_4(channel->tx_ch.direction, cfg->indirection_base,
					cfg->indirection_size);
		else
			return -EINVAL;
	}

	return 0;
}

int __acpm_ipc_send_data(struct acpm_ipc_info *ipc, unsigned int channel_id,
			 struct ipc_config *cfg, bool w_mode)
{
	unsigned int front;
	unsigned int rear;
	unsigned int tmp_index;
	struct acpm_ipc_ch *channel;
	bool timeout_flag = 0;
	int ret;
	u64 timeout, now;
	u32 retry_cnt = 0;
	u32 tmp_seq_num;
	u32 seq_cnt = 0;
	unsigned long flags;

	if (channel_id >= ipc->num_channels && !cfg)
		return -EIO;

	channel = &ipc->channel[channel_id];

	if (channel->interrupt && cfg->response)
		mutex_lock(&channel->wait_lock);

	spin_lock_irqsave(&channel->tx_lock, flags);

	front = __raw_readl(channel->tx_ch.front);
	rear = __raw_readl(channel->tx_ch.rear);

	tmp_index = front + 1;

	if (tmp_index >= channel->tx_ch.len)
		tmp_index = 0;

	/* buffer full check */
	UNTIL_EQUAL(true, tmp_index != __raw_readl(channel->tx_ch.rear), timeout_flag);
	if (timeout_flag) {
		acpm_log_print();
		acpm_debug->debug_log_level = 1;
		spin_unlock_irqrestore(&channel->tx_lock, flags);
		if (channel->interrupt && cfg->response)
			mutex_unlock(&channel->wait_lock);
		pr_err("[%s] tx buffer full! timeout!!!\n", __func__);
		return -ETIMEDOUT;
	}

	if (!cfg->cmd) {
		spin_unlock_irqrestore(&channel->tx_lock, flags);
		if (channel->interrupt && cfg->response)
			mutex_unlock(&channel->wait_lock);
		return -EIO;
	}

	tmp_seq_num = channel->seq_num;
	do {
		if (unlikely(tmp_seq_num != channel->seq_num)) {
			pr_warn("[ACPM IPC] [ACPM_IPC] channel:%d, cmd:0x%x, 0x%x, 0x%x, 0x%x",
					channel->id, cfg->cmd[0], cfg->cmd[1],
					cfg->cmd[2], cfg->cmd[3]);
			pr_warn("[ACPM IPC] duplicate assignment: sequence number:%d, tmp_seq_num:%d, flag:0x%x",
					channel->seq_num, tmp_seq_num, channel->seq_num_flag[tmp_seq_num]);
		}

		if (++tmp_seq_num == SEQUENCE_NUM_MAX)
			tmp_seq_num = 1;

		if (unlikely(seq_cnt++ == SEQUENCE_NUM_MAX)) {
			pr_err("[ACPM IPC] sequence number full! error!!!\n");
			BUG();
		}

	} while (channel->seq_num_flag[tmp_seq_num]);

	channel->seq_num = tmp_seq_num;
	if (channel->polling && cfg->response)
		channel->seq_num_flag[channel->seq_num] = cfg->cmd[0] | (0x1 << 31);

	cfg->cmd[0] &= ~(0x3f << ACPM_IPC_PROTOCOL_SEQ_NUM);
	cfg->cmd[0] |= (channel->seq_num & 0x3f) << ACPM_IPC_PROTOCOL_SEQ_NUM;

	memcpy_align_4(channel->tx_ch.base + channel->tx_ch.size * front, cfg->cmd,
			channel->tx_ch.size);

	cfg->cmd[1] = 0;
	cfg->cmd[2] = 0;
	cfg->cmd[3] = 0;

	ret = enqueue_indirection_cmd(channel, cfg);
	if (ret) {
		pr_err("[ACPM] indirection command fail %d\n", ret);
		spin_unlock_irqrestore(&channel->tx_lock, flags);
		if (channel->interrupt && cfg->response)
			mutex_unlock(&channel->wait_lock);
		return ret;
	}

	writel(tmp_index, channel->tx_ch.front);

	acpm_interrupt_gen(ipc, channel->id);
	spin_unlock_irqrestore(&channel->tx_lock, flags);

	if (channel->polling && cfg->response && !channel->interrupt) {
retry:
		timeout = sched_clock() + IPC_TIMEOUT;
		timeout_flag = false;

		while (!(get_acpm_interrupt_status(ipc) & (1 << channel->id)) ||
				check_response(ipc, channel, cfg)) {
			now = sched_clock();
			if (timeout < now) {
				if (retry_cnt > 5) {
					timeout_flag = true;
					break;
				} else if (retry_cnt > 0) {
					pr_err("acpm_ipc timeout retry %d "
						"now = %llu,"
						"timeout = %llu\n",
						retry_cnt, now, timeout);
					++retry_cnt;
					goto retry;
				} else {
					++retry_cnt;
					continue;
				}
			} else {
				if (w_mode)
					usleep_range(50, 100);
				else
					udelay(10);
			}
		}
	} else if (channel->interrupt && cfg->response) {
		timeout = sched_clock() + IPC_TIMEOUT * 5;
		do {
			ret = wait_for_completion_interruptible_timeout(&channel->wait,
				  nsecs_to_jiffies(IPC_TIMEOUT));
			now = sched_clock();
			if (timeout < now) {
				timeout_flag = true;
				break;
			}
		} while (check_response(ipc, channel, cfg));
		mutex_unlock(&channel->wait_lock);
	} else {
		return 0;
	}

	if (timeout_flag) {
		if (!check_response(ipc, channel, cfg))
			return 0;
		pr_err("%s Timeout error! now = %llu, timeout = %llu\n",
				__func__, now, timeout);
		pr_err("[ACPM] int_status:0x%x, ch_id: 0x%x\n",
				get_acpm_interrupt_status(ipc),
				1 << channel->id);
		pr_err("[ACPM] queue, rx_rear:%u, rx_front:%u\n",
				__raw_readl(channel->rx_ch.rear),
				__raw_readl(channel->rx_ch.front));
		pr_err("[ACPM] queue, tx_rear:%u, tx_front:%u\n",
				__raw_readl(channel->tx_ch.rear),
				__raw_readl(channel->tx_ch.front));

		acpm_debug->debug_log_level = 1;
		acpm_log_print();
		acpm_debug->debug_log_level = 0;
		acpm_ramdump();

		dump_stack();
		msleep(1000);
		dbg_snapshot_expire_watchdog();
	}

	if (!is_acpm_stop_log) {
		if (acpm_debug->debug_log_level)
			queue_work(update_log_wq, &acpm_debug->update_log_work);
		else
			acpm_log_idx_update();
	}

	return 0;
}

int acpm_ipc_send_data(unsigned int channel_id, struct ipc_config *cfg)
{
	int ret;

	ret = __acpm_ipc_send_data(acpm_ipc, channel_id, cfg, false);

	return ret;
}
EXPORT_SYMBOL_GPL(acpm_ipc_send_data);

int esca_ipc_send_data(unsigned int channel_id, struct ipc_config *cfg)
{
	int ret;

	if (esca_ipc)
		ret = __acpm_ipc_send_data(esca_ipc, channel_id, cfg, false);
	else
		ret = __acpm_ipc_send_data(acpm_ipc, channel_id, cfg, false);

	return ret;
}
EXPORT_SYMBOL_GPL(esca_ipc_send_data);

bool is_acpm_ipc_busy(unsigned ch_id)
{
	struct acpm_ipc_ch *channel;
	unsigned int tx_front, rx_front;

	channel = &acpm_ipc->channel[ch_id];
	tx_front = __raw_readl(channel->tx_ch.front);
	rx_front = __raw_readl(channel->rx_ch.front);

	return (tx_front != rx_front);
}
EXPORT_SYMBOL_GPL(is_acpm_ipc_busy);

#if IS_ENABLED(CONFIG_EXYNOS_ESCA)
bool is_esca_ipc_busy(unsigned ch_id)
{
	struct acpm_ipc_ch *channel;
	unsigned int tx_front, rx_front;

	if (esca_ipc)
		channel = &esca_ipc->channel[ch_id];
	else
		channel = &acpm_ipc->channel[ch_id];

	tx_front = __raw_readl(channel->tx_ch.front);
	rx_front = __raw_readl(channel->rx_ch.front);

	return (tx_front != rx_front);
}
EXPORT_SYMBOL_GPL(is_esca_ipc_busy);
#endif

static void log_buffer_init(struct device *dev, struct device_node *node)
{
	const __be32 *prop;
	unsigned int num_timestamps = 0;
	unsigned int len = 0;
	unsigned int dump_base = 0;
	unsigned int dram_dump_base = 0;
	unsigned int dump_size = 0;

	prop = of_get_property(node, "num-timestamps", &len);
	if (prop)
		num_timestamps = be32_to_cpup(prop);

	acpm_debug = devm_kzalloc(dev, sizeof(struct acpm_debug_info), GFP_KERNEL);
	if (IS_ERR(acpm_debug))
		return ;

	acpm_debug->time_index = acpm_ipc->sram_base + acpm_ipc->initdata->ktime_index;
	acpm_debug->num_timestamps = num_timestamps;
	acpm_debug->timestamps = devm_kzalloc(dev,
			sizeof(unsigned long long) * num_timestamps, GFP_KERNEL);
	acpm_debug->log_buff_rear = acpm_ipc->sram_base + acpm_ipc->initdata->log_buf_rear;
	acpm_debug->log_buff_front = acpm_ipc->sram_base + acpm_ipc->initdata->log_buf_front;
	acpm_debug->log_buff_base = acpm_ipc->sram_base + acpm_ipc->initdata->log_data;
	acpm_debug->log_buff_len = acpm_ipc->initdata->log_entry_len;
	acpm_debug->log_buff_size = acpm_ipc->initdata->log_entry_size;

	prop = of_get_property(node, "debug-log-level", &len);
	if (prop)
		acpm_debug->debug_log_level = be32_to_cpup(prop);

	prop = of_get_property(node, "dump-base", &len);
	if (prop)
		dump_base = be32_to_cpup(prop);

	prop = of_get_property(node, "dump-size", &len);
	if (prop)
		dump_size = be32_to_cpup(prop);

	if (dump_base && dump_size) {
		acpm_debug->dump_base = ioremap(dump_base, dump_size);
		acpm_debug->dump_size = dump_size;
	}

	prop = of_get_property(node, "logging-period", &len);
	if (prop)
		acpm_debug->period = be32_to_cpup(prop);

	acpm_debug->dump_dram_base = kzalloc(acpm_debug->dump_size, GFP_KERNEL);
	dbg_snapshot_printk("[ACPM] acpm framework SRAM dump to dram base: 0x%x\n",
			virt_to_phys(acpm_debug->dump_dram_base));

	dbg_snapshot_add_bl_item_info("acpm_dram", virt_to_phys(acpm_debug->dump_dram_base),
						acpm_debug->dump_size);

	prop = of_get_property(node, "dram-dump-base", &len);
	if (prop) {
		dram_dump_base = be32_to_cpup(prop);
		dbg_snapshot_add_bl_item_info("acpm_sram", dram_dump_base, acpm_debug->dump_size);
	}
	pr_info("[ACPM] acpm framework SRAM dump to dram base: 0x%llx\n",
			virt_to_phys(acpm_debug->dump_dram_base));

	spin_lock_init(&acpm_debug->lock);
}

static int channel_init(struct acpm_ipc_info *ipc, u32 *ch_buf, int len)
{
	int i, j;
	unsigned int mask = 0;
	struct ipc_channel *ipc_ch;

	ipc->num_channels = ipc->initdata->ipc_ap_max;

	ipc->channel = devm_kzalloc(ipc->dev,
			sizeof(struct acpm_ipc_ch) * ipc->num_channels, GFP_KERNEL);

	for (i = 0; i < ipc->num_channels; i++) {
		ipc_ch = (struct ipc_channel *)(ipc->sram_base + ipc->initdata->ipc_channels);
		ipc->channel[i].polling = ipc_ch[i].ap_poll;
		ipc->channel[i].id = ipc_ch[i].id;
		ipc->channel[i].type = ipc_ch[i].type;
		mask |= ipc->channel[i].polling << ipc->channel[i].id;

		/* Channel's RX buffer info */
		ipc->channel[i].rx_ch.size = ipc_ch[i].ch.q_elem_size;
		ipc->channel[i].rx_ch.len = ipc_ch[i].ch.q_len;
		ipc->channel[i].rx_ch.rear = ipc->sram_base + ipc_ch[i].ch.tx_rear;
		ipc->channel[i].rx_ch.front = ipc->sram_base + ipc_ch[i].ch.tx_front;
		ipc->channel[i].rx_ch.base = ipc->sram_base + ipc_ch[i].ch.tx_base;
		/* Channel's TX buffer info */
		ipc->channel[i].tx_ch.size = ipc_ch[i].ch.q_elem_size;
		ipc->channel[i].tx_ch.len = ipc_ch[i].ch.q_len;
		ipc->channel[i].tx_ch.rear = ipc->sram_base + ipc_ch[i].ch.rx_rear;
		ipc->channel[i].tx_ch.front = ipc->sram_base + ipc_ch[i].ch.rx_front;
		ipc->channel[i].tx_ch.base = ipc->sram_base + ipc_ch[i].ch.rx_base;
#if !(defined(CONFIG_SOC_S5E9935) || defined(CONFIG_SOC_S5E8535) || defined(CONFIG_SOC_S5E8835))
		ipc->channel[i].tx_ch.d_buff_size = ipc_ch[i].ch.rx_indr_buf_size;
		ipc->channel[i].tx_ch.direction = ipc->sram_base + ipc_ch[i].ch.rx_indr_buf;
#endif

		ipc->channel[i].cmd = devm_kzalloc(ipc->dev,
				ipc->channel[i].tx_ch.size, GFP_KERNEL);

		init_completion(&ipc->channel[i].wait);
		spin_lock_init(&ipc->channel[i].rx_lock);
		spin_lock_init(&ipc->channel[i].tx_lock);
		spin_lock_init(&ipc->channel[i].ch_lock);
		INIT_LIST_HEAD(&ipc->channel[i].list);
		mutex_init(&ipc->channel[i].wait_lock);

		if (!ch_buf)
			continue;

		for (j = 0; j < len; j++) {
			if (i == ch_buf[j]) {
				ipc->channel[i].interrupt = true;
				mask &= ~(0x1 << i);
				pr_info("acpm interrupt-ch #%d enabled\n", i);
			}
		}
	}

	acpm_interrupt_mask(ipc, mask);

	return 0;
}

static int acpm_ipc_die_handler(struct notifier_block *nb,
		unsigned long l, void *buf)
{
	if (!acpm_stop_log_req)
		acpm_stop_log();
	return NOTIFY_DONE;
}

static struct notifier_block nb_die_block = {
	.notifier_call = acpm_ipc_die_handler,
};

static struct notifier_block nb_panic_block = {
	.notifier_call = acpm_ipc_die_handler,
	.priority = INT_MAX,
};

int acpm_ipc_probe(struct platform_device *pdev)
{
	struct acpm_ipc_info *ipc;
	struct acpm_framework *acpm_initdata;
	const struct acpm_ipc_drvdata *drvdata;
	struct device_node *node = pdev->dev.of_node;
	struct resource *res;
	int ret = 0, len;
	const __be32 *prop;
	u32 *ch_buf;
	unsigned int dump_base = 0, dump_size = 0;

	if (!node) {
		dev_err(&pdev->dev, "driver doesnt support"
				"non-dt devices\n");
		return -ENODEV;
	}

	dev_info(&pdev->dev, "acpm_ipc probe\n");

	ipc = devm_kzalloc(&pdev->dev,
			sizeof(struct acpm_ipc_info), GFP_KERNEL);

	if (IS_ERR(ipc))
		return PTR_ERR(ipc);

	ipc->irq = irq_of_parse_and_map(node, 0);

	if (ret) {
		dev_err(&pdev->dev, "failed to register acpm_ipc interrupt:%d\n", ret);
		return ret;
	}

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	ipc->intr = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(ipc->intr))
		return PTR_ERR(ipc->intr);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	ipc->sram_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
	if (IS_ERR(ipc->sram_base))
		return PTR_ERR(ipc->sram_base);

	prop = of_get_property(node, "initdata-base", &len);
	if (prop) {
		ipc->initdata_base = be32_to_cpup(prop);
	} else {
		dev_err(&pdev->dev, "Parsing initdata_base failed.\n");
		return -EINVAL;
	}

	ipc->initdata = (struct acpm_framework *)(ipc->sram_base + ipc->initdata_base);
	acpm_initdata = ipc->initdata;

	prop = of_get_property(node, "board-id", &len);
	if (prop) {
		acpm_initdata->board_info = be32_to_cpup(prop) & 0xff;
	} else {
		dev_err(&pdev->dev, "Parsing board-id failed.\n");
		return -EINVAL;
	}

	prop = of_get_property(node, "board-rev", &len);
	if (prop) {
		acpm_initdata->board_info |= ((be32_to_cpup(prop) & 0xff) << 8);
	} else {
		dev_err(&pdev->dev, "Parsing board-rev failed.\n");
		return -EINVAL;
	}

	dev_info(&pdev->dev, "board_info = 0x%x\n", acpm_initdata->board_info);

#if defined(CONFIG_SOC_S5E8835)
	prop = of_get_property(node, "nfc-log-offset", &len);
	if (prop) {
		acpm_nfc_log_offset = be32_to_cpup(prop);
	}

	prop = of_get_property(node, "nfc-log-len", &len);
	if (prop) {
		acpm_nfc_log_len = be32_to_cpup(prop);
	}
#endif

	ipc->dev = &pdev->dev;

	platform_set_drvdata(pdev, ipc);

	/* Get interrupt mode channel info */
	len = of_property_count_u32_elems(node, "interrupt-ch");
	if (len > 0) {
		ch_buf = devm_kzalloc(ipc->dev, sizeof(u32) * len,
				GFP_KERNEL);

		if (of_property_read_u32_array(node, "interrupt-ch", ch_buf, len)) {
			ch_buf = NULL;
			dev_info(&pdev->dev, "interrupt channels empty\n");
		} else {
			dev_info(&pdev->dev, "interrupt channels buf %x, len %d\n", (u64)ch_buf, len);
		}
	} else {
		ch_buf = NULL;
		dev_info(&pdev->dev, "interrupt channels empty\n");
	}


	drvdata = of_device_get_match_data(&pdev->dev);

	ipc->is_mailbox_master = drvdata->is_mailbox_master;

	dev_info(&pdev->dev, "is_mailbox_master %d, is_apm %d, is_asm %d\n",
			     drvdata->is_mailbox_master,
			     drvdata->is_apm,
			     drvdata->is_asm);

	channel_init(ipc, ch_buf, len);

	if (drvdata->is_apm) {
		acpm_ipc = ipc;

		log_buffer_init(&pdev->dev, node);

		update_log_wq = alloc_workqueue("%s", __WQ_LEGACY | WQ_MEM_RECLAIM |
				WQ_UNBOUND | WQ_SYSFS, 1, "acpm_update_log");
		INIT_WORK(&acpm_debug->update_log_work, acpm_update_log);

		if (acpm_debug->period) {
			INIT_DELAYED_WORK(&acpm_debug->periodic_work, acpm_debug_logging);

			queue_delayed_work_on(0, update_log_wq, &acpm_debug->periodic_work,
					msecs_to_jiffies(acpm_debug->period));
		}

		register_die_notifier(&nb_die_block);
		atomic_notifier_chain_register(&panic_notifier_list, &nb_panic_block);
		acpm_debug->ipc_info = ipc;
		acpm_set_sram_addr(ipc->sram_base);

		task_profile_running = &(acpm_initdata->task_profile_running);
		task_info = (struct task_info*)(ipc->sram_base + acpm_initdata->task_info);

	} else {
		prop = of_get_property(node, "dump-base", &len);
		if (prop)
			dump_base = be32_to_cpup(prop);

		prop = of_get_property(node, "dump-size", &len);
		if (prop)
			dump_size = be32_to_cpup(prop);

		if (dump_base && dump_size)
			dbg_snapshot_add_bl_item_info("esca_sram", dump_base, dump_size);

		dump_base = dump_size = 0;

		prop = of_get_property(node, "dram-dump-base", &len);
		if (prop)
			dump_base = be32_to_cpup(prop);

		prop = of_get_property(node, "dram-dump-size", &len);
		if (prop)
			dump_size = be32_to_cpup(prop);

		if (dump_base && dump_size)
			dbg_snapshot_add_bl_item_info("esca_dram", dump_base, dump_size);

		esca_ipc = ipc;
	}

	ret = devm_request_threaded_irq(&pdev->dev, ipc->irq, acpm_ipc_irq_handler,
			acpm_ipc_irq_handler_thread,
			IRQF_ONESHOT,
			dev_name(&pdev->dev), ipc);

	dev_info(&pdev->dev, "acpm_ipc probe done.\n");

	return ret;
}
EXPORT_SYMBOL_GPL(acpm_ipc_probe);

int acpm_ipc_remove(struct platform_device *pdev)
{
	return 0;
}
EXPORT_SYMBOL_GPL(acpm_ipc_remove);