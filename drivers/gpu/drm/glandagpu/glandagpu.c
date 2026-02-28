#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <linux/delay.h>      // udelay (polling)
#include <linux/mod_devicetable.h> // Device Tree parsing
#include <linux/of.h>
#include <linux/slab.h>  // GFP_KERNEL

// Hardware Constants
#define GLANDA_VRAM_BASE  0x40000000
#define GLANDA_VRAM_SIZE  (640 * 480 * 4)
#define GLANDA_MMIO_BASE  0x40200000
#define GLANDA_MMIO_SIZE  32

// Register Offsets
#define REG_STATUS  0x00
#define REG_CTRL    0x04
#define REG_COORD0  0x08
#define REG_COORD1  0x0C
#define REG_COLOR   0x10
#define REG_ISR     0x14
#define REG_IER     0x18

// Bit Masks
#define STATUS_BUSY (1 << 0)
#define CMD_CLEAR   (0x1)
#define CMD_RECT    (0x2)
#define CMD_LINE    (0x3)
#define CTRL_START  (1 << 4)

struct glanda_device {
    struct device *dev;
    void __iomem *vram_base;
    void __iomem *mmio_base;
};

// helper function to wait until hardware is idle (polling) //TODO remove when interrupt support is added
static int glanda_wait_idle(struct glanda_device *gdev)
{
    unsigned int status;
    int timeout = 10000;

    do {
        status = readl(gdev->mmio_base + REG_STATUS);
        if (!(status & STATUS_BUSY))
            return 0;
        
        udelay(1);
    } while (--timeout > 0);

    dev_err(gdev->dev, "GlandaGPU: glanda_wait_idle timed out\n");
    return -ETIMEDOUT;
}

// Submit Rectangle Command
static void glanda_hw_draw_rect(struct glanda_device *gdev, 
                                int x, int y, int w, int h, int color)
{
    u32 coord0, coord1, ctrl;

    if (glanda_wait_idle(gdev)) return;

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
}

// Submit Line Command
static void glanda_hw_draw_line(struct glanda_device *gdev, 
                                int x1, int y1, int x2, int y2, int color)
{
    u32 coord0, coord1, ctrl;

    if (glanda_wait_idle(gdev)) return;

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
}

static int glandagpu_probe(struct platform_device *pdev)
{
    struct resource *res;
    struct glanda_device *gdev;

    dev_info(&pdev->dev, "GlandaGPU Probe started\n");

    // create device structure
    gdev = devm_kzalloc(&pdev->dev, sizeof(*gdev), GFP_KERNEL);
    if (!gdev) {
        return -ENOMEM;
    }
    gdev->dev = &pdev->dev;
    platform_set_drvdata(pdev, gdev);

    // Map VRAM
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(&pdev->dev, "Failed to get VRAM resource\n");
        return -ENODEV;
    }
    gdev->vram_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!gdev->vram_base) {
        return -ENOMEM;
    }
    dev_info(&pdev->dev, "VRAM mapped at 0x%p\n", gdev->vram_base);

    // Map MMIO
    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    gdev->mmio_base = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!gdev->mmio_base) {
        return -ENOMEM;
    }
    dev_info(&pdev->dev, "MMIO mapped at 0x%p\n", gdev->mmio_base);

    // rendering Test
    dev_info(&pdev->dev, "Start Test\n");

    // Clear screen (CPU)
    memset_io(gdev->vram_base, 0, GLANDA_VRAM_SIZE); 

    // Draw Red Rectangle
    glanda_hw_draw_rect(gdev, 100, 100, 200, 150, 0xF00);

    // Draw Yellow Rectangle
    glanda_hw_draw_rect(gdev, 150, 150, 50, 50, 0xFF0);

    // Draw Green Line
    glanda_hw_draw_line(gdev, 150, 150, 50, 50, 0x0F0);

    return 0;
}

static void glandagpu_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "Driver removed\n");
}

// Device Tree Match
static const struct of_device_id glanda_of_match[] = {
    { .compatible = "glanda,glandagpu", },
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
    [0] = { // VRAM
        .start = GLANDA_VRAM_BASE,
        .end   = GLANDA_VRAM_BASE + GLANDA_VRAM_SIZE - 1,
        .flags = IORESOURCE_MEM,
    },
    [1] = { // MMIO
        .start = GLANDA_MMIO_BASE,
        .end   = GLANDA_MMIO_BASE + GLANDA_MMIO_SIZE - 1,
        .flags = IORESOURCE_MEM,
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