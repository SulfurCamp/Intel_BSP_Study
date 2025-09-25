// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/atomic.h>
#include <linux/gpio/consumer.h>
#include <linux/gpio/driver.h>
#include <linux/gpio/machine.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/string.h>

static unsigned int led1_gpio = 17;
module_param(led1_gpio, uint, 0444);
MODULE_PARM_DESC(led1_gpio, "First LED GPIO (default: 17)");

static unsigned int led2_gpio = 19;
module_param(led2_gpio, uint, 0444);
MODULE_PARM_DESC(led2_gpio, "Second LED GPIO (default: 19)");

static unsigned int pwm_gpio = 18;
module_param(pwm_gpio, uint, 0444);
MODULE_PARM_DESC(pwm_gpio, "LED breathing GPIO (default: 18)");

static unsigned int button_gpio = 27;
module_param(button_gpio, uint, 0444);
MODULE_PARM_DESC(button_gpio, "Button GPIO (default: 27)");

static unsigned int toggle_period_ms = 1000;
module_param(toggle_period_ms, uint, 0644);
MODULE_PARM_DESC(toggle_period_ms, "Alternating LED period in ms (default: 1000)");

static unsigned int pwm_period_ns = 20000000;
module_param(pwm_period_ns, uint, 0644);
MODULE_PARM_DESC(pwm_period_ns, "Breathing PWM base period in nanoseconds (default: 20ms)");

static unsigned int pwm_resolution = 100;
module_param(pwm_resolution, uint, 0644);
MODULE_PARM_DESC(pwm_resolution, "Software PWM resolution (default: 100 steps)");

static unsigned int pwm_step = 2;
module_param(pwm_step, uint, 0644);
MODULE_PARM_DESC(pwm_step, "Breathing duty increment per update (default: 2 steps)");

static unsigned int pwm_step_time_ms = 40;
module_param(pwm_step_time_ms, uint, 0644);
MODULE_PARM_DESC(pwm_step_time_ms, "Breathing duty update interval in ms (default: 40)");

static char *chip_label = "pinctrl-bcm2711";
module_param(chip_label, charp, 0444);
MODULE_PARM_DESC(chip_label, "GPIO chip label (default: pinctrl-bcm2711)");

static struct gpio_device *gpio_dev;
static struct gpio_chip *gpio_chip;
static struct gpio_desc *led1_desc;
static struct gpio_desc *led2_desc;
static struct gpio_desc *pwm_led_desc;
static struct gpio_desc *button_desc;
static int button_irq;

static struct hrtimer toggle_timer;
static struct hrtimer pwm_tick_timer;
static struct hrtimer pwm_breathe_timer;

#define MODE_IDLE   -1
#define MODE_TOGGLE  0
#define MODE_PWM     1

static atomic_t run_state = ATOMIC_INIT(MODE_IDLE);

static bool led_toggle_state;
static unsigned int pwm_level;
static unsigned int pwm_counter;
static bool pwm_increasing = true;
static u64 pwm_tick_ns;

static int gpio_pwm_match_chip(struct gpio_chip *gc, const void *data)
{
    unsigned int gpio = *(const unsigned int *)data;

    if (gc->base < 0)
        return false;

    return gc->base <= gpio && gpio < gc->base + gc->ngpio;
}

static struct gpio_desc *gpio_pwm_request_line(unsigned int gpio,
                                               const char *label,
                                               enum gpiod_flags flags)
{
    struct gpio_desc *desc;

    if (!gpio_dev) {
        if (chip_label && chip_label[0])
            gpio_dev = gpio_device_find_by_label(chip_label);

        if (!gpio_dev)
            gpio_dev = gpio_device_find(&gpio, gpio_pwm_match_chip);

        if (!gpio_dev) {
            pr_err("gpio_pwm_breath: unable to locate gpiochip for GPIO %u\n",
                   gpio);
            return ERR_PTR(-ENODEV);
        }

        if (chip_label && chip_label[0] &&
            strcmp(gpio_device_get_label(gpio_dev), chip_label))
            pr_warn("gpio_pwm_breath: using chip '%s' instead of requested '%s'\n",
                    gpio_device_get_label(gpio_dev), chip_label);
    }

    if (!gpio_chip) {
        gpio_chip = gpio_device_get_chip(gpio_dev);
        if (!gpio_chip) {
            pr_err("gpio_pwm_breath: failed to obtain gpio_chip for GPIO %u\n",
                   gpio);
            return ERR_PTR(-ENODEV);
        }
    }

    desc = gpiochip_request_own_desc(gpio_chip, gpio, label, 0, flags);
    if (IS_ERR(desc))
        pr_err("gpio_pwm_breath: unable to request GPIO %u (%ld)\n",
               gpio, PTR_ERR(desc));

    return desc;
}

static enum hrtimer_restart toggle_timer_fn(struct hrtimer *timer)
{
    if (atomic_read(&run_state) != MODE_TOGGLE)
        return HRTIMER_NORESTART;

    led_toggle_state = !led_toggle_state;
    gpiod_set_value(led1_desc, led_toggle_state ? 0 : 1);
    gpiod_set_value(led2_desc, led_toggle_state ? 1 : 0);

    hrtimer_forward_now(timer, ms_to_ktime(toggle_period_ms));
    return HRTIMER_RESTART;
}

static enum hrtimer_restart pwm_tick_timer_fn(struct hrtimer *timer)
{
    if (atomic_read(&run_state) != MODE_PWM)
        return HRTIMER_NORESTART;

    pwm_counter++;
    if (pwm_counter >= pwm_resolution)
        pwm_counter = 0;

    gpiod_set_value(pwm_led_desc, pwm_counter < pwm_level);

    hrtimer_forward_now(timer, ns_to_ktime(pwm_tick_ns));
    return HRTIMER_RESTART;
}

static enum hrtimer_restart pwm_breathe_timer_fn(struct hrtimer *timer)
{
    if (atomic_read(&run_state) != MODE_PWM)
        return HRTIMER_NORESTART;

    if (pwm_increasing) {
        if (pwm_level + pwm_step >= pwm_resolution) {
            pwm_level = pwm_resolution;
            pwm_increasing = false;
        } else {
            pwm_level += pwm_step;
        }
    } else {
        if (pwm_level <= pwm_step) {
            pwm_level = 0;
            pwm_increasing = true;
        } else {
            pwm_level -= pwm_step;
        }
    }

    hrtimer_forward_now(timer, ms_to_ktime(pwm_step_time_ms));
    return HRTIMER_RESTART;
}

static void gpio_pwm_stop_toggle(void)
{
    atomic_set(&run_state, MODE_IDLE);
    hrtimer_cancel(&toggle_timer);
    gpiod_set_value(led1_desc, 0);
    gpiod_set_value(led2_desc, 0);
}

static void gpio_pwm_start_toggle(void)
{
    pwm_level = 0;
    pwm_counter = 0;
    pwm_increasing = true;
    led_toggle_state = false;

    gpiod_set_value(led1_desc, 1);
    gpiod_set_value(led2_desc, 0);
    gpiod_set_value(pwm_led_desc, 0);

    atomic_set(&run_state, MODE_TOGGLE);
    hrtimer_start(&toggle_timer, ms_to_ktime(toggle_period_ms),
                  HRTIMER_MODE_REL);
}

static void gpio_pwm_stop_pwm(void)
{
    atomic_set(&run_state, MODE_IDLE);
    hrtimer_cancel(&pwm_tick_timer);
    hrtimer_cancel(&pwm_breathe_timer);
    pwm_level = 0;
    pwm_counter = 0;
    gpiod_set_value(pwm_led_desc, 0);
}

static void gpio_pwm_start_pwm(void)
{
    pwm_level = 0;
    pwm_counter = 0;
    pwm_increasing = true;

    gpiod_set_value(led1_desc, 0);
    gpiod_set_value(led2_desc, 0);
    gpiod_set_value(pwm_led_desc, 0);

    atomic_set(&run_state, MODE_PWM);
    hrtimer_start(&pwm_tick_timer, ns_to_ktime(pwm_tick_ns),
                  HRTIMER_MODE_REL);
    hrtimer_start(&pwm_breathe_timer, ms_to_ktime(pwm_step_time_ms),
                  HRTIMER_MODE_REL);
}

static irqreturn_t button_irq_thread(int irq, void *data)
{
    int val;
    int mode;

    val = gpiod_get_value_cansleep(button_desc);
    
    if (val <= 0)
        return IRQ_HANDLED;

    mode = atomic_read(&run_state);
    if (mode == MODE_TOGGLE) {
        gpio_pwm_stop_toggle();
        gpio_pwm_start_pwm();
        pr_info("gpio_pwm_breath: PWM breathing started\n");
    } else if (mode == MODE_PWM) {
        gpio_pwm_stop_pwm();
        gpio_pwm_start_toggle();
        pr_info("gpio_pwm_breath: toggle animation resumed\n");
    } else {
        gpio_pwm_start_toggle();
        pr_info("gpio_pwm_breath: toggle animation started\n");
    }

    return IRQ_HANDLED;
}

static int gpio_pwm_breath_init(void)
{
    int ret;

    if (!toggle_period_ms)
        toggle_period_ms = 1;
    if (!pwm_period_ns)
        pwm_period_ns = 1;
    if (!pwm_resolution)
        pwm_resolution = 1;
    if (!pwm_step)
        pwm_step = 1;
    if (!pwm_step_time_ms)
        pwm_step_time_ms = 10;

    pwm_tick_ns = div_u64(pwm_period_ns, pwm_resolution);
    if (!pwm_tick_ns)
        pwm_tick_ns = 1000; /* 1us fall-back */

    led1_desc = gpio_pwm_request_line(led1_gpio, "led1", GPIOD_OUT_HIGH);
    if (IS_ERR(led1_desc))
        return PTR_ERR(led1_desc);

    led2_desc = gpio_pwm_request_line(led2_gpio, "led2", GPIOD_OUT_LOW);
    if (IS_ERR(led2_desc)) {
        ret = PTR_ERR(led2_desc);
        goto err_free_led1;
    }

    pwm_led_desc = gpio_pwm_request_line(pwm_gpio, "pwm-led", GPIOD_OUT_LOW);
    if (IS_ERR(pwm_led_desc)) {
        ret = PTR_ERR(pwm_led_desc);
        goto err_free_led2;
    }

    button_desc = gpio_pwm_request_line(button_gpio, "button", GPIOD_IN);
    if (IS_ERR(button_desc)) {
        ret = PTR_ERR(button_desc);
        goto err_free_pwm_led;
    }

    hrtimer_init(&toggle_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    toggle_timer.function = toggle_timer_fn;

    hrtimer_init(&pwm_tick_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pwm_tick_timer.function = pwm_tick_timer_fn;

    hrtimer_init(&pwm_breathe_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    pwm_breathe_timer.function = pwm_breathe_timer_fn;

    gpio_pwm_start_toggle();

    button_irq = gpiod_to_irq(button_desc);
    if (button_irq < 0) {
        ret = button_irq;
        goto err_stop_modes;
    }

    ret = request_threaded_irq(button_irq, NULL, button_irq_thread,
                               IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING |
                               IRQF_ONESHOT,
                               "gpio_pwm_breath", NULL);
    if (ret)
        goto err_stop_modes;

    pr_info("gpio_pwm_breath: started (LEDs %u/%u, PWM GPIO %u, button %u)\n",
            led1_gpio, led2_gpio, pwm_gpio, button_gpio);
    return 0;

err_stop_modes:
    gpio_pwm_stop_pwm();
    gpio_pwm_stop_toggle();
    gpiochip_free_own_desc(button_desc);
    button_desc = NULL;
err_free_pwm_led:
    gpiochip_free_own_desc(pwm_led_desc);
    pwm_led_desc = NULL;
err_free_led2:
    gpiochip_free_own_desc(led2_desc);
    led2_desc = NULL;
err_free_led1:
    gpiochip_free_own_desc(led1_desc);
    led1_desc = NULL;
    if (gpio_dev) {
        gpio_device_put(gpio_dev);
        gpio_dev = NULL;
        gpio_chip = NULL;
    }
    return ret;
}

static void gpio_pwm_breath_exit(void)
{
    gpio_pwm_stop_pwm();
    gpio_pwm_stop_toggle();

    if (button_desc) {
        free_irq(button_irq, NULL);
        gpiochip_free_own_desc(button_desc);
        button_desc = NULL;
    }

    if (pwm_led_desc) {
        gpiochip_free_own_desc(pwm_led_desc);
        pwm_led_desc = NULL;
    }

    if (led1_desc) {
        gpiod_set_value(led1_desc, 0);
        gpiochip_free_own_desc(led1_desc);
        led1_desc = NULL;
    }

    if (led2_desc) {
        gpiod_set_value(led2_desc, 0);
        gpiochip_free_own_desc(led2_desc);
        led2_desc = NULL;
    }

    if (gpio_dev) {
        gpio_device_put(gpio_dev);
        gpio_dev = NULL;
        gpio_chip = NULL;
    }

    atomic_set(&run_state, MODE_IDLE);

    pr_info("gpio_pwm_breath: module unloaded\n");
}

module_init(gpio_pwm_breath_init);
module_exit(gpio_pwm_breath_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Intel BSP practice");
MODULE_DESCRIPTION("GPIO dual LED toggle with software PWM breathing and button control");
