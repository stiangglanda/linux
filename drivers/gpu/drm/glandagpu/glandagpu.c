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

//static struct glanda_device *g_gdev = NULL;

struct glanda_device {
    struct drm_device drm;
    void __iomem *mmio_base;    // Pointer (4 Byte)
    void __iomem *vram_base;    // Pointer zuerst (4 Byte)
    struct device *dev;         // Pointer (4 Byte)
    phys_addr_t vram_phys;      // phys_addr_t kann 4 oder 8 Byte sein -> ans Ende!
    
    int irq;
    wait_queue_head_t cmd_wq;
    bool cmd_done;

    struct mutex lock;
};

#define to_glanda(dev) container_of(dev, struct glanda_device, drm)

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
    .driver_features    = DRIVER_GEM,
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
    } // TODO Handle VSYNC interrupt

    // Clear interrupt(W1C)
    writel(isr, gdev->mmio_base + REG_ISR);

    return IRQ_HANDLED;
}

/*

// helper function to wait until hardware is idle
static int glanda_wait_idle(struct glanda_device *gdev)
{
    int ret;
    unsigned int status;

    status = readl(gdev->mmio_base + REG_STATUS);
    if (!(status & STATUS_BUSY)) {
        return 0;
    }

    // polling
    if (gdev->irq < 0) {
        int timeout = 10000;

        do {
            status = readl(gdev->mmio_base + REG_STATUS);
            if (!(status & STATUS_BUSY)) {
                return 0;
            }
            udelay(1);
        } while (--timeout > 0);

        dev_err(gdev->dev, "GlandaGPU: glanda_wait_idle polled timeout\n");
        return -ETIMEDOUT;
    }

    gdev->cmd_done = false;

    ret = wait_event_interruptible_timeout(
        gdev->cmd_wq,
        gdev->cmd_done || !(readl(gdev->mmio_base + REG_STATUS) & STATUS_BUSY),
        msecs_to_jiffies(500)); // 500ms timeout

    if (ret == 0) {
        dev_err(gdev->dev, "GlandaGPU: glanda_wait_idle IRQ timeout\n");
        return -ETIMEDOUT;
    } else if (ret < 0) {
        return ret; // Interrupted by signal
    }

    return 0;
}

// Submit Rectangle Command
static int glanda_hw_draw_rect(struct glanda_device *gdev, 
                                int x, int y, int w, int h, int color)
{
    u32 coord0, coord1, ctrl;
    int ret;

    if (mutex_lock_interruptible(&gdev->lock)) {
        return -ERESTARTSYS;
    }

    ret = glanda_wait_idle(gdev);
    if (ret) {
        mutex_unlock(&gdev->lock);
        return ret;
    }

    //compact coordinates into 32-bit
    coord0 = (y << 16) | (x & 0x3FF);
    coord1 = (h << 16) | (w & 0x3FF);

    writel(coord0, gdev->mmio_base + REG_COORD0);
    writel(coord1, gdev->mmio_base + REG_COORD1);
    writel(color,  gdev->mmio_base + REG_COLOR);

    // start command
    ctrl = CTRL_START | CMD_RECT;
    writel(ctrl, gdev->mmio_base + REG_CTRL);
    
    dev_info(gdev->dev, "CMD Sent: Rect at %d,%d size %dx%d color 0x%x\n", x,y,w,h,color);

    mutex_unlock(&gdev->lock);
    return 0;
}

// Submit Line Command
static int glanda_hw_draw_line(struct glanda_device *gdev, 
                                int x1, int y1, int x2, int y2, int color)
{
    u32 coord0, coord1, ctrl;
    int ret;

    if (mutex_lock_interruptible(&gdev->lock)) {
        return -ERESTARTSYS;
    }

    ret = glanda_wait_idle(gdev);
    if (ret) {
        mutex_unlock(&gdev->lock);
        return ret;
    }

    //compact coordinates into 32-bit
    coord0 = (y1 << 16) | (x1 & 0x3FF);
    coord1 = (y2 << 16) | (x2 & 0x3FF);

    writel(coord0, gdev->mmio_base + REG_COORD0);
    writel(coord1, gdev->mmio_base + REG_COORD1);
    writel(color,  gdev->mmio_base + REG_COLOR);

    // start command
    ctrl = CTRL_START | CMD_LINE;
    writel(ctrl, gdev->mmio_base + REG_CTRL);
    
    dev_info(gdev->dev, "CMD Sent: Line from (%d,%d) to (%d,%d) color 0x%x\n", x1, y1, x2, y2, color);
    mutex_unlock(&gdev->lock);
    return 0;
}

// Submit Clear Screen Command
static int glanda_hw_clear(struct glanda_device *gdev, int color)
{
    u32 ctrl;
    int ret;

    if (mutex_lock_interruptible(&gdev->lock)) {
        return -ERESTARTSYS;
    }

    ret = glanda_wait_idle(gdev);
    if (ret) {
        mutex_unlock(&gdev->lock);
        return ret;
    }

    writel(color, gdev->mmio_base + REG_COLOR);

    // start command
    ctrl = CTRL_START | CMD_CLEAR;
    writel(ctrl, gdev->mmio_base + REG_CTRL);
    
    dev_info(gdev->dev, "CMD Sent: Clear Screen color 0x%x\n", color);
    mutex_unlock(&gdev->lock);
    return 0;
}
*/

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

    // Interrupt setup
    init_waitqueue_head(&gdev->cmd_wq);
    gdev->irq = -1;

    // Map VRAM
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) return -ENODEV;
    gdev->vram_phys = res->start;
    gdev->vram_base = devm_ioremap(&pdev->dev, res->start, GLANDA_VRAM_SIZE);
    gdev->mmio_base = devm_ioremap(&pdev->dev, res->start + GLANDA_MMIO_OFFSET, GLANDA_MMIO_SIZE);
    
    if (!gdev->vram_base || !gdev->mmio_base) return -ENOMEM;

    writel(0, gdev->mmio_base + REG_IER);
    writel(0xFFFFFFFF, gdev->mmio_base + REG_ISR); // Alle alten Flags löschen

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

    ret = drm_dev_register(&gdev->drm, 0);
    if (ret) {
        dev_err(&pdev->dev, "Failed to register DRM device\n");
        return ret;
    }

    dev_info(&pdev->dev, "GlandaGPU DRM Initialized (/dev/dri/cardX created)\n");
    return 0;
}

static void glandagpu_remove(struct platform_device *pdev)
{
    struct glanda_device *gdev = platform_get_drvdata(pdev);

    drm_dev_unregister(&gdev->drm);

    // Disable interrupts
    writel(0, gdev->mmio_base + REG_IER);

    dev_info(&pdev->dev, "GlandaGPU DRM Driver removed\n");
    
}

// Device Tree Match
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

// Device Registration only for x86 TODO use device tree for ARM
#ifdef CONFIG_X86
static struct platform_device *pdev_x86;

static struct resource glandagpu_resources[] = {
    [0] = { // Single Resource covering VRAM and MMIO
        .start = BRIDGE_BASE,
        .end   = GLANDA_BASE_SIZE, // Size from DTS
        .flags = IORESOURCE_MEM,
    },
    [1] = { // IRQ
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

    // Device Registration only for x86 TODO use device tree for ARM
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
#ifdef CONFIG_X86 // Device Registration only for x86 TODO use device tree for ARM
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