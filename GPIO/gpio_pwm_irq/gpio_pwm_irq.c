// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/string.h>

static unsigned int led_gpio = 17;
module_param(led_gpio, uint, 0444);
MODULE_PARM_DESC(led_gpio, "BCM numbering for the LED line (default: 17)");

static unsigned int button_gpio = 27;
module_param(button_gpio, uint, 0444);
MODULE_PARM_DESC(button_gpio, "BCM numbering for the button line (default: 27)");

static unsigned int period_ms = 1000;
module_param(period_ms, uint, 0644);
MODULE_PARM_DESC(period_ms, "Blink period in milliseconds (default: 1000)");

static char *chip_label = "pinctrl-bcm2711";
module_param(chip_label, charp, 0444);
MODULE_PARM_DESC(chip_label, "GPIO chip label providing the lines (default: pinctrl-bcm2711)");

static struct gpio_device *gpio_dev;
static struct gpio_chip *gpio_chip;
static struct gpio_desc *led_desc;
static struct gpio_desc *button_desc;
static int button_irq;
static struct hrtimer blink_timer;
static atomic_t blink_enabled = ATOMIC_INIT(0);
static bool led_state;

static int gpio_pwm_irq_match_chip(struct gpio_chip *gc, const void *data)
{
    unsigned int gpio = *(const unsigned int *)data;

    if (gc->base < 0)
        return false;

    return gc->base <= gpio && gpio < gc->base + gc->ngpio;
}

static enum hrtimer_restart gpio_pwm_irq_timer_fn(struct hrtimer *timer)
{
    if (!atomic_read(&blink_enabled))
        return HRTIMER_NORESTART;

    led_state = !led_state;
    gpiod_set_value(led_desc, led_state);
    hrtimer_forward_now(timer, ms_to_ktime(period_ms));

    return HRTIMER_RESTART;
}

static irqreturn_t gpio_pwm_irq_button_thread(int irq, void *data)
{
    if (atomic_xchg(&blink_enabled, 0)) {
        hrtimer_cancel(&blink_timer);
        led_state = false;
        gpiod_set_value(led_desc, led_state);
        pr_info("gpio_pwm_irq: blinking stopped by button\n");
    } else {
        led_state = true;
        gpiod_set_value(led_desc, led_state);
        atomic_set(&blink_enabled, 1);
        hrtimer_start(&blink_timer, ms_to_ktime(period_ms), HRTIMER_MODE_REL);
        pr_info("gpio_pwm_irq: blinking resumed by button\n");
    }

    return IRQ_HANDLED;
}

static int gpio_pwm_irq_request_gpio(unsigned int gpio, const char *label,
                                     enum gpiod_flags flags,
                                     struct gpio_desc **out_desc)
{
    struct gpio_desc *desc;

    if (!gpio_dev) {
        if (chip_label && chip_label[0])
            gpio_dev = gpio_device_find_by_label(chip_label);

        if (!gpio_dev) {
            gpio_dev = gpio_device_find(&gpio, gpio_pwm_irq_match_chip);
            if (!gpio_dev) {
                pr_err("gpio_pwm_irq: no chip contains GPIO %u\n", gpio);
                return -ENODEV;
            }
        }

        if (chip_label && chip_label[0] &&
            strcmp(gpio_device_get_label(gpio_dev), chip_label))
            pr_warn("gpio_pwm_irq: using chip '%s' instead of requested '%s'\n",
                    gpio_device_get_label(gpio_dev), chip_label);
    }

    if (!gpio_chip) {
        gpio_chip = gpio_device_get_chip(gpio_dev);
        if (!gpio_chip) {
            pr_err("gpio_pwm_irq: failed to obtain gpio_chip from '%s'\n",
                   chip_label);
            return -ENODEV;
        }
    }

    desc = gpiochip_request_own_desc(gpio_chip, gpio, label, 0, flags);
    if (IS_ERR(desc)) {
        pr_err("gpio_pwm_irq: line %u request failed (%ld)\n",
               gpio, PTR_ERR(desc));
        return PTR_ERR(desc);
    }

    *out_desc = desc;
    return 0;
}

static int __init gpio_pwm_irq_init(void)
{
    int ret;

    if (!period_ms)
        period_ms = 1;

    ret = gpio_pwm_irq_request_gpio(led_gpio, "gpio17-led", GPIOD_OUT_HIGH,
                                    &led_desc);
    if (ret)
        return ret;

    led_state = true;
    gpiod_set_value(led_desc, led_state);

    ret = gpio_pwm_irq_request_gpio(button_gpio, "gpio27-button", GPIOD_IN,
                                    &button_desc);
    if (ret)
        goto err_release_led;

    button_irq = gpiod_to_irq(button_desc);
    if (button_irq < 0) {
        ret = button_irq;
        goto err_release_button;
    }

    hrtimer_init(&blink_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    blink_timer.function = gpio_pwm_irq_timer_fn;
    led_state = true;
    atomic_set(&blink_enabled, 1);
    hrtimer_start(&blink_timer, ms_to_ktime(period_ms), HRTIMER_MODE_REL);

    ret = request_threaded_irq(button_irq, NULL, gpio_pwm_irq_button_thread,
                               IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING |
                               IRQF_ONESHOT,
                               "gpio_pwm_irq", NULL);
    if (ret) {
        atomic_set(&blink_enabled, 0);
        hrtimer_cancel(&blink_timer);
        goto err_release_button;
    }

    pr_info("gpio_pwm_irq: LED blinking started (GPIO%u, period %ums)\n",
            led_gpio, period_ms);
    pr_info("gpio_pwm_irq: button on GPIO%u to stop blinking\n",
            button_gpio);

    return 0;

err_release_button:
    gpiochip_free_own_desc(button_desc);
    button_desc = NULL;
err_release_led:
    led_state = false;
    gpiod_set_value(led_desc, led_state);
    gpiochip_free_own_desc(led_desc);
    led_desc = NULL;
    if (gpio_dev) {
        gpio_device_put(gpio_dev);
        gpio_dev = NULL;
        gpio_chip = NULL;
    }
    return ret;
}

static void __exit gpio_pwm_irq_exit(void)
{
    atomic_set(&blink_enabled, 0);
    hrtimer_cancel(&blink_timer);
    if (button_desc)
        free_irq(button_irq, NULL);
    led_state = false;
    if (led_desc)
        gpiod_set_value(led_desc, led_state);
    if (button_desc) {
        gpiochip_free_own_desc(button_desc);
        button_desc = NULL;
    }
    if (led_desc) {
        gpiochip_free_own_desc(led_desc);
        led_desc = NULL;
    }
    if (gpio_dev) {
        gpio_device_put(gpio_dev);
        gpio_dev = NULL;
        gpio_chip = NULL;
    }
    pr_info("gpio_pwm_irq: module unloaded\n");
}

module_init(gpio_pwm_irq_init);
module_exit(gpio_pwm_irq_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel BSP practice");
MODULE_DESCRIPTION("GPIO LED blink with button stop for Raspberry Pi");
