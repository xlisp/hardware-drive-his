/*
 * gpio_libgpiod.c - 用 GPIO 字符设备（/dev/gpiochipN）+ libgpiod v2 闪烁 LED
 *
 * 这是当前 Linux 推荐的用户态 GPIO 方式：
 *   - /sys/class/gpio（sysfs）从内核 4.8 起已废弃，Pi 5 上全局编号也变了；
 *   - 字符设备 ABI v2（内核 5.10+）支持一次申请多根线、边沿事件带时间戳、去抖、偏置；
 *   - 进程退出时内核自动释放线，不会像 sysfs 那样留下"export 了没 unexport"的残留。
 *
 * 安装：sudo apt install libgpiod-dev gpiod      （Pi OS Trixie 起为 libgpiod 2.x）
 * 编译：gcc -O2 -Wall -o gpio_libgpiod gpio_libgpiod.c -lgpiod
 * 运行：./gpio_libgpiod [/dev/gpiochip0] [17]
 * 命令行等价物：gpioset -c gpiochip0 17=1 ; gpioinfo 查看谁占用了哪根线
 *
 * Pi 5 注意：40 针排针在 RP1 南桥上。早期内核里它是 gpiochip4，2024 年下半年起的内核改成了 gpiochip0。
 *            用 `gpiodetect` 找标签为 pinctrl-rp1 的那个。
 */
#include <gpiod.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *chip_path = argc > 1 ? argv[1] : "/dev/gpiochip0";
    unsigned int offset = argc > 2 ? (unsigned int)atoi(argv[2]) : 17;
    int ret = 1;

    struct gpiod_chip *chip = gpiod_chip_open(chip_path);
    if (!chip) { perror(chip_path); return 1; }

    struct gpiod_line_settings *settings = gpiod_line_settings_new();
    struct gpiod_line_config *line_cfg = gpiod_line_config_new();
    struct gpiod_request_config *req_cfg = gpiod_request_config_new();
    struct gpiod_line_request *req = NULL;
    if (!settings || !line_cfg || !req_cfg) goto out;

    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
    gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_INACTIVE);
    if (gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings)) goto out;
    gpiod_request_config_set_consumer(req_cfg, "blink-demo");   /* gpioinfo 里会显示这个名字 */

    req = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
    if (!req) { perror("request lines (line busy? check gpioinfo)"); goto out; }

    for (int i = 0; i < 20; i++) {
        gpiod_line_request_set_value(req, offset,
                                     i & 1 ? GPIOD_LINE_VALUE_INACTIVE : GPIOD_LINE_VALUE_ACTIVE);
        usleep(250 * 1000);
    }
    ret = 0;

out:
    if (req) gpiod_line_request_release(req);
    gpiod_request_config_free(req_cfg);
    gpiod_line_config_free(line_cfg);
    gpiod_line_settings_free(settings);
    gpiod_chip_close(chip);
    return ret;
}
