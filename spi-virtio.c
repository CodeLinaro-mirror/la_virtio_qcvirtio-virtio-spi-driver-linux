/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: GPL-2.0-only
 */
/*
 * SPI bus driver for the Virtio SPI controller
 */

#include "include/uapi/linux/virtio_spi.h"
#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/spi/spi.h>
#include <linux/stddef.h>
#include <linux/version.h> /* Only for module */
#include <linux/virtio.h>
#include <linux/virtio_ring.h>

#ifndef VIRTIO_ID_SPI
#define VIRTIO_ID_SPI 45
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 13, 0)
#define PTR_IF(cond, ptr) ((cond) ? (ptr) : NULL)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 17, 0)
#define pm_sleep_ptr(_ptr) PTR_IF(IS_ENABLED(CONFIG_PM_SLEEP), (_ptr))
#define virtio_reset_device(vdev) ((vdev)->config->reset(vdev))
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 1, 71)
static inline u8 spi_get_chipselect(const struct spi_device *spi, u8 idx)
{
	return spi->chip_select;
}
#endif

// clang-format off
struct virtio_spi_req {
	struct completion completion;
	struct spi_transfer_head transfer_head	____cacheline_aligned;
	const uint8_t *tx_buf			____cacheline_aligned;
	uint8_t *rx_buf				____cacheline_aligned;
	struct spi_transfer_result result	____cacheline_aligned;
};
// clang-format on

struct virtio_spi_priv {
	/* Virtio SPI message */
	struct virtio_spi_req spi_req;
	/* The virtio device we're associated with */
	struct virtio_device *vdev;
	/* Pointer to the virtqueue */
	struct virtqueue *vq;
	/* Copy of config space mode_func_supported */
	u32 mode_func_supported;
	/* Copy of config space max_freq_hz */
	u32 max_freq_hz;
};

static struct spi_board_info board_info = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 14, 14) /* Documentation update */
	/*
	 * To automatically create a user mode SPI device node /dev/spidevB.C
	 * the modalias here must match an entry in spidev_spi_ids. The vanilla
	 * kernel does not know spi-virtio. It may be better to create the
	 * device node using the advise in Documentation/spi/spidev.rst adding
	 * an udev rule to bind the spi-virtio to the user mode SPI device
	 * instead of applying a kernel change to add "spi-virtio" as a new
	 * element to the kernel structure.
	 */
	.modalias = "spi-virtio",
#else
	/*
	 * Automatic /dev/spidevB.C having a modalias of "spidev" does not work
	 * in newer kernels but still works for example in 4.14.35. See also
	 * Documentation/spi/spidev.rst.
	 */
	.modalias = "spidev", /* Must be "spidev" for user mode SPI */
#endif
};

static void virtio_spi_msg_done(struct virtqueue *vq)
{
	struct virtio_spi_req *req;
	unsigned int len;

	while ((req = virtqueue_get_buf(vq, &len)))
		complete(&req->completion);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)
/*
 *       .   .      .    .    .   .   .   .   .   .
 * Delay + A +      + B  +    + C + D + E + F + A +
 *       .   .      .    .    .   .   .   .   .   .
 *    ___.   .      .    .    .   .   .___.___.   .
 * CS#   |___.______.____.____.___.___|   .   |___._____________
 *       .   .      .    .    .   .   .   .   .   .
 *       .   .      .    .    .   .   .   .   .   .
 * SCLK__.___.___NNN_____NNN__.___.___.___.___.___.___NNN_______
 *
 * NOTE: 1st transfer has two words, the delay between these two words are
 * 'B' in the diagram.
 *
 * A => struct spi_device -> cs_setup
 * B => max{struct spi_transfer -> word_delay, struct spi_device -> word_delay}
 *   Note: spi_device and spi_transfer both have word_delay, Linux
 *         choose the bigger one, refer to _spi_xfer_word_delay_update function
 * C => struct spi_transfer -> delay
 * D => struct spi_device -> cs_hold
 * E => struct spi_device -> cs_inactive
 * F => struct spi_transfer -> cs_change_delay
 *
 * So the corresponding relationship:
 * A <===> cs_setup_ns (after CS asserted)
 * B <===> word_delay_ns (no matter with CS)
 * C+D <===> cs_delay_hold_ns (before CS deasserted)
 * E+F <===> cs_change_delay_inactive_ns (after CS deasserted, these two
 * values are also recommended in the Linux driver to be added up)
 */
static int virtio_spi_set_delays(struct spi_transfer_head *th,
				 struct spi_device *spi,
				 struct spi_transfer *xfer)
{
	int cs_setup;
	int cs_word_delay_xfer;
	int cs_word_delay_spi;
	int delay;
	int cs_hold;
	int cs_inactive;
	int cs_change_delay;

	cs_setup = spi_delay_to_ns(&spi->cs_setup, xfer);
	if (cs_setup < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_setup\n");
		return cs_setup;
	}
	th->cs_setup_ns = cpu_to_le32((u32)cs_setup);

	cs_word_delay_xfer = spi_delay_to_ns(&xfer->word_delay, xfer);
	if (cs_word_delay_xfer < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_word_delay_xfer\n");
		return cs_word_delay_xfer;
	}
	cs_word_delay_spi = spi_delay_to_ns(&spi->word_delay, xfer);
	if (cs_word_delay_spi < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_word_delay_spi\n");
		return cs_word_delay_spi;
	}
	if (cs_word_delay_spi > cs_word_delay_xfer)
		th->word_delay_ns = cpu_to_le32((u32)cs_word_delay_spi);
	else
		th->word_delay_ns = cpu_to_le32((u32)cs_word_delay_xfer);

	delay = spi_delay_to_ns(&xfer->delay, xfer);
	if (delay < 0) {
		dev_warn(&spi->dev, "Cannot convert delay\n");
		return delay;
	}
	cs_hold = spi_delay_to_ns(&spi->cs_hold, xfer);
	if (cs_hold < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_hold\n");
		return cs_hold;
	}
	th->cs_delay_hold_ns = cpu_to_le32((u32)delay + (u32)cs_hold);

	cs_inactive = spi_delay_to_ns(&spi->cs_inactive, xfer);
	if (cs_inactive < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_inactive\n");
		return cs_inactive;
	}
	cs_change_delay = spi_delay_to_ns(&xfer->cs_change_delay, xfer);
	if (cs_change_delay < 0) {
		dev_warn(&spi->dev, "Cannot convert cs_change_delay\n");
		return cs_change_delay;
	}
	th->cs_change_delay_inactive_ns =
		cpu_to_le32((u32)cs_inactive + (u32)cs_change_delay);

	return 0;
}
#endif

static int virtio_spi_transfer_one(struct spi_controller *ctrl,
				   struct spi_device *spi,
				   struct spi_transfer *xfer)
{
	struct virtio_spi_priv *priv = spi_controller_get_devdata(ctrl);
	struct virtio_spi_req *spi_req = &priv->spi_req;
	struct spi_transfer_head *th;
	struct scatterlist sg_out_head, sg_out_payload;
	struct scatterlist sg_in_result, sg_in_payload;
	struct scatterlist *sgs[4];
	unsigned int outcnt = 0u;
	unsigned int incnt = 0u;
	int ret;

	th = &spi_req->transfer_head;

	/* Fill struct spi_transfer_head */
	th->chip_select_id = spi_get_chipselect(spi, 0);
	th->bits_per_word = spi->bits_per_word;
	th->cs_change = xfer->cs_change;
	th->tx_nbits = xfer->tx_nbits;
	th->rx_nbits = xfer->rx_nbits;
	th->reserved[0] = 0;
	th->reserved[1] = 0;
	th->reserved[2] = 0;

	BUILD_BUG_ON(VIRTIO_SPI_CPHA != SPI_CPHA);
	BUILD_BUG_ON(VIRTIO_SPI_CPOL != SPI_CPOL);
	BUILD_BUG_ON(VIRTIO_SPI_CS_HIGH != SPI_CS_HIGH);
	BUILD_BUG_ON(VIRTIO_SPI_MODE_LSB_FIRST != SPI_LSB_FIRST);

	th->mode = cpu_to_le32(spi->mode & (SPI_LSB_FIRST | SPI_CS_HIGH |
					    SPI_CPOL | SPI_CPHA));
	if ((spi->mode & SPI_LOOP) != 0)
		th->mode |= cpu_to_le32(VIRTIO_SPI_MODE_LOOP);

	th->freq = cpu_to_le32(xfer->speed_hz);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 5, 0)
	ret = virtio_spi_set_delays(th, spi, xfer);
	if (ret)
		goto msg_done;
#else
	th->word_delay_ns = cpu_to_le32(0);
	th->cs_setup_ns = cpu_to_le32(0);
	th->cs_delay_hold_ns = cpu_to_le32(0);

	th->cs_change_delay_inactive_ns =
		cpu_to_le32((u32)xfer->delay_usecs * 1000);
#endif

	/* Set buffers */
	spi_req->tx_buf = xfer->tx_buf;
	spi_req->rx_buf = xfer->rx_buf;

	/* Prepare sending of virtio message */
	reinit_completion(&spi_req->completion);

	sg_init_one(&sg_out_head, th, sizeof(*th));
	sgs[outcnt] = &sg_out_head;
	outcnt++;

	if (spi_req->tx_buf) {
		sg_init_one(&sg_out_payload, spi_req->tx_buf, xfer->len);
		sgs[outcnt] = &sg_out_payload;
		outcnt++;
	}

	if (spi_req->rx_buf) {
		sg_init_one(&sg_in_payload, spi_req->rx_buf, xfer->len);
		sgs[outcnt] = &sg_in_payload;
		incnt++;
	}

	sg_init_one(&sg_in_result, &spi_req->result,
		    sizeof(struct spi_transfer_result));
	sgs[outcnt + incnt] = &sg_in_result;
	incnt++;

	ret = virtqueue_add_sgs(priv->vq, sgs, outcnt, incnt, spi_req,
				GFP_KERNEL);
	if (ret)
		goto msg_done;

	/* Simple implementation: There can be only one transfer in flight */
	virtqueue_kick(priv->vq);

	wait_for_completion(&priv->spi_req.completion);

	/* Read result from message and translate return code */
	switch (priv->spi_req.result.result) {
	case VIRTIO_SPI_TRANS_OK:
		/* ret is 0 */
		break;
	case VIRTIO_SPI_PARAM_ERR:
		ret = -EINVAL;
		break;
	case VIRTIO_SPI_TRANS_ERR:
		ret = -EIO;
		break;
	default: /* Protocol violation */
		ret = -EIO;
		break;
	}

msg_done:
	if (ret)
		ctrl->cur_msg->status = ret;

	return ret;
}

static void virtio_spi_read_config(struct virtio_device *vdev)
{
	struct spi_controller *ctrl = dev_get_drvdata(&vdev->dev);
	struct virtio_spi_priv *priv = vdev->priv;
	u8 cs_max_number;
	u8 tx_nbits_supported;
	u8 rx_nbits_supported;

	// clang-format off
	cs_max_number = virtio_cread8(vdev, offsetof(struct virtio_spi_config,
						     cs_max_number));
	ctrl->num_chipselect = cs_max_number;

	/* Set the mode bits which are understood by this driver */
	priv->mode_func_supported =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      mode_func_supported));
	ctrl->mode_bits = priv->mode_func_supported &
			  (VIRTIO_SPI_CS_HIGH | VIRTIO_SPI_MODE_LSB_FIRST);
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_CPHA_1) != 0)
		ctrl->mode_bits |= VIRTIO_SPI_CPHA;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_CPOL_1) != 0)
		ctrl->mode_bits |= VIRTIO_SPI_CPOL;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_LSB_FIRST) != 0)
		ctrl->mode_bits |= SPI_LSB_FIRST;
	if ((priv->mode_func_supported & VIRTIO_SPI_MF_SUPPORT_LOOPBACK) != 0)
		ctrl->mode_bits |= SPI_LOOP;
	tx_nbits_supported =
		virtio_cread8(vdev, offsetof(struct virtio_spi_config,
					     tx_nbits_supported));
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_DUAL) != 0)
		ctrl->mode_bits |= SPI_TX_DUAL;
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_QUAD) != 0)
		ctrl->mode_bits |= SPI_TX_QUAD;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	if ((tx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_OCTAL) != 0)
		ctrl->mode_bits |= SPI_TX_OCTAL;
#endif
	rx_nbits_supported =
		virtio_cread8(vdev, offsetof(struct virtio_spi_config,
					     rx_nbits_supported));
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_DUAL) != 0)
		ctrl->mode_bits |= SPI_RX_DUAL;
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_QUAD) != 0)
		ctrl->mode_bits |= SPI_RX_QUAD;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
	if ((rx_nbits_supported & VIRTIO_SPI_RX_TX_SUPPORT_OCTAL) != 0)
		ctrl->mode_bits |= SPI_RX_OCTAL;
#endif

	ctrl->bits_per_word_mask =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      bits_per_word_mask));

	priv->max_freq_hz =
		virtio_cread32(vdev, offsetof(struct virtio_spi_config,
					      max_freq_hz));
	// clang-format on
}

static int virtio_spi_find_vqs(struct virtio_spi_priv *priv)
{
	struct virtqueue *vq;

	vq = virtio_find_single_vq(priv->vdev, virtio_spi_msg_done, "spi-rq");
	if (IS_ERR(vq))
		return (int)PTR_ERR(vq);
	priv->vq = vq;
	return 0;
}

/* Function must not be called before virtio_spi_find_vqs() has been run */
static void virtio_spi_del_vq(struct virtio_device *vdev)
{
	virtio_reset_device(vdev);
	vdev->config->del_vqs(vdev);
}

static int virtio_spi_validate(struct virtio_device *vdev)
{
	/*
	 * SPI needs always access to the config space.
	 * Check that the driver can access the config space
	 */
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	if (!virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev,
			"device does not comply with spec version 1.x\n");
		return -EINVAL;
	}

	return 0;
}

static int virtio_spi_probe(struct virtio_device *vdev)
{
	struct device_node *np = vdev->dev.parent->of_node;
	struct virtio_spi_priv *priv;
	struct spi_controller *ctrl;
	int err;
	u32 bus_num;
	u16 csi;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
	ctrl = spi_alloc_master(&vdev->dev, sizeof(*priv));
#else
	ctrl = devm_spi_alloc_host(&vdev->dev, sizeof(*priv));
#endif
	if (!ctrl)
		return -ENOMEM;

	priv = spi_controller_get_devdata(ctrl);
	priv->vdev = vdev;
	vdev->priv = priv;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0) /* Assign "spi" child node */
	ctrl->dev.of_node = vdev->dev.of_node;
#endif
	dev_set_drvdata(&vdev->dev, ctrl);

	init_completion(&priv->spi_req.completion);

	err = of_property_read_u32(np, "spi,bus-num", &bus_num);
	if (!err && bus_num <= S16_MAX)
		ctrl->bus_num = (s16)bus_num;

	virtio_spi_read_config(vdev);

	ctrl->transfer_one = virtio_spi_transfer_one;

	err = virtio_spi_find_vqs(priv);
	if (err) {
		dev_err(&vdev->dev, "Cannot setup virtqueues\n");
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
		spi_controller_put(ctrl);
#endif
		return err;
	}

	board_info.max_speed_hz = priv->max_freq_hz;
	board_info.bus_num = (u16)ctrl->bus_num;

	if (!(priv->mode_func_supported & VIRTIO_SPI_CS_HIGH))
		board_info.mode = SPI_MODE_0;
	else
		board_info.mode = SPI_MODE_0 | SPI_CS_HIGH;

	err = spi_register_controller(ctrl);
	if (err) {
		dev_err(&vdev->dev, "Cannot register controller\n");
		goto err_return;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
	if (vdev->dev.of_node) {
		dev_dbg(&vdev->dev, "Final setup triggered by DT child node\n");
		return 0;
	}
#endif

	/* Add chip selects to controller */
	for (csi = 0; csi < ctrl->num_chipselect; csi++) {
		dev_dbg(&vdev->dev, "Setting up CS=%u\n", csi);
		board_info.chip_select = csi;

		if (!spi_new_device(ctrl, &board_info)) {
			dev_err(&vdev->dev, "Cannot setup device %u\n", csi);
			spi_unregister_controller(ctrl);
			err = -ENODEV;
			goto err_return;
		}
	}

	return 0;

err_return:
	vdev->config->del_vqs(vdev);
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 2, 0)
	spi_controller_put(ctrl);
#endif
	return err;
}

static void virtio_spi_remove(struct virtio_device *vdev)
{
	struct spi_controller *ctrl = dev_get_drvdata(&vdev->dev);

	/* Order: 1.) unregister controller, 2.) remove virtqueue */
	spi_unregister_controller(ctrl);
	virtio_spi_del_vq(vdev);
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)) || defined(CONFIG_PM_SLEEP)
static int virtio_spi_freeze(struct virtio_device *vdev)
{
	struct device *dev = &vdev->dev;
	struct spi_controller *ctrl = dev_get_drvdata(dev);
	int ret;

	ret = spi_controller_suspend(ctrl);
	if (ret) {
		dev_warn(dev, "cannot suspend controller (%d)\n", ret);
		return ret;
	}

	virtio_spi_del_vq(vdev);
	return 0;
}

static int virtio_spi_restore(struct virtio_device *vdev)
{
	struct device *dev = &vdev->dev;
	struct spi_controller *ctrl = dev_get_drvdata(dev);
	int ret;

	ret = virtio_spi_find_vqs(vdev->priv);
	if (ret) {
		dev_err(dev, "problem starting vqueue (%d)\n", ret);
		return ret;
	}

	ret = spi_controller_resume(ctrl);
	if (ret)
		dev_err(dev, "problem resuming controller (%d)\n", ret);

	return ret;
}
#endif

static struct virtio_device_id virtio_spi_id_table[] = {
	{ VIRTIO_ID_SPI, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_spi_driver = {
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table = virtio_spi_id_table,
	.validate = virtio_spi_validate,
	.probe = virtio_spi_probe,
	.remove = virtio_spi_remove,
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(6, 6, 0)) || defined(CONFIG_PM_SLEEP)
	.freeze = pm_sleep_ptr(virtio_spi_freeze),
	.restore = pm_sleep_ptr(virtio_spi_restore),
#endif
};

module_virtio_driver(virtio_spi_driver);
MODULE_DEVICE_TABLE(virtio, virtio_spi_id_table);

MODULE_AUTHOR("Qualcomm Technologies, Inc. and/or its subsidiaries");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Virtio SPI bus driver");
