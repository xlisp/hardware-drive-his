// SPDX-License-Identifier: GPL-2.0
/*
 * pifocuser.c - 树莓派电动调焦器的内核驱动：STEP/DIR 步进电机 + 梯形加减速
 *
 * 为什么放在内核里：步进电机每一步都要一个 STEP 脉冲，脉冲间隔决定速度，间隔的抖动
 * 就是电机的抖动/丢步。用户态程序随时可能被调度走几毫秒；内核 hrtimer 在硬中断里触发，
 * 抖动在几微秒量级。协议、温度补偿、回差这类"慢逻辑"则留在用户态守护进程（../daemon）。
 *
 * 硬件：任何 STEP/DIR 接口的驱动器（TMC2209、A4988、DRV8825……），引脚由设备树给出：
 *   step-gpios / dir-gpios / enable-gpios（可选，TMC2209 的 EN 低有效，设备树里写 ACTIVE_LOW）
 * 方向反了不用改代码：把 dir-gpios 的 flag 改成 1（GPIO_ACTIVE_LOW）。
 *
 * 用户态接口（/sys/devices/platform/pi-focuser/）：
 *   position      rw  当前位置（步）。写入 = "同步"：只改计数不转电机（运动中 -EBUSY）
 *   target        rw  写入即开始运动，运动中写入会平滑改道（先减速、必要时反向）
 *   moving        ro  0/1，状态变化时 sysfs_notify，用户态可以 poll() 等待
 *   halt          wo  写 1 = 按当前加速度减速停下
 *   max_position  rw  行程上限（步），target 超出范围返回 -ERANGE
 *   max_speed     rw  最高速度（步/秒）
 *   accel         rw  加速度（步/秒²）
 *   hold          rw  1 = 停止后保持电机通电（有保持力矩，发热）；0 = 停止后断电
 *
 * 需要内核 ≥ 6.2；6.13 起 hrtimer 初始化接口改名，下面用版本宏兼容。
 */
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spinlock.h>
#include <linux/version.h>
#include <linux/workqueue.h>

#define STEP_PULSE_US    2           /* TMC2209 要求 STEP 高电平 >100 ns，A4988 >1 µs，取 2 µs */
#define DIR_SETUP_NS     (50 * 1000) /* 改变 DIR 后到下一个 STEP 的间隔，远大于手册要求的几十 ns */
#define ENABLE_SETTLE_NS (2 * NSEC_PER_MSEC) /* 使能驱动器后等它建立电流 */
#define SPEED_LIMIT      20000       /* 步/秒，防止写错参数把 CPU 打满 */

struct pifoc {
	struct device *dev;
	struct gpio_desc *step, *dir, *enable;
	struct hrtimer timer;
	struct work_struct notify_work;
	spinlock_t lock;             /* 保护下面所有字段：sysfs 写入和定时器中断会并发访问 */

	s32 position, target;
	u32 max_position;
	u32 max_speed, min_speed, accel;
	u32 speed;                   /* 当前速度（步/秒），空闲时为 0 */
	int dir_sign;                /* 当前 DIR 引脚对应的方向：+1 / -1 */
	bool moving, hold, powered;
};

static void pifoc_power(struct pifoc *f, bool on)
{
	if (f->enable)
		gpiod_set_value(f->enable, on);
	f->powered = on;
}

static void pifoc_set_dir(struct pifoc *f, int sign)
{
	f->dir_sign = sign;
	gpiod_set_value(f->dir, sign > 0);
}

/*
 * 每一步调用一次（硬中断上下文，不能睡眠）。
 * 梯形速度曲线：剩余步数 <= 当前速度下的刹车距离 v²/(2a) 就减速，否则加速到 max_speed。
 * 速度增量 dv = a·dt = a/v（dt = 1/v 是这一步的时间）。
 */
static enum hrtimer_restart pifoc_timer_fn(struct hrtimer *t)
{
	struct pifoc *f = container_of(t, struct pifoc, timer);
	unsigned long flags;
	u64 period, stop_dist;
	u32 left, dv;
	s32 delta;
	int want;

	spin_lock_irqsave(&f->lock, flags);
	delta = f->target - f->position;
	if (delta == 0) {
		f->moving = false;
		f->speed = 0;
		if (!f->hold)
			pifoc_power(f, false);
		spin_unlock_irqrestore(&f->lock, flags);
		schedule_work(&f->notify_work);   /* sysfs_notify 可能睡眠，交给工作队列 */
		return HRTIMER_NORESTART;
	}

	want = delta > 0 ? 1 : -1;
	if (want != f->dir_sign) {
		s32 next = f->position + f->dir_sign;

		if (f->speed <= f->min_speed || next < 0 || next > (s32)f->max_position) {
			/* 已经够慢（或到了行程端点）：换向，等 DIR 建立后再走 */
			pifoc_set_dir(f, want);
			f->speed = f->min_speed;
			period = DIR_SETUP_NS;
			goto rearm;
		}
		left = 0;                         /* 目标在身后：沿原方向继续减速 */
	} else {
		left = abs(delta);
	}

	stop_dist = (u64)f->speed * f->speed / (2 * f->accel);
	dv = max(f->accel / f->speed, 1U);
	if (left <= stop_dist)
		f->speed = max(f->speed - min(dv, f->speed), f->min_speed);
	else
		f->speed = min(f->speed + dv, f->max_speed);

	gpiod_set_value(f->step, 1);
	udelay(STEP_PULSE_US);
	gpiod_set_value(f->step, 0);
	f->position += f->dir_sign;
	period = div_u64(NSEC_PER_SEC, f->speed);

rearm:
	hrtimer_forward_now(t, ns_to_ktime(period));
	spin_unlock_irqrestore(&f->lock, flags);
	return HRTIMER_RESTART;
}

/* 调用者持有 f->lock */
static void pifoc_start(struct pifoc *f)
{
	u64 first;

	if (f->moving || f->target == f->position)
		return;
	f->moving = true;
	f->speed = f->min_speed;
	pifoc_set_dir(f, f->target > f->position ? 1 : -1);
	first = DIR_SETUP_NS;
	if (!f->powered) {
		pifoc_power(f, true);
		first = ENABLE_SETTLE_NS;
	}
	hrtimer_start(&f->timer, ns_to_ktime(first), HRTIMER_MODE_REL);
	schedule_work(&f->notify_work);
}

static void pifoc_notify(struct work_struct *w)
{
	struct pifoc *f = container_of(w, struct pifoc, notify_work);

	sysfs_notify(&f->dev->kobj, NULL, "moving");
}

/* ---------------- sysfs ---------------- */

#define PIFOC_SHOW(name, fmt)                                                        \
static ssize_t name##_show(struct device *dev, struct device_attribute *a, char *buf) \
{                                                                                    \
	struct pifoc *f = dev_get_drvdata(dev);                                      \
	return sysfs_emit(buf, fmt "\n", READ_ONCE(f->name));                        \
}

PIFOC_SHOW(position, "%d")
PIFOC_SHOW(target, "%d")
PIFOC_SHOW(moving, "%d")
PIFOC_SHOW(max_position, "%u")
PIFOC_SHOW(max_speed, "%u")
PIFOC_SHOW(accel, "%u")
PIFOC_SHOW(hold, "%d")

static ssize_t position_store(struct device *dev, struct device_attribute *a,
			      const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;
	s32 v;

	if (kstrtos32(buf, 0, &v))
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	if (f->moving)
		ret = -EBUSY;
	else if (v < 0 || v > (s32)f->max_position)
		ret = -ERANGE;
	else
		f->position = f->target = v;
	spin_unlock_irqrestore(&f->lock, flags);
	return ret ?: n;
}

static ssize_t target_store(struct device *dev, struct device_attribute *a,
			    const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;
	s32 v;

	if (kstrtos32(buf, 0, &v))
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	if (v < 0 || v > (s32)f->max_position) {
		ret = -ERANGE;
	} else {
		f->target = v;              /* 运动中改目标：定时器下一步就会按新目标规划 */
		pifoc_start(f);
	}
	spin_unlock_irqrestore(&f->lock, flags);
	return ret ?: n;
}

static ssize_t halt_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	s32 stop;

	spin_lock_irqsave(&f->lock, flags);
	if (f->moving) {
		/* 把目标设在"刹车距离"之外，定时器自然减速停下，不会猛停丢步 */
		stop = f->position + f->dir_sign *
		       (s32)div_u64((u64)f->speed * f->speed, 2 * f->accel);
		f->target = clamp_t(s32, stop, 0, f->max_position);
	}
	spin_unlock_irqrestore(&f->lock, flags);
	return n;
}

static ssize_t max_position_store(struct device *dev, struct device_attribute *a,
				  const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	int ret = 0;
	u32 v;

	if (kstrtou32(buf, 0, &v) || v > INT_MAX)
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	if (f->moving)
		ret = -EBUSY;
	else if (v < (u32)f->position)
		ret = -ERANGE;
	else
		f->max_position = v;
	spin_unlock_irqrestore(&f->lock, flags);
	return ret ?: n;
}

static ssize_t max_speed_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	u32 v;

	if (kstrtou32(buf, 0, &v) || v < f->min_speed || v > SPEED_LIMIT)
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	f->max_speed = v;
	spin_unlock_irqrestore(&f->lock, flags);
	return n;
}

static ssize_t accel_store(struct device *dev, struct device_attribute *a,
			   const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	u32 v;

	if (kstrtou32(buf, 0, &v) || v < 10 || v > 1000000)
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	f->accel = v;
	spin_unlock_irqrestore(&f->lock, flags);
	return n;
}

static ssize_t hold_store(struct device *dev, struct device_attribute *a,
			  const char *buf, size_t n)
{
	struct pifoc *f = dev_get_drvdata(dev);
	unsigned long flags;
	bool v;

	if (kstrtobool(buf, &v))
		return -EINVAL;
	spin_lock_irqsave(&f->lock, flags);
	f->hold = v;
	if (!f->moving)
		pifoc_power(f, v);
	spin_unlock_irqrestore(&f->lock, flags);
	return n;
}

static DEVICE_ATTR_RW(position);
static DEVICE_ATTR_RW(target);
static DEVICE_ATTR_RO(moving);
static DEVICE_ATTR_WO(halt);
static DEVICE_ATTR_RW(max_position);
static DEVICE_ATTR_RW(max_speed);
static DEVICE_ATTR_RW(accel);
static DEVICE_ATTR_RW(hold);

static struct attribute *pifoc_attrs[] = {
	&dev_attr_position.attr, &dev_attr_target.attr, &dev_attr_moving.attr,
	&dev_attr_halt.attr, &dev_attr_max_position.attr, &dev_attr_max_speed.attr,
	&dev_attr_accel.attr, &dev_attr_hold.attr, NULL,
};
ATTRIBUTE_GROUPS(pifoc);

/* ---------------- probe ---------------- */

static void pifoc_teardown(void *data)
{
	struct pifoc *f = data;

	hrtimer_cancel(&f->timer);        /* 等正在执行的回调结束，之后不会再触发 */
	cancel_work_sync(&f->notify_work);
	pifoc_power(f, false);
}

static u32 prop_u32(struct device *dev, const char *name, u32 def)
{
	u32 v;

	return device_property_read_u32(dev, name, &v) ? def : v;
}

static int pifoc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pifoc *f;

	f = devm_kzalloc(dev, sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	f->dev = dev;
	spin_lock_init(&f->lock);
	INIT_WORK(&f->notify_work, pifoc_notify);

	f->step = devm_gpiod_get(dev, "step", GPIOD_OUT_LOW);
	if (IS_ERR(f->step))
		return dev_err_probe(dev, PTR_ERR(f->step), "step-gpios\n");
	f->dir = devm_gpiod_get(dev, "dir", GPIOD_OUT_LOW);
	if (IS_ERR(f->dir))
		return dev_err_probe(dev, PTR_ERR(f->dir), "dir-gpios\n");
	f->enable = devm_gpiod_get_optional(dev, "enable", GPIOD_OUT_LOW);
	if (IS_ERR(f->enable))
		return dev_err_probe(dev, PTR_ERR(f->enable), "enable-gpios\n");

	/* 定时器回调在硬中断里，GPIO 必须是不睡眠的（SoC 直连的 GPIO 都是；I2C 扩展芯片上的不是） */
	if (gpiod_cansleep(f->step) || gpiod_cansleep(f->dir) ||
	    (f->enable && gpiod_cansleep(f->enable)))
		return dev_err_probe(dev, -EINVAL, "step/dir/enable must be SoC GPIOs (non-sleeping)\n");

	f->max_position = prop_u32(dev, "max-position", 50000);
	f->max_speed = clamp(prop_u32(dev, "max-speed", 800), 1U, (u32)SPEED_LIMIT);
	f->min_speed = clamp(prop_u32(dev, "start-speed", 100), 1U, f->max_speed);
	f->accel = clamp(prop_u32(dev, "acceleration", 2000), 10U, 1000000U);
	f->hold = device_property_read_bool(dev, "hold-current");
	f->dir_sign = 1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&f->timer, pifoc_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&f->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	f->timer.function = pifoc_timer_fn;
#endif
	platform_set_drvdata(pdev, f);
	pifoc_power(f, f->hold);

	dev_info(dev, "focuser ready: max %u steps, %u..%u steps/s, accel %u, hold %d\n",
		 f->max_position, f->min_speed, f->max_speed, f->accel, f->hold);
	return devm_add_action_or_reset(dev, pifoc_teardown, f);
}

static const struct of_device_id pifoc_of_match[] = {
	{ .compatible = "demo,pi-focuser" },
	{ }
};
MODULE_DEVICE_TABLE(of, pifoc_of_match);

static struct platform_driver pifoc_driver = {
	.probe = pifoc_probe,
	.driver = {
		.name = "pi-focuser",
		.of_match_table = pifoc_of_match,
		.dev_groups = pifoc_groups,
	},
};
module_platform_driver(pifoc_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Raspberry Pi stepper focuser (STEP/DIR, trapezoidal ramp, sysfs)");
