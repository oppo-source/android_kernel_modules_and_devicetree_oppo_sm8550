// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020-2022 Oplus. All rights reserved.
 */

#define pr_fmt(fmt) "[HL7227]([%s][%d]): " fmt, __func__, __LINE__

#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/gpio.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/bitops.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/list.h>
#include <oplus_chg_module.h>
#include <oplus_chg_comm.h>
#include <oplus_mms.h>

#ifndef I2C_ERR_MAX
#define I2C_ERR_MAX 2
#endif

struct oplus_hl7227 {
	struct i2c_client *client;
	struct device *dev;
	struct oplus_chg_ic_dev *ic_dev;

	int cp_en_gpio;
	int cp_int_gpio;
	int cp_int_irq;

	struct mutex i2c_lock;

	struct pinctrl *pinctrl;
	struct pinctrl_state *cp_int_default;
	struct pinctrl_state *cp_en_active;
	struct pinctrl_state *cp_en_sleep;

	struct regmap *regmap;

	atomic_t suspended;
	atomic_t i2c_err_count;
};

static struct regmap_config hl7227_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0x10,
};

static __inline__ void hl7227_i2c_err_inc(struct oplus_hl7227 *chip)
{
	if (atomic_inc_return(&chip->i2c_err_count) > I2C_ERR_MAX) {
		atomic_set(&chip->i2c_err_count, 0);
		chg_debug("i2c_err_count > I2C_ERR_MAX\n");
		/*todo, add i2c error callback*/
	}
}

static __inline__ void hl7227_i2c_err_clr(struct oplus_hl7227 *chip)
{
	atomic_set(&chip->i2c_err_count, 0);
}

static int hl7227_read_byte(struct oplus_hl7227 *chip, u8 addr, u8 *data)
{
	int rc;

	mutex_lock(&chip->i2c_lock);
	rc = i2c_master_send(chip->client, &addr, 1);
	if (rc < 1) {
		chg_err("read 0x%04x error, rc=%d\n", addr, rc);
		rc = rc < 0 ? rc : -EIO;
		goto error;
	}

	rc = i2c_master_recv(chip->client, data, 1);
	if (rc < 1) {
		chg_err("read 0x%04x error, rc=%d\n", addr, rc);
		rc = rc < 0 ? rc : -EIO;
		goto error;
	}

	mutex_unlock(&chip->i2c_lock);
	hl7227_i2c_err_clr(chip);
	return 0;

error:
	mutex_unlock(&chip->i2c_lock);
	hl7227_i2c_err_inc(chip);
	return rc;
}

__maybe_unused static int hl7227_read_data(struct oplus_hl7227 *chip, u8 addr, u8 *buf, int len)
{
	int rc;

	mutex_lock(&chip->i2c_lock);
	rc = i2c_master_send(chip->client, &addr, 1);
	if (rc < 1) {
		chg_err("read 0x%04x error, rc=%d\n", addr, rc);
		rc = rc < 0 ? rc : -EIO;
		goto error;
	}

	rc = i2c_master_recv(chip->client, buf, len);
	if (rc < len) {
		chg_err("read 0x%04x error, rc=%d\n", addr, rc);
		rc = rc < 0 ? rc : -EIO;
		goto error;
	}

	mutex_unlock(&chip->i2c_lock);
	hl7227_i2c_err_clr(chip);
	return 0;

error:
	mutex_unlock(&chip->i2c_lock);
	hl7227_i2c_err_inc(chip);
	return rc;
}

static int hl7227_write_byte(struct oplus_hl7227 *chip, u8 addr, u8 data)
{
	u8 buf_temp[2] = { addr, data };
	int rc;

	mutex_lock(&chip->i2c_lock);
	rc = i2c_master_send(chip->client, buf_temp, 2);
	if (rc < 2) {
		chg_err("write 0x%04x error, rc=%d\n", addr, rc);
		mutex_unlock(&chip->i2c_lock);
		hl7227_i2c_err_inc(chip);
		rc = rc < 0 ? rc : -EIO;
		return rc;
	}
	mutex_unlock(&chip->i2c_lock);
	hl7227_i2c_err_clr(chip);

	return 0;
}

__maybe_unused static int hl7227_write_data(struct oplus_hl7227 *chip, u8 addr, u8 *buf, int len)
{
	u8 *buf_temp;
	int i;
	int rc;

	buf_temp = kzalloc(len + 1, GFP_KERNEL);
	if (!buf_temp) {
		chg_err("alloc memary error\n");
		return -ENOMEM;
	}

	buf_temp[0] = addr;
	for (i = 0; i < len; i++)
		buf_temp[i + 1] = buf[i];

	mutex_lock(&chip->i2c_lock);
	rc = i2c_master_send(chip->client, buf_temp, len + 1);
	if (rc < 3) {
		chg_err("write 0x%04x error, rc=%d\n", addr, rc);
		mutex_unlock(&chip->i2c_lock);
		kfree(buf_temp);
		rc = rc < 0 ? rc : -EIO;
		return rc;
	}
	mutex_unlock(&chip->i2c_lock);
	kfree(buf_temp);

	return 0;
}

__maybe_unused static int hl7227_read_byte_mask(struct oplus_hl7227 *chip, u8 addr, u8 mask, u8 *data)
{
	u8 temp;
	int rc;

	rc = hl7227_read_byte(chip, addr, &temp);
	if (rc < 0)
		return rc;

	*data = mask & temp;

	return 0;
}

__maybe_unused static int hl7227_write_byte_mask(struct oplus_hl7227 *chip, u8 addr, u8 mask, u8 data)
{
	u8 temp;
	int rc;

	rc = hl7227_read_byte(chip, addr, &temp);
	if (rc < 0)
		return rc;
	temp = (data & mask) | (temp & (~mask));
	rc = hl7227_write_byte(chip, addr, temp);
	if (rc < 0)
		return rc;

	return 0;
}

static int hl7227_hardware_init(struct oplus_hl7227 *chip)
{
	u8 version;
	int rc = 0;

	if (atomic_read(&chip->suspended) == 1) {
		chg_err("cp is suspend\n");
		return -EINVAL;
	}

	rc = hl7227_read_byte(chip, 0x10, &version);
	if (rc < 0) {
		chg_err("can't read chip version, rc=%d", rc);
		return rc;
	}
	chg_info("hl7227 version is 0x%02x\n", version);

	if (version == 0xa0) {
		rc = hl7227_write_byte(chip, 0xA7, 0xF9);  /*# Passwd*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0xA7, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x0D, 0x9D);  /*#QRB disable ILIM*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x0D, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x01, 0x04);  /*#UV/OV->standby*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x01, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x02, 0xFE);  /*#600mV->UV/OV*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x02, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x03, 0xFE);  /*#disable PMID_OV*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x03, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x04, 0x17);  /*#500khz*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x04, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x58, 0x81);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x58, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x0F, 0xC0);  /*#set thermal alarm to 135d*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x0F, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x00, 0x00);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x00, rc);
			return rc;
		}
	} else if (version == 0xa8) {
		rc = hl7227_write_byte(chip, 0xA7, 0xF9);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0xA7, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x0D, 0x89);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x0D, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x01, 0x08);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x01, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x02, 0xFC);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x02, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x03, 0xFA);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x03, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x04, 0x17);  /*#500khz*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x04, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x58, 0x81);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x58, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x53, 0x05);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x58, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x0F, 0xC0);  /*#set thermal alarm to 135d*/
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x0F, rc);
			return rc;
		}
		rc = hl7227_write_byte(chip, 0x00, 0x00);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x00, rc);
			return rc;
		}
	} else {
		chg_err("unknown chip version(=0x%02x)\n", version);
		return -EINVAL;
	}

	return 0;
}

static int hl7227_start_chg(struct oplus_chg_ic_dev *dev, bool start)
{
	struct oplus_hl7227 *chip;
	int rc = 0;

	if (dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	chip = oplus_chg_ic_get_drvdata(dev);

	if (atomic_read(&chip->suspended) == 1) {
		chg_err("cp is suspend\n");
		return -EINVAL;
	}

	rc = hl7227_write_byte(chip, 0x00, 0x81);
	if (rc < 0) {
		chg_err("write 0x%02x error, rc=%d", 0x00, rc);
		return rc;
	}
	rc = hl7227_write_byte(chip, 0x02, 0xFE);  /*#600mV->UV/OV*/
	if (rc < 0) {
		chg_err("write 0x%02x error, rc=%d", 0x02, rc);
		return rc;
	}
	msleep(20);
	rc = hl7227_write_byte(chip, 0x03, 0xFF);  /*#disable PMID_OV*/
	if (rc < 0) {
		chg_err("write 0x%02x error, rc=%d", 0x03, rc);
		return rc;
	}
	rc = hl7227_write_byte(chip, 0x01, 0x04);  /*#disable PMID_OV*/
	if (rc < 0) {
		chg_err("write 0x%02x error, rc=%d", 0x03, rc);
		return rc;
	}

	return 0;
}

static int hl7227_set_cp_en_gpio_enable(struct oplus_hl7227 *chip, bool en)
{
	int rc;

	if (IS_ERR_OR_NULL(chip->pinctrl) ||
	    IS_ERR_OR_NULL(chip->cp_en_active) ||
	    IS_ERR_OR_NULL(chip->cp_en_sleep)) {
		chg_err("cp_en pinctrl error\n");
		return -ENODEV;
	}

	rc = pinctrl_select_state(chip->pinctrl,
		en ? chip->cp_en_active : chip->cp_en_sleep);
	if (rc < 0)
		chg_err("can't %s cp\n", en ? "enable" : "disable");
	else
		chg_info("set value:%d, gpio_val:%d\n", en, gpio_get_value(chip->cp_en_gpio));

	return rc;
}

static int hl7227_set_enable(struct oplus_chg_ic_dev *dev, bool en)
{
	struct oplus_hl7227 *chip;
	int rc = 0;

	if (dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	chip = oplus_chg_ic_get_drvdata(dev);

	if (atomic_read(&chip->suspended) == 1) {
		chg_err("cp is suspend\n");
		return -EINVAL;
	}

	if (en) {
		rc = hl7227_set_cp_en_gpio_enable(chip, true);
		if (rc < 0)
			return rc;
		msleep(10);
	} else {
		rc = hl7227_write_byte(chip, 0x00, 0x00);
		if (rc < 0) {
			chg_err("write 0x%02x error, rc=%d", 0x00, rc);
			return rc;
		}
		msleep(10);
		rc = hl7227_set_cp_en_gpio_enable(chip, false);
		if (rc < 0)
			return rc;
	}

	return 0;
}

#ifdef USE_DEFAULT_SMT_TEST
static int hl7227_ic_test(struct oplus_chg_ic_dev *dev)
{
	struct oplus_hl7227 *chip;
	int rc = 0;
	u8 temp;

	if (dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	chip = oplus_chg_ic_get_drvdata(dev);

	rc = hl7227_read_byte(chip, 0x06, &temp);
	if (rc < 0)
		return rc;

	return 0;
}
#endif

static irqreturn_t hl7227_int_handler(int irq, void *dev_id)
{
	/*struct oplus_hl7227 *chip = dev_id;*/

	chg_debug("hl7227 int irq\n");
	return IRQ_HANDLED;
}

static int hl7227_gpio_init(struct oplus_hl7227 *chip)
{
	int rc = 0;
	struct device_node *node = chip->dev->of_node;

	chip->pinctrl = devm_pinctrl_get(chip->dev);
	if (IS_ERR_OR_NULL(chip->pinctrl)) {
		chg_err("get pinctrl fail\n");
		return -ENODEV;
	}

	chip->cp_int_gpio = of_get_named_gpio(node, "oplus,cp_int-gpio", 0);
	if (!gpio_is_valid(chip->cp_int_gpio)) {
		chg_err("cp_int_gpio not specified\n");
		return -ENODEV;
	}
	rc = gpio_request(chip->cp_int_gpio, "cp_int-gpio");
	if (rc < 0) {
		chg_err("cp_int_gpio request error, rc=%d\n", rc);
		return rc;
	}
	chip->cp_int_default = pinctrl_lookup_state(chip->pinctrl, "cp_int_default");
	if (IS_ERR_OR_NULL(chip->cp_int_default)) {
		chg_err("get cp_int_default fail\n");
		goto free_int_gpio;
	}
	gpio_direction_input(chip->cp_int_gpio);
	pinctrl_select_state(chip->pinctrl, chip->cp_int_default);
	chip->cp_int_irq = gpio_to_irq(chip->cp_int_gpio);
	rc = devm_request_irq(chip->dev, chip->cp_int_irq, hl7227_int_handler,
			IRQF_TRIGGER_FALLING, "cp_int_irq", chip);
	if (rc < 0) {
		chg_err("cp_int_irq request error, rc=%d\n", rc);
		goto free_int_gpio;
	}
	enable_irq_wake(chip->cp_int_irq);

	chip->cp_en_gpio = of_get_named_gpio(node, "oplus,cp_en-gpio", 0);
	if (!gpio_is_valid(chip->cp_en_gpio)) {
		chg_err("cp_en_gpio not specified\n");
		goto disable_int_irq;
	}
	rc = gpio_request(chip->cp_en_gpio, "cp_en-gpio");
	if (rc < 0) {
		chg_err("cp_en_gpio request error, rc=%d\n", rc);
		goto disable_int_irq;
	}
	chip->cp_en_active = pinctrl_lookup_state(chip->pinctrl, "cp_en_active");
	if (IS_ERR_OR_NULL(chip->cp_en_active)) {
		chg_err("get cp_en_active fail\n");
		goto free_en_gpio;
	}
	chip->cp_en_sleep = pinctrl_lookup_state(chip->pinctrl, "cp_en_sleep");
	if (IS_ERR_OR_NULL(chip->cp_en_sleep)) {
		chg_err("get cp_en_sleep fail\n");
		goto free_en_gpio;
	}
	gpio_direction_output(chip->cp_en_gpio, 0);
	pinctrl_select_state(chip->pinctrl, chip->cp_en_sleep);

	return 0;

free_en_gpio:
	if (!gpio_is_valid(chip->cp_en_gpio))
		gpio_free(chip->cp_en_gpio);
disable_int_irq:
	disable_irq(chip->cp_int_irq);
free_int_gpio:
	if (!gpio_is_valid(chip->cp_int_gpio))
		gpio_free(chip->cp_int_gpio);
	return rc;
}

static int hl7227_init(struct oplus_chg_ic_dev *ic_dev)
{
	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	ic_dev->online = true;
	oplus_chg_ic_virq_trigger(ic_dev, OPLUS_IC_VIRQ_ONLINE);

	return 0;
}

static int hl7227_exit(struct oplus_chg_ic_dev *ic_dev)
{
	if (ic_dev == NULL) {
		chg_err("oplus_chg_ic_dev is NULL");
		return -ENODEV;
	}
	if (!ic_dev->online)
		return 0;

	ic_dev->online = false;
	oplus_chg_ic_virq_trigger(ic_dev, OPLUS_IC_VIRQ_OFFLINE);

	return 0;
}

static int hl7227_reg_dump(struct oplus_chg_ic_dev *ic_dev)
{
	return 0;
}

static int hl7227_smt_test(struct oplus_chg_ic_dev *ic_dev, char buf[], int len)
{
	return 0;
}

static void *oplus_chg_get_func(struct oplus_chg_ic_dev *ic_dev,
				enum oplus_chg_ic_func func_id)
{
	void *func = NULL;

	if (!ic_dev->online && (func_id != OPLUS_IC_FUNC_INIT) &&
	    (func_id != OPLUS_IC_FUNC_EXIT)) {
		chg_err("%s is offline\n", ic_dev->name);
		return NULL;
	}

	switch (func_id) {
	case OPLUS_IC_FUNC_INIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_INIT,
			hl7227_init);
		break;
	case OPLUS_IC_FUNC_EXIT:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_EXIT,
			hl7227_exit);
		break;
	case OPLUS_IC_FUNC_REG_DUMP:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_REG_DUMP,
			hl7227_reg_dump);
		break;
	case OPLUS_IC_FUNC_SMT_TEST:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_SMT_TEST,
			hl7227_smt_test);
		break;
	case OPLUS_IC_FUNC_CP_ENABLE:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_CP_ENABLE,
			hl7227_set_enable);
		break;
	case OPLUS_IC_FUNC_CP_SET_WORK_START:
		func = OPLUS_CHG_IC_FUNC_CHECK(OPLUS_IC_FUNC_CP_SET_WORK_START,
			hl7227_start_chg);
		break;
	default:
		chg_err("this func(=%d) is not supported\n", func_id);
		func = NULL;
		break;
	}

	return func;
}

struct oplus_chg_ic_virq hl7227_virq_table[] = {
	{ .virq_id = OPLUS_IC_VIRQ_ERR },
	{ .virq_id = OPLUS_IC_VIRQ_ONLINE },
	{ .virq_id = OPLUS_IC_VIRQ_OFFLINE },
};

static int hl7227_driver_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
	struct oplus_hl7227 *chip;
	struct device_node *node = client->dev.of_node;
	struct oplus_chg_ic_cfg ic_cfg = {0};
	enum oplus_chg_ic_type ic_type;
	int ic_index;
	int rc;

	chg_info("call !\n");
	chip = devm_kzalloc(&client->dev, sizeof(struct oplus_hl7227), GFP_KERNEL);
	if (!chip) {
		chg_err("failed to allocate memory\n");
		return -ENOMEM;
	}

	chip->regmap = devm_regmap_init_i2c(client, &hl7227_regmap_config);
	if (!chip->regmap) {
		rc = -ENODEV;
		goto regmap_init_err;
	}

	chip->dev = &client->dev;
	chip->client = client;
	i2c_set_clientdata(client, chip);

	rc = hl7227_gpio_init(chip);
	if (rc < 0) {
		chg_err("gpio init error, rc=%d\n", rc);
		goto gpio_init_err;
	}

	mutex_init(&chip->i2c_lock);

	msleep(10);
	rc = hl7227_hardware_init(chip);
	if (rc < 0) {
		chg_err("hardware init error, rc=%d\n", rc);
		goto hw_init_err;
	}

	rc = of_property_read_u32(node, "oplus,ic_type", &ic_type);
	if (rc < 0) {
		chg_err("can't get ic type, rc=%d\n", rc);
		goto reg_ic_err;
	}
	rc = of_property_read_u32(node, "oplus,ic_index", &ic_index);
	if (rc < 0) {
		chg_err("can't get ic index, rc=%d\n", rc);
		goto reg_ic_err;
	}
	ic_cfg.name = node->name;
	ic_cfg.type = ic_type;
	ic_cfg.index = ic_index;
	snprintf(ic_cfg.manu_name, OPLUS_CHG_IC_MANU_NAME_MAX - 1, "cp-hl7227");
	snprintf(ic_cfg.fw_id, OPLUS_CHG_IC_FW_ID_MAX - 1, "0x00");
	ic_cfg.virq_data = hl7227_virq_table;
	ic_cfg.virq_num = ARRAY_SIZE(hl7227_virq_table);
	ic_cfg.get_func = oplus_chg_get_func;
	ic_cfg.of_node = node;
	chip->ic_dev = devm_oplus_chg_ic_register(chip->dev, &ic_cfg);
	if (!chip->ic_dev) {
		rc = -ENODEV;
		chg_err("register %s error\n", node->name);
		goto reg_ic_err;
	}
	chip->ic_dev->type = ic_type;
	hl7227_init(chip->ic_dev);
	chg_info("call end!\n");

	return 0;

reg_ic_err:
hw_init_err:
	if (!gpio_is_valid(chip->cp_en_gpio))
		gpio_free(chip->cp_en_gpio);
	disable_irq(chip->cp_int_irq);
	if (!gpio_is_valid(chip->cp_int_gpio))
		gpio_free(chip->cp_int_gpio);
gpio_init_err:
regmap_init_err:
	devm_kfree(&client->dev, chip);
	return rc;
}

static int hl7227_pm_resume(struct device *dev_chip)
{
	struct i2c_client *client  = container_of(dev_chip, struct i2c_client, dev);
	struct oplus_hl7227 *chip = i2c_get_clientdata(client);

	if (chip == NULL)
		return 0;

	atomic_set(&chip->suspended, 0);

	return 0;
}

static int hl7227_pm_suspend(struct device *dev_chip)
{
	struct i2c_client *client  = container_of(dev_chip, struct i2c_client, dev);
	struct oplus_hl7227 *chip = i2c_get_clientdata(client);

	if (chip == NULL)
		return 0;

	atomic_set(&chip->suspended, 1);

	return 0;
}

static const struct dev_pm_ops hl7227_pm_ops = {
	.resume = hl7227_pm_resume,
	.suspend = hl7227_pm_suspend,
};

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0))
static int hl7227_driver_remove(struct i2c_client *client)
#else
static void hl7227_driver_remove(struct i2c_client *client)
#endif
{
	struct oplus_hl7227 *chip = i2c_get_clientdata(client);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0))
	if (chip == NULL)
		return -ENODEV;
#else
	if (chip == NULL)
		return;
#endif

	if (!gpio_is_valid(chip->cp_en_gpio))
		gpio_free(chip->cp_en_gpio);
	disable_irq(chip->cp_int_irq);
	if (!gpio_is_valid(chip->cp_int_gpio))
		gpio_free(chip->cp_int_gpio);
	devm_oplus_chg_ic_unregister(chip->dev, chip->ic_dev);
	devm_kfree(&client->dev, chip);

#if (LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0))
	return 0;
#else
	return;
#endif
}

static void hl7227_shutdown(struct i2c_client *chip_client)
{
}

static const struct of_device_id hl7227_match[] = {
	{ .compatible = "oplus,cp-hl7227"},
	{},
};

static const struct i2c_device_id hl7227_id[] = {
	{"oplus,cp-hl7227", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, hl7227_id);


static struct i2c_driver hl7227_i2c_driver = {
	.driver = {
		.name = "cp-hl7227",
		.owner = THIS_MODULE,
		.of_match_table = hl7227_match,
		.pm = &hl7227_pm_ops,
	},
	.probe = hl7227_driver_probe,
	.remove = hl7227_driver_remove,
	.id_table = hl7227_id,
	.shutdown = hl7227_shutdown,
};

static __init int hl7227_driver_init(void)
{
	return i2c_add_driver(&hl7227_i2c_driver);
}

static __exit void hl7227_driver_exit(void)
{
	i2c_del_driver(&hl7227_i2c_driver);
}

oplus_chg_module_register(hl7227_driver);
