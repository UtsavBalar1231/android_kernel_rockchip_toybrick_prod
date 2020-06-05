/*
 *  Copyright (C) 2010, Lars-Peter Clausen <lars@metafoo.de>
 *  Driver for chargers which report their online status through a GPIO pin
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under  the terms of the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include <linux/device.h>
#include <linux/gpio.h> /* For legacy platform data */
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/gpio/consumer.h>

#include <linux/power/gpio-charger.h>

struct gpio_charger {
	const struct gpio_charger_platform_data *pdata;
	unsigned int irq;
	unsigned int charge_status_irq;
	bool wakeup_enabled;

	struct power_supply *charger;
	struct power_supply_desc charger_desc;
	struct gpio_desc *gpiod;
	struct gpio_desc *charge_status;
};

static irqreturn_t gpio_charger_irq(int irq, void *devid)
{
	struct power_supply *charger = devid;

	power_supply_changed(charger);

	return IRQ_HANDLED;
}

static inline struct gpio_charger *psy_to_gpio_charger(struct power_supply *psy)
{
	return power_supply_get_drvdata(psy);
}

static int gpio_charger_get_property(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val)
{
	struct gpio_charger *gpio_charger = psy_to_gpio_charger(psy);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = gpiod_get_value_cansleep(gpio_charger->gpiod);
		break;
	case POWER_SUPPLY_PROP_STATUS:
		if (gpiod_get_value_cansleep(gpio_charger->charge_status))
			val->intval = POWER_SUPPLY_STATUS_CHARGING;
		else
			val->intval = POWER_SUPPLY_STATUS_NOT_CHARGING;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int gpio_charger_get_irq(struct device *dev, void *dev_id,
				struct gpio_desc *gpio)
{
	int ret, irq = gpiod_to_irq(gpio);

	if (irq > 0) {
		ret = devm_request_any_context_irq(dev, irq, gpio_charger_irq,
						   IRQF_TRIGGER_RISING |
						   IRQF_TRIGGER_FALLING,
						   dev_name(dev),
						   dev_id);
		if (ret < 0) {
			dev_warn(dev, "Failed to request irq: %d\n", ret);
			irq = 0;
		}
	}

	return irq;
}

/*
 * The entries will be overwritten by driver's probe routine depending
 * on the available features. This list ensures, that the array is big
 * enough for all optional features.
 */
static enum power_supply_property gpio_charger_properties[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_STATUS,
};

static
struct gpio_charger_platform_data *gpio_charger_parse_dt(struct device *dev)
{
	struct device_node *np = dev->of_node;
	struct gpio_charger_platform_data *pdata;
	const char *chargetype;
	int ret;

	if (!np)
		return ERR_PTR(-ENOENT);

	pdata = devm_kzalloc(dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return ERR_PTR(-ENOMEM);

	pdata->name = np->name;
	pdata->type = POWER_SUPPLY_TYPE_UNKNOWN;
	ret = of_property_read_string(np, "charger-type", &chargetype);
	if (ret >= 0) {
		if (!strncmp("unknown", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_UNKNOWN;
		else if (!strncmp("battery", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_BATTERY;
		else if (!strncmp("ups", chargetype, 3))
			pdata->type = POWER_SUPPLY_TYPE_UPS;
		else if (!strncmp("mains", chargetype, 5))
			pdata->type = POWER_SUPPLY_TYPE_MAINS;
		else if (!strncmp("usb-sdp", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_USB;
		else if (!strncmp("usb-dcp", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_USB_DCP;
		else if (!strncmp("usb-cdp", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_USB_CDP;
		else if (!strncmp("usb-aca", chargetype, 7))
			pdata->type = POWER_SUPPLY_TYPE_USB_ACA;
		else
			dev_warn(dev, "unknown charger type %s\n", chargetype);
	}

	return pdata;
}

static int gpio_charger_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct gpio_charger_platform_data *pdata = dev->platform_data;
	struct power_supply_config psy_cfg = {};
	struct gpio_charger *gpio_charger;
	struct power_supply_desc *charger_desc;
	struct gpio_desc *charge_status;
	int charge_status_irq;
	unsigned long flags;
	int ret;
	int num_props = 0;

	if (!pdata) {
		pdata = gpio_charger_parse_dt(dev);
		if (IS_ERR(pdata)) {
			ret = PTR_ERR(pdata);
			if (ret != -EPROBE_DEFER)
				dev_err(dev, "No platform data\n");
			return ret;
		}
	}

	gpio_charger = devm_kzalloc(dev, sizeof(*gpio_charger), GFP_KERNEL);
	if (!gpio_charger)
		return -ENOMEM;

	/*
	 * This will fetch a GPIO descriptor from device tree, ACPI or
	 * boardfile descriptor tables. It's good to try this first.
	 */
	gpio_charger->gpiod = devm_gpiod_get_optional(dev, NULL, GPIOD_IN);

	/*
	 * Fallback to legacy platform data method, if no GPIO is specified
	 * using boardfile descriptor tables.
	 */
	if (!gpio_charger->gpiod && pdata) {
		/* Non-DT: use legacy GPIO numbers */
		if (!gpio_is_valid(pdata->gpio)) {
			dev_err(dev, "Invalid gpio pin in pdata\n");
			return -EINVAL;
		}
		flags = GPIOF_IN;
		if (pdata->gpio_active_low)
			flags |= GPIOF_ACTIVE_LOW;
		ret = devm_gpio_request_one(dev, pdata->gpio, flags,
					    dev_name(dev));
		if (ret) {
			dev_err(dev, "Failed to request gpio pin: %d\n", ret);
			return ret;
		}
		/* Then convert this to gpiod for now */
		gpio_charger->gpiod = gpio_to_desc(pdata->gpio);
	} else if (IS_ERR(gpio_charger->gpiod)) {
		/* Just try again if this happens */
		if (PTR_ERR(gpio_charger->gpiod) == -EPROBE_DEFER)
			return -EPROBE_DEFER;
		dev_err(dev, "error getting GPIO descriptor\n");
		return PTR_ERR(gpio_charger->gpiod);
	}

	if (gpio_charger->gpiod) {
		gpio_charger_properties[num_props] = POWER_SUPPLY_PROP_ONLINE;
		num_props++;
	}

	charge_status = devm_gpiod_get_optional(dev, "charge-status", GPIOD_IN);
	if (IS_ERR(charge_status))
		return PTR_ERR(charge_status);
	if (charge_status) {
		gpio_charger->charge_status = charge_status;
		gpio_charger_properties[num_props] = POWER_SUPPLY_PROP_STATUS;
		num_props++;
	}

	charger_desc = &gpio_charger->charger_desc;

	charger_desc->name = pdata->name ? pdata->name : "gpio-charger";
	charger_desc->type = pdata->type;
	charger_desc->properties = gpio_charger_properties;
	charger_desc->num_properties = num_props;
	charger_desc->get_property = gpio_charger_get_property;

	psy_cfg.supplied_to = pdata->supplied_to;
	psy_cfg.num_supplicants = pdata->num_supplicants;
	psy_cfg.of_node = dev->of_node;
	psy_cfg.drv_data = gpio_charger;

	gpio_charger->charger = devm_power_supply_register(dev, charger_desc,
							   &psy_cfg);
	if (IS_ERR(gpio_charger->charger)) {
		ret = PTR_ERR(gpio_charger->charger);
		dev_err(dev, "Failed to register power supply: %d\n", ret);
		return ret;
	}

	gpio_charger->irq = gpio_charger_get_irq(dev, gpio_charger->charger,
						 gpio_charger->gpiod);

	charge_status_irq = gpio_charger_get_irq(dev, gpio_charger->charger,
						 gpio_charger->charge_status);
	gpio_charger->charge_status_irq = charge_status_irq;

	platform_set_drvdata(pdev, gpio_charger);

	device_init_wakeup(dev, 1);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int gpio_charger_suspend(struct device *dev)
{
	struct gpio_charger *gpio_charger = dev_get_drvdata(dev);

	if (device_may_wakeup(dev))
		gpio_charger->wakeup_enabled =
			!enable_irq_wake(gpio_charger->irq);

	return 0;
}

static int gpio_charger_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct gpio_charger *gpio_charger = platform_get_drvdata(pdev);

	if (device_may_wakeup(dev) && gpio_charger->wakeup_enabled)
		disable_irq_wake(gpio_charger->irq);
	power_supply_changed(gpio_charger->charger);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(gpio_charger_pm_ops,
		gpio_charger_suspend, gpio_charger_resume);

static const struct of_device_id gpio_charger_match[] = {
	{ .compatible = "gpio-charger" },
	{ }
};
MODULE_DEVICE_TABLE(of, gpio_charger_match);

static struct platform_driver gpio_charger_driver = {
	.probe = gpio_charger_probe,
	.driver = {
		.name = "gpio-charger",
		.pm = &gpio_charger_pm_ops,
		.of_match_table = gpio_charger_match,
	},
};

module_platform_driver(gpio_charger_driver);

MODULE_AUTHOR("Lars-Peter Clausen <lars@metafoo.de>");
MODULE_DESCRIPTION("Driver for chargers only communicating via GPIO(s)");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:gpio-charger");
