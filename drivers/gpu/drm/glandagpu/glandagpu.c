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
#define INT_DONE    (1 << 0)
#define INT_VSYNC   (1 << 1)

#define STATUS_BUSY (1 << 0)
#define CMD_CLEAR   (0x1)
#define CMD_RECT    (0x2)
#define CMD_LINE    (0x3)
#define CTRL_START  (1 << 4)

struct glanda_device {
    struct device *dev;
    void __iomem *vram_base;
    phys_addr_t vram_phys;
    void __iomem *mmio_base;

    int irq;
    wait_queue_head_t cmd_wq;
    bool cmd_done;

    // Char Device
    dev_t cdev_num;
    struct cdev cdev;
    struct class *class;
};

static irqreturn_t glanda_irq_handler(int irq, void *dev_id)
{
    struct glanda_device *gdev = dev_id;
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
static void glanda_hw_draw_rect(struct glanda_device *gdev, 
                                int x, int y, int w, int h, int color)
{
    u32 coord0, coord1, ctrl;

    if (glanda_wait_idle(gdev)) {
        return;
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
}

// Submit Line Command
static void glanda_hw_draw_line(struct glanda_device *gdev, 
                                int x1, int y1, int x2, int y2, int color)
{
    u32 coord0, coord1, ctrl;

    if (glanda_wait_idle(gdev)) {
        return;
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
}

static long glanda_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    struct glanda_device *gdev = file->private_data;
    struct glanda_draw_rect_cmd user_cmd;

    switch (cmd) {
    case GLANDA_IOC_DRAW_RECT:
        if (copy_from_user(&user_cmd, (void __user *)arg, sizeof(user_cmd))) {
            return -EFAULT;
        }

        dev_info(gdev->dev, "IOCTL: Draw Rect %dx%d color %x\n", 
                 user_cmd.w, user_cmd.h, user_cmd.color);
        
        glanda_hw_draw_rect(gdev, user_cmd.x, user_cmd.y, user_cmd.w, user_cmd.h, user_cmd.color);
        break;
    case GLANDA_IOC_DRAW_LINE:
        if (copy_from_user(&user_cmd, (void __user *)arg, sizeof(user_cmd))) {
            return -EFAULT;
        }

        dev_info(gdev->dev, "IOCTL: Draw Line %dx%d color %x\n", 
                 user_cmd.w, user_cmd.h, user_cmd.color);
        
        glanda_hw_draw_line(gdev, user_cmd.x, user_cmd.y, user_cmd.w, user_cmd.h, user_cmd.color);
        break;
    default:
        return -EINVAL;
    }
    return 0;
}

static int glanda_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct glanda_device *gdev = file->private_data;
    unsigned long size = vma->vm_end - vma->vm_start;

    if (size > GLANDA_VRAM_SIZE)
        return -EINVAL;

    // Use non-cached for IO memory
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot); 

    if (remap_pfn_range(vma, vma->vm_start, 
                        gdev->vram_phys >> PAGE_SHIFT, // Convert physical address to PFN (Page Frame Number)
                        size, vma->vm_page_prot)) {
        return -EAGAIN;
    }
    return 0;
}

static int glanda_open(struct inode *inode, struct file *file)
{
    // get device pointer
    struct glanda_device *gdev = container_of(inode->i_cdev, struct glanda_device, cdev);
    file->private_data = gdev; // save for ioctl
    return 0;
}

static const struct file_operations glanda_fops = {
    .owner          = THIS_MODULE,
    .open           = glanda_open,
    .mmap           = glanda_mmap,
    .unlocked_ioctl = glanda_ioctl,
};

static int glandagpu_probe(struct platform_device *pdev)
{
    struct resource *res;
    struct glanda_device *gdev;
    int ret;

    dev_info(&pdev->dev, "GlandaGPU Probe started\n");

    // create device structure
    gdev = devm_kzalloc(&pdev->dev, sizeof(*gdev), GFP_KERNEL);
    if (!gdev) {
        return -ENOMEM;
    }
    gdev->dev = &pdev->dev;
    platform_set_drvdata(pdev, gdev);

    // Interrupt setup
    init_waitqueue_head(&gdev->cmd_wq);
    gdev->irq = -1;

    // Fetch IRQ
    ret = platform_get_irq(pdev, 0);
    if (ret > 0) {
        gdev->irq = ret;
        ret = devm_request_irq(&pdev->dev, gdev->irq, glanda_irq_handler,
                               IRQF_SHARED, "glandagpu", gdev);
        if (ret) {
            dev_err(&pdev->dev, "Failed to request IRQ %d\n", gdev->irq);
            return ret;
        }
        dev_info(&pdev->dev, "IRQ %d requested successfully\n", gdev->irq);
    } else {
        dev_warn(&pdev->dev, "No IRQ found, driver will fall back to polling\n");
    }

    // Map VRAM
    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    if (!res) {
        dev_err(&pdev->dev, "Failed to get VRAM resource\n");
        return -ENODEV;
    }
    gdev->vram_phys = res->start;
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

    writel(0, gdev->mmio_base + REG_IER);
    writel(INT_DONE | INT_VSYNC, gdev->mmio_base + REG_ISR);
    
    if (gdev->irq >= 0) {
        // Enable Done Interrupt
        writel(INT_DONE, gdev->mmio_base + REG_IER);
    }

    // rendering Test
    dev_info(&pdev->dev, "Start Test\n");

    // Clear screen (CPU)
    memset_io(gdev->vram_base, 0, GLANDA_VRAM_SIZE); 

    // Char Device
    ret = alloc_chrdev_region(&gdev->cdev_num, 0, 1, "glandagpu");
    if (ret < 0) {
        dev_err(&pdev->dev, "Failed to alloc chrdev region\n");
        return ret;
    }

    cdev_init(&gdev->cdev, &glanda_fops);
    gdev->cdev.owner = THIS_MODULE;

    ret = cdev_add(&gdev->cdev, gdev->cdev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(gdev->cdev_num, 1);
        return ret;
    }

    // Create sysfs class and trigger udev to automatically create /dev/glandagpu
    gdev->class = class_create("glanda_class");
    if (IS_ERR(gdev->class)) {
        cdev_del(&gdev->cdev);
        unregister_chrdev_region(gdev->cdev_num, 1);
        return PTR_ERR(gdev->class);
    }
    
    device_create(gdev->class, NULL, gdev->cdev_num, NULL, "glandagpu");

    dev_info(&pdev->dev, "GlandaGPU Initialized /dev/glandagpu created\n");
    return 0;
}

static void glandagpu_remove(struct platform_device *pdev)
{
    struct glanda_device *gdev = platform_get_drvdata(pdev);

    // Disable interrupts
    writel(0, gdev->mmio_base + REG_IER);

    // Clean up Char Device
    device_destroy(gdev->class, gdev->cdev_num);
    class_destroy(gdev->class);
    cdev_del(&gdev->cdev);
    unregister_chrdev_region(gdev->cdev_num, 1);

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
    [2] = { // IRQ
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