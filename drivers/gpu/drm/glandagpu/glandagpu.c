#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/platform_device.h>
#include <linux/io.h>
#include <drm/drm_print.h>

// Hardware Constants
#define GLANDA_VRAM_BASE  0x40000000
#define GLANDA_VRAM_SIZE  (640 * 480 * 4)
#define GLANDA_MMIO_BASE  0x40200000
#define GLANDA_MMIO_SIZE  32

static struct platform_device *pdev;
static void __iomem *vram_virt;
static void __iomem *mmio_virt;

static int glandagpu_probe(struct platform_device *pdev)
{
    struct resource *res;
    int i;
    u32 *pixel_ptr;

    DRM_DEV_INFO(&pdev->dev, "Probe started!\n");

    // Map VRAM
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        DRM_DEV_ERROR(&pdev->dev, "Failed to get VRAM resource\n");
        return -ENODEV;
    }
    
    vram_virt = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!vram_virt) {
        DRM_DEV_ERROR(&pdev->dev, "Failed to map VRAM\n");
        return -ENOMEM;
    }
    DRM_DEV_INFO(&pdev->dev, "VRAM mapped at 0x%p (Phys: 0x%llx)\n", vram_virt, res->start);

    // Map MMIO
    res = platform_get_resource(pdev, IORESOURCE_MEM, 1);
    mmio_virt = devm_ioremap(&pdev->dev, res->start, resource_size(res));
    if (!mmio_virt) {
        DRM_DEV_ERROR(&pdev->dev, "Failed to map MMIO\n");
        return -ENOMEM;
    }
    DRM_DEV_INFO(&pdev->dev, "MMIO mapped at 0x%p\n", mmio_virt);

    // Paint screen blue Test
    DRM_DEV_INFO(&pdev->dev, "Painting screen blue\n");
    pixel_ptr = (u32 *)vram_virt;
    for (i = 0; i < (640 * 480); i++) {
        writel(0x000000FF, &pixel_ptr[i]);
    }

    return 0;
}

static void glandagpu_remove(struct platform_device *pdev)
{
    DRM_DEV_INFO(&pdev->dev, "Driver removed\n");
}

static struct platform_driver glandagpu_driver = {
    .driver = {
        .name = "glandagpu",
    },
    .probe = glandagpu_probe,
    .remove = glandagpu_remove,
};

// Device Registration only for x86 TODO use device tree for ARM
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

static int __init glandagpu_init(void)
{
    int ret;

    // Register Device only for x86 TODO use device tree for ARM
    pdev = platform_device_register_simple("glandagpu", -1, 
                                           glandagpu_resources, 
                                           ARRAY_SIZE(glandagpu_resources));
    if (IS_ERR(pdev)) {
        pr_err("GlandaGPU: Failed to register platform device\n");
        return PTR_ERR(pdev);
    }

    // Register Driver
    ret = platform_driver_register(&glandagpu_driver);
    if (ret) {
        platform_device_unregister(pdev);
        pr_err("GlandaGPU: Failed to register platform driver\n");
        return ret;
    }

    pr_info("GlandaGPU: Module loaded successfully\n");
    return 0;
}

static void __exit glandagpu_exit(void)
{
    platform_driver_unregister(&glandagpu_driver);
    platform_device_unregister(pdev);
    pr_info("GlandaGPU: Module unloaded\n");
}

module_init(glandagpu_init);
module_exit(glandagpu_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Leander Kieweg <kieweg.leander@gmail.com>");
MODULE_DESCRIPTION("GlandaGPU Driver");