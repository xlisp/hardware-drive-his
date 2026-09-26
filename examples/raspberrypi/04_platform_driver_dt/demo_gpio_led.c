// SPDX-License-Identifier: GPL-2.0
/*
 * demo_gpio_led.c - platform 驱动 + 设备树：驱动不写死引脚，引脚由设备树告诉它
 *
 * Linux 设备模型的核心思想（2.6 内核引入，ARM 在 2011 年后全面转向设备树）：
 *   设备（device）  = 硬件"在哪、接了什么"，由设备树描述（demo-led-overlay.dts）
 *   驱动（driver）  = "怎么操作这类硬件"，靠 compatible 字符串和设备匹配
 *   总线（bus）     = 负责匹配和调用 probe()。这里是 platform 总线（SoC 上不可枚举的设备）
 * 换一块板子、换一根 GPIO，只改设备树，驱动一行不改 —— 这就是 Pi 的 dtoverlay 机制。
 *
 * 本驱动：
 *   probe()  从设备树拿到 "led-gpios"，申请为输出
 *   sysfs    /sys/devices/platform/demo-led/state   读写 0/1
 *            /sys/devices/platform/demo-led/blink_ms 非 0 时由内核定时器闪烁
 *   全部资源用 devm_* 申请，设备解绑时自动释放，所以不需要 remove()
 *
 * 需要内核 ≥ 6.2（timer_delete_sync）。Pi OS 当前内核是 6.6/6.12，满足。
 *
 * 真实项目里 LED 应该用内核现成的 leds-gpio 驱动（compatible = "gpio-leds"），
 * 这里自己写是为了看清 probe/设备树/sysfs 的完整链路。
 */
#include <linux/device.h>
#include <linux/gpio/consumer.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/timer.h>

struct demo_led {
	struct gpio_desc *gpio;
	struct timer_list timer;
	unsigned int blink_ms;
	bool on;
};

static void demo_led_timer(struct timer_list *t)
{
	struct demo_led *led = container_of(t, struct demo_led, timer);

	led->on = !led->on;
	gpiod_set_value(led->gpio, led->on);   /* 定时器回调在软中断上下文，不能用 _cansleep 版本 */
	if (led->blink_ms)
		mod_timer(&led->timer, jiffies + msecs_to_jiffies(led->blink_ms));
}

static ssize_t state_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct demo_led *led = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%d\n", led->on);
}

static ssize_t state_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct demo_led *led = dev_get_drvdata(dev);
	bool on;
	int ret = kstrtobool(buf, &on);

	if (ret)
		return ret;
	led->blink_ms = 0;
	timer_delete_sync(&led->timer);
	led->on = on;
	gpiod_set_value(led->gpio, on);
	return count;
}
static DEVICE_ATTR_RW(state);

static ssize_t blink_ms_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct demo_led *led = dev_get_drvdata(dev);

	return sysfs_emit(buf, "%u\n", led->blink_ms);
}

static ssize_t blink_ms_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct demo_led *led = dev_get_drvdata(dev);
	unsigned int ms;
	int ret = kstrtouint(buf, 0, &ms);

	if (ret)
		return ret;
	if (ms && ms < 10)
		return -EINVAL;
	led->blink_ms = ms;
	if (ms)
		mod_timer(&led->timer, jiffies + msecs_to_jiffies(ms));
	else
		timer_delete_sync(&led->timer);
	return count;
}
static DEVICE_ATTR_RW(blink_ms);

static struct attribute *demo_led_attrs[] = {
	&dev_attr_state.attr,
	&dev_attr_blink_ms.attr,
	NULL,
};
ATTRIBUTE_GROUPS(demo_led);

static void demo_led_stop(void *data)
{
	struct demo_led *led = data;

	led->blink_ms = 0;
	timer_delete_sync(&led->timer);
}

static int demo_led_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct demo_led *led;

	led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	/* 对应设备树里的 led-gpios = <&gpio 17 GPIO_ACTIVE_HIGH>; 极性由设备树决定，驱动只管逻辑值 */
	led->gpio = devm_gpiod_get(dev, "led", GPIOD_OUT_LOW);
	if (IS_ERR(led->gpio))
		return dev_err_probe(dev, PTR_ERR(led->gpio), "cannot get led-gpios\n");

	timer_setup(&led->timer, demo_led_timer, 0);
	platform_set_drvdata(pdev, led);
	dev_info(dev, "demo led ready (gpio desc %d)\n", desc_to_gpio(led->gpio));
	/* 解绑时先停定时器，再由 devm 释放 GPIO 和内存（devm 按申请的逆序释放） */
	return devm_add_action_or_reset(dev, demo_led_stop, led);
}

static const struct of_device_id demo_led_of_match[] = {
	{ .compatible = "demo,gpio-led" },
	{ }
};
MODULE_DEVICE_TABLE(of, demo_led_of_match);   /* 让 udev/modprobe 能按 compatible 自动加载模块 */

static struct platform_driver demo_led_driver = {
	.probe = demo_led_probe,
	.driver = {
		.name = "demo-gpio-led",
		.of_match_table = demo_led_of_match,
		.dev_groups = demo_led_groups,      /* sysfs 属性随设备一起创建，没有竞态 */
	},
};
module_platform_driver(demo_led_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Platform driver + device tree demo: GPIO LED with sysfs");
