#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>      // udelay (polling)
#include <linux/mod_devicetable.h> // Device Tree parsing
#include <linux/of.h>
#include <linux/slab.h>  // GFP_KERNEL
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include "glanda_uapi.h"
#include <linux/mutex.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem_shmem_helper.h>

#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_modeset_helper_vtables.h>

// Hardware Constants
#define GLANDA_WIDTH      640
#define GLANDA_HEIGHT     480
#define GLANDA_VRAM_SIZE  (GLANDA_WIDTH * GLANDA_HEIGHT * 4)
#define GLANDA_MMIO_SIZE  32
#define GLANDA_MMIO_OFFSET 0x00200000

// Base addresses (x86 testing)
#define BRIDGE_BASE       0xC0000000
#define GLANDA_VRAM_BASE  (BRIDGE_BASE + 0x00000000)
#define GLANDA_MMIO_BASE  (BRIDGE_BASE + GLANDA_MMIO_OFFSET)
#define GLANDA_BASE_SIZE  BRIDGE_BASE + 0x01000000 - 1

// Register Offsets
#define REG_STATUS  0x00
#define REG_CTRL    0x04
#define REG_COORD0  0x08
#define REG_COORD1  0x0C
#define REG_COLOR   0x10
#define REG_ISR     0x14
#define REG_IER     0x18

// Bit Masks
#define INT_DONE    (1 << 0)
#define INT_VSYNC   (1 << 1)

#define STATUS_BUSY (1 << 0)
#define CMD_CLEAR   (0x1)
#define CMD_RECT    (0x2)
#define CMD_LINE    (0x3)
#define CTRL_START  (1 << 4)

struct glanda_device {
    struct drm_device drm;
    void __iomem *mmio_base;    
    void __iomem *vram_base;    
    struct device *dev;         
    phys_addr_t vram_phys;      
    
    int irq;
    wait_queue_head_t cmd_wq;
    bool cmd_done;

    struct mutex lock;

    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
};

#define to_glanda(dev) container_of(dev, struct glanda_device, drm)

static int glanda_connector_get_modes(struct drm_connector *connector)
{
    struct drm_display_mode *mode;

    pr_info("GlandaGPU-Debug: glanda_connector_get_modes() wurde aufgerufen!\n");

    mode = drm_mode_create(connector->dev);
    if (!mode) {
        pr_info("GlandaGPU-Debug: Fehler beim Erstellen des Modus-Objekts!\n");
        return 0;
    }

    // VGA-Standard 640x480 @ 60 Hz
    mode->hdisplay = 640;
    mode->hsync_start = 656;
    mode->hsync_end = 752;
    mode->htotal = 800;

    mode->vdisplay = 480;
    mode->vsync_start = 490;
    mode->vsync_end = 492;
    mode->vtotal = 525;

    mode->clock = 25175; // 25.175 MHz Pixelclock

    mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
    mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;

    drm_mode_set_name(mode);
    drm_mode_probed_add(connector, mode);

    pr_info("GlandaGPU-Debug: Modus 640x480 erfolgreich hinzugefügt.\n");
    return 1;
}

static enum drm_connector_status glanda_connector_detect(struct drm_connector *connector, bool force)
{
    pr_info("GlandaGPU-Debug: glanda_connector_detect() aufgerufen! force=%d\n", force);
    return connector_status_connected;
}

static const struct drm_crtc_funcs glanda_crtc_funcs = {
    .destroy = drm_crtc_cleanup,
    .set_config = drm_crtc_helper_set_config, 
};

static const struct drm_crtc_helper_funcs glanda_crtc_helper_funcs = {
};

static const struct drm_connector_helper_funcs glanda_connector_helper_funcs = {
    .get_modes = glanda_connector_get_modes,
};

static const struct drm_connector_funcs glanda_connector_funcs = {
    .fill_modes = drm_helper_probe_single_connector_modes,
    .destroy = drm_connector_cleanup,
    .detect = glanda_connector_detect,
};

static const struct drm_mode_config_funcs glanda_mode_config_funcs = {
    .fb_create = drm_gem_fb_create, 
};

static const struct file_operations glanda_drm_fops = {
    .owner          = THIS_MODULE,
    .open           = drm_open,
    .release        = drm_release,
    .unlocked_ioctl = drm_ioctl,
    .compat_ioctl   = drm_compat_ioctl,
    .poll           = drm_poll,
    .read           = drm_read,
    .llseek         = noop_llseek,
    .mmap           = drm_gem_mmap,
};

static const struct drm_driver glanda_drm_driver = {
    .driver_features    = DRIVER_GEM | DRIVER_MODESET,
    .name               = "glandagpu",
    .desc               = "GlandaGPU Hardware Accelerated DRM Driver",
    .major              = 1,
    .minor              = 0,
    .fops               = &glanda_drm_fops,
    .dumb_create        = drm_gem_shmem_dumb_create,
};

static irqreturn_t glanda_irq_handler(int irq, void *dev_id)
{
    struct glanda_device *gdev = dev_id;
    if (!gdev || !gdev->mmio_base) {
        return IRQ_NONE;
    }
    u32 isr = readl(gdev->mmio_base + REG_ISR);

    if (!isr) {
        return IRQ_NONE;
    }

    if (isr & INT_DONE) {
        gdev->cmd_done = true;
        wake_up_interruptible(&gdev->cmd_wq);
    } 

    writel(isr, gdev->mmio_base + REG_ISR);
    return IRQ_HANDLED;
}

static int glandagpu_probe(struct platform_device *pdev)
{
    struct resource *res;
    struct glanda_device *gdev;
    int ret;

    dev_info(&pdev->dev, "GlandaGPU Probe started\n");

    gdev = devm_drm_dev_alloc(&pdev->dev, &glanda_drm_driver, struct glanda_device, drm);
    if (IS_ERR(gdev)) {
        return PTR_ERR(gdev);
    }

    gdev->dev = &pdev->dev;
    platform_set_drvdata(pdev, gdev);

    mutex_init(&gdev->lock);

    init_waitqueue_head(&gdev->cmd_wq);
    gdev->irq = -1;

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) return -ENODEV;
    gdev->vram_phys = res->start;
    gdev->vram_base = devm_ioremap(&pdev->dev, res->start, GLANDA_VRAM_SIZE);
    gdev->mmio_base = devm_ioremap(&pdev->dev, res->start + GLANDA_MMIO_OFFSET, GLANDA_MMIO_SIZE);
    
    if (!gdev->vram_base || !gdev->mmio_base) return -ENOMEM;

    writel(0, gdev->mmio_base + REG_IER);
    writel(0xFFFFFFFF, gdev->mmio_base + REG_ISR); 

    ret = platform_get_irq(pdev, 0);
    if (ret > 0) {
        gdev->irq = ret;
        ret = devm_request_irq(&pdev->dev, gdev->irq, glanda_irq_handler,
                               IRQF_SHARED, "glandagpu", gdev);
        if (ret) {
            dev_err(&pdev->dev, "Failed to request IRQ %d\n", gdev->irq);
            return ret;
        }
        
        writel(INT_DONE, gdev->mmio_base + REG_IER);
        dev_info(&pdev->dev, "IRQ %d requested and enabled\n", gdev->irq);
    } else {
        dev_warn(&pdev->dev, "No IRQ found, falling back to polling\n");
    }

    // DRM Modus Konfiguration
    drm_mode_config_init(&gdev->drm);
    gdev->drm.mode_config.min_width = 640;
    gdev->drm.mode_config.min_height = 480;
    gdev->drm.mode_config.max_width = 640;
    gdev->drm.mode_config.max_height = 480;
    gdev->drm.mode_config.funcs = &glanda_mode_config_funcs;

    // CRTC initialisieren
    ret = drm_crtc_init(&gdev->drm, &gdev->crtc, &glanda_crtc_funcs);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize CRTC\n");
        goto err_mode_cleanup;
    }
    drm_crtc_helper_add(&gdev->crtc, &glanda_crtc_helper_funcs);

    // Encoder initialisieren
    ret = drm_simple_encoder_init(&gdev->drm, &gdev->encoder, DRM_MODE_ENCODER_DAC);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize encoder\n");
        goto err_mode_cleanup;
    }
    gdev->encoder.possible_crtcs = 1; 

    // Connector initialisieren
    ret = drm_connector_init(&gdev->drm, &gdev->connector, &glanda_connector_funcs, DRM_MODE_CONNECTOR_VGA);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize connector\n");
        goto err_mode_cleanup;
    }
    drm_connector_helper_add(&gdev->connector, &glanda_connector_helper_funcs);

    drm_connector_attach_encoder(&gdev->connector, &gdev->encoder);

    drm_helper_probe_single_connector_modes(&gdev->connector, 1024, 768);

    ret = drm_dev_register(&gdev->drm, 0);
    if (ret) goto err_mode_cleanup;

    dev_info(&pdev->dev, "GlandaGPU DRM Initialized (/dev/dri/cardX created)\n");
    return 0;

err_mode_cleanup:
    drm_mode_config_cleanup(&gdev->drm);
    return ret;
}

static void glandagpu_remove(struct platform_device *pdev)
{
    struct glanda_device *gdev = platform_get_drvdata(pdev);

    drm_dev_unregister(&gdev->drm);
    drm_mode_config_cleanup(&gdev->drm);

    writel(0, gdev->mmio_base + REG_IER);
    dev_info(&pdev->dev, "GlandaGPU DRM Driver removed\n");
}

static const struct of_device_id glanda_of_match[] = {
    { .compatible = "glanda,gpu-1.0", },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, glanda_of_match);

static struct platform_driver glandagpu_driver = {
    .driver = {
        .name = "glandagpu",
        .of_match_table = glanda_of_match,
    },
    .probe = glandagpu_probe,
    .remove = glandagpu_remove,
};

#ifdef CONFIG_X86
static struct platform_device *pdev_x86;

static struct resource glandagpu_resources[] = {
    [0] = { 
        .start = BRIDGE_BASE,
        .end   = GLANDA_BASE_SIZE, 
        .flags = IORESOURCE_MEM,
    },
    [1] = { 
        .start = 11,
        .end   = 11,
        .flags = IORESOURCE_IRQ,
    },
};
#endif

static int __init glandagpu_init(void)
{
    int ret;

    ret = platform_driver_register(&glandagpu_driver);
    if (ret) {
        pr_err("GlandaGPU: Failed to register platform driver\n");
        return ret;
    }

#ifdef CONFIG_X86
    pdev_x86 = platform_device_register_simple("glandagpu", -1, 
                                           glandagpu_resources, 
                                           ARRAY_SIZE(glandagpu_resources));
    if (IS_ERR(pdev_x86)) {
        pr_err("GlandaGPU: Failed to register platform device\n");
        platform_driver_unregister(&glandagpu_driver);
        return PTR_ERR(pdev_x86);
    }
#endif

    pr_info("GlandaGPU: Module loaded successfully\n");
    return 0;
}

static void __exit glandagpu_exit(void)
{
#ifdef CONFIG_X86 
    if (pdev_x86)
        platform_device_unregister(pdev_x86);
#endif
    platform_driver_unregister(&glandagpu_driver);
    pr_info("GlandaGPU: Module unloaded\n");
}

module_init(glandagpu_init);
module_exit(glandagpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leander Kieweg <kieweg.leander@gmail.com>");
MODULE_DESCRIPTION("GlandaGPU Hardware Accelerated Driver");