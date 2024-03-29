/* Source for:
 * Cypress CY8CTMA300 Prototype touchscreen driver.
 * drivers/input/touchscreen/cy8c_ts.c
 *
 * Copyright (C) 2009, 2010 Cypress Semiconductor, Inc.
 * Copyright (c) 2010, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2, and only version 2, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Cypress reserves the right to make changes without further notice
 * to the materials described herein. Cypress does not assume any
 * liability arising out of the application described herein.
 *
 * Contact Cypress Semiconductor at www.cypress.com
 *
 * History:
 *			(C) 2010 Cypress - Update for GPL distribution
 *			(C) 2009 Cypress - Assume maintenance ownership
 *			(C) 2009 Enea - Original prototype
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#include <linux/workqueue.h>
#include <linux/input/cy8c_ts.h>
#include <linux/pm.h>

#define CY8CTMA300	0x0
#define CY8CTMG200	0x1

#define INVALID_DATA	0xff

#define TOUCHSCREEN_TIMEOUT	(msecs_to_jiffies(10))
#define INITIAL_DELAY		(msecs_to_jiffies(25000))

struct cy8c_ts_data {
	u8 x_index;
	u8 y_index;
	u8 z_index;
	u8 id_index;
	u8 touch_index;
	u8 data_reg;
	u8 status_reg;
	u8 data_size;
	u8 touch_bytes;
	u8 update_data;
	u8 touch_meta_data;
	u8 finger_size;
};

static struct cy8c_ts_data devices[] = {
	[0] = {
		.x_index = 6,
		.y_index = 4,
		.z_index = 3,
		.id_index = 0,
		.data_reg = 0x3,
		.status_reg = 0x1,
		.update_data = 0x4,
		.touch_bytes = 8,
		.touch_meta_data = 3,
		.finger_size = 70,
	},
	[1] = {
		.x_index = 2,
		.y_index = 4,
		.id_index = 6,
		.data_reg = 0x6,
		.status_reg = 0x5,
		.update_data = 0x1,
		.touch_bytes = 12,
		.finger_size = 70,
	},
};

struct cy8c_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct delayed_work work;
	struct workqueue_struct *wq;
	struct cy8c_ts_platform_data *pdata;
	struct cy8c_ts_data *dd;
	u8 *touch_data;
	u8 device_id;
	u8 prev_touches;
};

static inline u16 join_bytes(u8 a, u8 b)
{
	u16 ab = 0;
	ab = ab | a;
	ab = ab << 8 | b;
	return ab;
}

static s32 cy8c_ts_write_reg_u8(struct i2c_client *client, u8 reg, u8 val)
{
	s32 data;

	data = i2c_smbus_write_byte_data(client, reg, val);
	if (data < 0)
		dev_err(&client->dev, "error %d in writing reg 0x%x\n",
						 data, reg);

	return data;
}

static int cy8c_ts_read(struct i2c_client *client, u8 reg, u8 *buf, int num)
{
	struct i2c_msg xfer_msg[2];

	xfer_msg[0].addr = client->addr;
	xfer_msg[0].len = 1;
	xfer_msg[0].flags = 0;
	xfer_msg[0].buf = &reg;

	xfer_msg[1].addr = client->addr;
	xfer_msg[1].len = num;
	xfer_msg[1].flags = I2C_M_RD;
	xfer_msg[1].buf = buf;

	return i2c_transfer(client->adapter, xfer_msg, 2);
}

static void report_data(struct cy8c_ts *ts, u16 x, u16 y, u8 pressure, u8 id)
{
	if (ts->pdata->swap_xy)
		swap(x, y);

	/* handle inverting coordinates */
	if (ts->pdata->invert_x)
		x = ts->pdata->res_x - x;
	if (ts->pdata->invert_y)
		y = ts->pdata->res_y - y;

	input_report_abs(ts->input, ABS_MT_TRACKING_ID, id);
	input_report_abs(ts->input, ABS_MT_POSITION_X, x);
	input_report_abs(ts->input, ABS_MT_POSITION_Y, y);
	input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, pressure);
	input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, ts->dd->finger_size);
	input_mt_sync(ts->input);
}

static void process_tma300_data(struct cy8c_ts *ts)
{
	u8 id, pressure, touches, i;
	u16 x, y;

	touches = ts->touch_data[ts->dd->touch_index];

	for (i = 0; i < touches; i++) {
		id = ts->touch_data[i * ts->dd->touch_bytes +
						ts->dd->id_index];
		pressure = ts->touch_data[i * ts->dd->touch_bytes +
							ts->dd->z_index];
		x = join_bytes(ts->touch_data[i * ts->dd->touch_bytes +
							ts->dd->x_index],
			ts->touch_data[i * ts->dd->touch_bytes +
							ts->dd->x_index + 1]);
		y = join_bytes(ts->touch_data[i * ts->dd->touch_bytes +
							ts->dd->y_index],
			ts->touch_data[i * ts->dd->touch_bytes +
							ts->dd->y_index + 1]);

		report_data(ts, x, y, pressure, id);
	}

	for (i = 0; i < ts->prev_touches - touches; i++) {
		input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR, 0);
		input_mt_sync(ts->input);
	}

	ts->prev_touches = touches;
	input_sync(ts->input);
}

static void process_tmg200_data(struct cy8c_ts *ts)
{
	u8 id, touches, i;
	u16 x, y;

	touches = ts->touch_data[ts->dd->touch_index];

	if (touches > 0) {
		x = join_bytes(ts->touch_data[ts->dd->x_index],
				ts->touch_data[ts->dd->x_index+1]);
		y = join_bytes(ts->touch_data[ts->dd->y_index],
				ts->touch_data[ts->dd->y_index+1]);
		id = ts->touch_data[ts->dd->id_index];

		report_data(ts, x, y, 255, id);

		if (touches == 2) {
			x = join_bytes(ts->touch_data[ts->dd->x_index+5],
					ts->touch_data[ts->dd->x_index+6]);
			y = join_bytes(ts->touch_data[ts->dd->y_index+5],
				ts->touch_data[ts->dd->y_index+6]);
			id = ts->touch_data[ts->dd->id_index+5];

			report_data(ts, x, y, 255, id);
		}
	} else {
		for (i = 0; i < ts->prev_touches; i++) {
			input_report_abs(ts->input, ABS_MT_TOUCH_MAJOR,	0);
			input_report_abs(ts->input, ABS_MT_WIDTH_MAJOR,	0);
			input_mt_sync(ts->input);
		}
	}

	input_sync(ts->input);
	ts->prev_touches = touches;
}

static void cy8c_ts_xy_worker(struct work_struct *work)
{
	int rc;
	struct cy8c_ts *ts = container_of(work, struct cy8c_ts,
				 work.work);

	/* read data from DATA_REG */
	rc = cy8c_ts_read(ts->client, ts->dd->data_reg, ts->touch_data,
							ts->dd->data_size);
	if (rc < 0) {
		dev_err(&ts->client->dev, "read failed\n");
		goto schedule;
	}

	if (ts->touch_data[ts->dd->touch_index] == INVALID_DATA)
		goto schedule;

	if (ts->device_id == CY8CTMA300)
		process_tma300_data(ts);
	else
		process_tmg200_data(ts);

schedule:
	if (ts->pdata->use_polling)
		queue_delayed_work(ts->wq, &ts->work, TOUCHSCREEN_TIMEOUT);
	else
		enable_irq(ts->client->irq);

	/* write to STATUS_REG to update coordinates*/
	rc = cy8c_ts_write_reg_u8(ts->client, ts->dd->status_reg,
						ts->dd->update_data);
	if (rc < 0) {
		dev_err(&ts->client->dev, "write failed, try once more\n");

		rc = cy8c_ts_write_reg_u8(ts->client, ts->dd->status_reg,
						ts->dd->update_data);
		if (rc < 0)
			dev_err(&ts->client->dev, "write failed, exiting\n");
	}
}

static irqreturn_t cy8c_ts_irq(int irq, void *dev_id)
{
	struct cy8c_ts *ts = dev_id;

	disable_irq_nosync(irq);

	queue_delayed_work(ts->wq, &ts->work, 0);

	return IRQ_HANDLED;
}

static int cy8c_ts_open(struct input_dev *dev)
{
	int rc;
	struct cy8c_ts *ts = input_get_drvdata(dev);

	/* power on the device */
	if (ts->pdata->power_on) {
		rc = ts->pdata->power_on(1);
		if (rc) {
			pr_err("%s: Unable to power on the device\n", __func__);
			return -EINVAL;
		}
	}

	if (!ts->pdata->use_polling) {

		rc = request_irq(ts->client->irq, cy8c_ts_irq,
					IRQF_TRIGGER_FALLING,
					ts->client->dev.driver->name, ts);
		if (rc) {
			dev_err(&ts->client->dev, "could not request irq\n");
			goto error_req_irq_fail;
		}

		/* Clear the status register of the TS controller */
		rc = cy8c_ts_write_reg_u8(ts->client, ts->dd->status_reg,
							ts->dd->update_data);
		if (rc < 0) {
			/* Do multiple writes in case of failure */
			dev_err(&ts->client->dev, "%s: write failed %d"
					"trying again\n", __func__, rc);
			rc = cy8c_ts_write_reg_u8(ts->client,
				ts->dd->status_reg, ts->dd->update_data);
			if (rc < 0) {
				dev_err(&ts->client->dev, "%s: write failed"
					"second time %d \n", __func__, rc);
			}
		}
	} else {
		/* wait for 25sec before reading I2C due to
		 * hardware limitation
		 */
		queue_delayed_work(ts->wq, &ts->work, INITIAL_DELAY);
	}

	return 0;

error_req_irq_fail:
	if (ts->pdata->power_on)
		ts->pdata->power_on(0);

	return rc;
}

static void cy8c_ts_close(struct input_dev *dev)
{
	int rc;
	struct cy8c_ts *ts = input_get_drvdata(dev);

	rc = cancel_delayed_work_sync(&ts->work);

	if (rc && !ts->pdata->use_polling)
		enable_irq(ts->client->irq);

	if (!ts->pdata->use_polling)
		free_irq(ts->client->irq, ts);

	/* power off the device */
	if (ts->pdata->power_on)
		ts->pdata->power_on(0);
}

static int cy8c_ts_init_ts(struct i2c_client *client, struct cy8c_ts *ts)
{
	struct input_dev *input_device;
	int rc;

	ts->dd = &devices[ts->device_id];

	if (!ts->pdata->nfingers) {
		dev_err(&client->dev, "Touches information not specified\n");
		return -EINVAL;
	}

	if (ts->device_id == CY8CTMA300) {
		if (ts->pdata->nfingers > 10) {
			dev_err(&client->dev, "Touches >=1 & <= 10\n");
			return -EINVAL;
		}
		ts->dd->data_size = ts->pdata->nfingers * ts->dd->touch_bytes +
						ts->dd->touch_meta_data;
		ts->dd->touch_index = ts->pdata->nfingers *
						ts->dd->touch_bytes;
	} else if (ts->device_id == CY8CTMG200) {
		if (ts->pdata->nfingers > 2) {
			dev_err(&client->dev, "Touches >=1 & <= 2\n");
			return -EINVAL;
		}
		ts->dd->data_size = ts->dd->touch_bytes;
		ts->dd->touch_index = 0x0;
	}

	ts->touch_data = kzalloc(ts->dd->data_size, GFP_KERNEL);
	if (!ts->touch_data) {
		pr_err("%s: Unable to allocate memory\n", __func__);
		return -ENOMEM;
	}

	ts->prev_touches = 0;

	input_device = input_allocate_device();
	if (!input_device) {
		rc = -ENOMEM;
		goto error_alloc_dev;
	}

	ts->input = input_device;
	input_device->name = ts->pdata->ts_name;
	input_device->id.bustype = BUS_I2C;
	input_device->dev.parent = &client->dev;
	input_set_drvdata(input_device, ts);

	__set_bit(EV_ABS, input_device->evbit);

	input_set_abs_params(input_device, ABS_MT_POSITION_X,
			ts->pdata->dis_min_x, ts->pdata->dis_max_x, 0, 0);
	input_set_abs_params(input_device, ABS_MT_POSITION_Y,
			ts->pdata->dis_min_y, ts->pdata->dis_max_y, 0, 0);
	input_set_abs_params(input_device, ABS_MT_TOUCH_MAJOR,
			ts->pdata->min_touch, ts->pdata->max_touch, 0, 0);
	input_set_abs_params(input_device, ABS_MT_WIDTH_MAJOR,
			ts->pdata->min_width, ts->pdata->max_width, 0, 0);
	input_set_abs_params(input_device, ABS_MT_TRACKING_ID,
			ts->pdata->min_tid, ts->pdata->max_tid, 0, 0);
	/*setting dummy key to make it work for virutal keys*/
	input_set_capability(input_device, EV_KEY, KEY_PROG1);

	ts->wq = create_singlethread_workqueue("kworkqueue_ts");
	if (!ts->wq) {
		dev_err(&client->dev, "Could not create workqueue\n");
		goto error_wq_create;
	}

	INIT_DELAYED_WORK(&ts->work, cy8c_ts_xy_worker);

	input_device->open = cy8c_ts_open;
	input_device->close = cy8c_ts_close;

	ts->wq = create_singlethread_workqueue("kworkqueue_ts");
	if (!ts->wq) {
		dev_err(&client->dev, "Could not create workqueue\n");
		goto error_wq_create;
	}

	INIT_DELAYED_WORK(&ts->work, cy8c_ts_xy_worker);

	rc = input_register_device(input_device);
	if (rc)
		goto error_unreg_device;

	return 0;

error_unreg_device:
	destroy_workqueue(ts->wq);
error_wq_create:
	input_free_device(input_device);
error_alloc_dev:
	kfree(ts->touch_data);
	return rc;
}

#ifdef CONFIG_PM
static int cy8c_ts_suspend(struct device *dev)
{
	struct cy8c_ts *ts = dev_get_drvdata(dev);
	int rc = 0;

	if (!ts->pdata->use_polling)
		disable_irq_nosync(ts->client->irq);

	rc = cancel_delayed_work_sync(&ts->work);

	if (rc && !ts->pdata->use_polling)
		enable_irq(ts->client->irq);

	if (ts->pdata->power_on) {
		rc = ts->pdata->power_on(0);
		if (rc) {
			dev_err(dev, "unable to goto suspend\n");
			return rc;
		}
	}
	return 0;
}

static int cy8c_ts_resume(struct device *dev)
{
	struct cy8c_ts *ts = dev_get_drvdata(dev);
	int rc = 0;

	if (ts->pdata->power_on) {
		rc = ts->pdata->power_on(1);
		if (rc) {
			dev_err(dev, "unable to resume\n");
			return rc;
		}
	}

	if (ts->pdata->use_polling)
		queue_delayed_work(ts->wq, &ts->work, TOUCHSCREEN_TIMEOUT);
	else
		enable_irq(ts->client->irq);

	return 0;
}

static struct dev_pm_ops cy8c_ts_pm_ops = {
	.suspend	= cy8c_ts_suspend,
	.resume		= cy8c_ts_resume,
};
#endif

static int __devinit cy8c_ts_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cy8c_ts *ts;
	struct cy8c_ts_platform_data *pdata = client->dev.platform_data;
	int rc;

	if (!pdata) {
		dev_err(&client->dev, "platform data is required!\n");
		return -EINVAL;
	}

	if (!i2c_check_functionality(client->adapter,
				     I2C_FUNC_SMBUS_READ_WORD_DATA)) {
		dev_err(&client->dev, "I2C functionality not supported\n");
		return -EIO;
	}


	ts = kzalloc(sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;

	ts->client = client;
	ts->pdata = pdata;
	i2c_set_clientdata(client, ts);
	ts->device_id = id->driver_data;

	rc = cy8c_ts_init_ts(client, ts);
	if (rc < 0) {
		dev_err(&client->dev, "CY8CTMG200-TMA300 init failed\n");
		goto error_touch_data_alloc;
	}

	return 0;

error_touch_data_alloc:
	kfree(ts);
	return rc;
}

static int __devexit cy8c_ts_remove(struct i2c_client *client)
{
	struct cy8c_ts *ts = i2c_get_clientdata(client);
	int rc;

	rc = cancel_delayed_work_sync(&ts->work);

	if (rc && !ts->pdata->use_polling)
		enable_irq(ts->client->irq);

	if (!ts->pdata->use_polling)
		free_irq(client->irq, ts);

	destroy_workqueue(ts->wq);

	input_unregister_device(ts->input);

	if (ts->pdata->power_on)
		ts->pdata->power_on(0);

	kfree(ts->touch_data);
	kfree(ts);

	return 0;
}

static const struct i2c_device_id cy8c_ts_id[] = {
	{"cy8ctma300", CY8CTMA300},
	{"cy8ctmg200", CY8CTMG200},
	{}
};
MODULE_DEVICE_TABLE(i2c, cy8c_ts_id);


static struct i2c_driver cy8c_ts_driver = {
	.driver = {
		.name = "cy8c_ts",
		.owner = THIS_MODULE,
#ifdef CONFIG_PM
		.pm = &cy8c_ts_pm_ops,
#endif
	},
	.probe		= cy8c_ts_probe,
	.remove		= __devexit_p(cy8c_ts_remove),
	.id_table	= cy8c_ts_id,
};

static int __init cy8c_ts_init(void)
{
	return i2c_add_driver(&cy8c_ts_driver);
}
/* Making this as late init to avoid power fluctuations
 * during LCD initialization.
 */
late_initcall(cy8c_ts_init);

static void __exit cy8c_ts_exit(void)
{
	return i2c_del_driver(&cy8c_ts_driver);
}
module_exit(cy8c_ts_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CY8CTMA300-CY8CTMG200 touchscreen controller driver");
MODULE_AUTHOR("Cypress");
MODULE_ALIAS("platform:cy8c_ts");
