// SPDX-License-Identifier: GPL-2.0
/*
 * bmp280_hwmon.c - 同一颗 BMP280，这次写成内核 I2C 客户端驱动，通过 hwmon 子系统导出温度
 *
 * 对比 02_i2c_dev_bmp280（用户态）：
 *   用户态：程序自己 open /dev/i2c-1、自己解析，别的程序不知道有这个传感器
 *   内核态：驱动绑定到 i2c 设备上，温度出现在标准接口
 *           /sys/class/hwmon/hwmonN/temp1_input（毫摄氏度），`sensors`、Prometheus
 *           node_exporter、任何监控工具都能直接读 —— 这就是"驱动子系统"的价值。
 *
 * 主线内核已有完整的 BMP280 IIO 驱动（drivers/iio/pressure/bmp280-core.c），
 * 做实验前先确认它没有抢先绑定：lsmod | grep bmp280。本驱动只为演示链路。
 *
 * 两种实例化方式（二选一）：
 *   a) 运行时：echo demo_bmp280 0x76 | sudo tee /sys/bus/i2c/devices/i2c-1/new_device
 *   b) 设备树：bmp280-overlay.dts（compatible = "demo,bmp280"）
 * 然后：sudo insmod bmp280_hwmon.ko && cat /sys/class/hwmon/hwmon*/temp1_input
 *
 * 需要内核 ≥ 6.3（i2c_driver.probe 为单参数）。
 */
#include <linux/delay.h>
#include <linux/hwmon.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>

#define REG_CALIB     0x88
#define REG_ID        0xD0
#define REG_STATUS    0xF3
#define REG_CTRL_MEAS 0xF4
#define REG_TEMP      0xFA

struct bmp280_data {
	struct i2c_client *client;
	struct mutex lock;           /* 一次测量是"写 ctrl → 等 → 读"的多步序列，不能被打断 */
	u16 t1;
	s16 t2, t3;
};

static int bmp280_read_temp(struct bmp280_data *d, long *millicelsius)
{
	struct i2c_client *c = d->client;
	s32 adc, var1, var2, t_fine;
	u8 raw[3];
	int ret, i;

	mutex_lock(&d->lock);
	/* 强制模式，只开温度 x1 过采样，气压跳过（osrs_p = 0） */
	ret = i2c_smbus_write_byte_data(c, REG_CTRL_MEAS, (1 << 5) | 1);
	if (ret)
		goto out;
	for (i = 0;; i++) {
		usleep_range(5000, 6000);
		ret = i2c_smbus_read_byte_data(c, REG_STATUS);
		if (ret < 0)
			goto out;
		if (!(ret & 0x08))            /* measuring 位清零 = 转换完成 */
			break;
		if (i == 9) {
			ret = -ETIMEDOUT;
			goto out;
		}
	}
	ret = i2c_smbus_read_i2c_block_data(c, REG_TEMP, 3, raw);
	ret = ret == 3 ? 0 : (ret < 0 ? ret : -EIO);
out:
	mutex_unlock(&d->lock);
	if (ret)
		return ret;

	adc = (raw[0] << 12) | (raw[1] << 4) | (raw[2] >> 4);
	var1 = ((((adc >> 3) - ((s32)d->t1 << 1))) * d->t2) >> 11;
	var2 = (((((adc >> 4) - (s32)d->t1) * ((adc >> 4) - (s32)d->t1)) >> 12) * d->t3) >> 14;
	t_fine = var1 + var2;
	*millicelsius = ((t_fine * 5 + 128) >> 8) * 10;   /* 0.01°C → hwmon 规定的 m°C */
	return 0;
}

static umode_t bmp280_is_visible(const void *data, enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	return type == hwmon_temp && attr == hwmon_temp_input ? 0444 : 0;
}

static int bmp280_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			     u32 attr, int channel, long *val)
{
	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;
	return bmp280_read_temp(dev_get_drvdata(dev), val);
}

static const struct hwmon_ops bmp280_hwmon_ops = {
	.is_visible = bmp280_is_visible,
	.read = bmp280_hwmon_read,
};

static const struct hwmon_channel_info *const bmp280_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_chip_info bmp280_chip_info = {
	.ops = &bmp280_hwmon_ops,
	.info = bmp280_info,
};

static int bmp280_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct bmp280_data *d;
	struct device *hwmon;
	u8 cal[6];
	int id, ret;

	/* 第一件事：确认芯片身份。设备树/new_device 只是"声称"这里有个 BMP280 */
	id = i2c_smbus_read_byte_data(client, REG_ID);
	if (id < 0)
		return dev_err_probe(dev, id, "no response at 0x%02x\n", client->addr);
	if (id != 0x58 && id != 0x60)
		return dev_err_probe(dev, -ENODEV, "unexpected chip id 0x%02x\n", id);

	ret = i2c_smbus_read_i2c_block_data(client, REG_CALIB, sizeof(cal), cal);
	if (ret != sizeof(cal))
		return dev_err_probe(dev, ret < 0 ? ret : -EIO, "cannot read calibration\n");

	d = devm_kzalloc(dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return -ENOMEM;
	d->client = client;
	mutex_init(&d->lock);
	d->t1 = cal[0] | (cal[1] << 8);             /* 校准值是小端 */
	d->t2 = (s16)(cal[2] | (cal[3] << 8));
	d->t3 = (s16)(cal[4] | (cal[5] << 8));

	hwmon = devm_hwmon_device_register_with_info(dev, "bmp280demo", d, &bmp280_chip_info, NULL);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	dev_info(dev, "chip id 0x%02x, registered %s\n", id, dev_name(hwmon));
	return 0;
}

static const struct i2c_device_id bmp280_id[] = {
	{ "demo_bmp280" },           /* 对应 new_device 里写的名字 */
	{ }
};
MODULE_DEVICE_TABLE(i2c, bmp280_id);

static const struct of_device_id bmp280_of_match[] = {
	{ .compatible = "demo,bmp280" },
	{ }
};
MODULE_DEVICE_TABLE(of, bmp280_of_match);

static struct i2c_driver bmp280_driver = {
	.driver = {
		.name = "demo_bmp280",
		.of_match_table = bmp280_of_match,
	},
	.probe = bmp280_probe,
	.id_table = bmp280_id,
};
module_i2c_driver(bmp280_driver);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BMP280 temperature via hwmon - I2C client driver demo");
