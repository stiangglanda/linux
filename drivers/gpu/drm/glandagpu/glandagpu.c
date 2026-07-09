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
#include <linux/mutex.h>
#include <linux/iosys-map.h>

#include <drm/drm_drv.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_vblank.h>

#include <drm/drm_connector.h>
#include <drm/drm_encoder.h>
#include <drm/drm_modeset_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_modeset_helper_vtables.h>
#include <drm/drm_plane.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>

#include "../../../../include/uapi/drm/glanda_drm.h"

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

    struct drm_plane primary_plane;

    struct drm_crtc crtc;
    struct drm_encoder encoder;
    struct drm_connector connector;
};

#define to_glanda(dev) container_of(dev, struct glanda_device, drm)

static const uint32_t glanda_plane_formats[] = {
    DRM_FORMAT_XRGB8888,
};

static int glanda_wait_idle(struct glanda_device *gdev)
{
    int ret;
    unsigned int status;

    status = readl(gdev->mmio_base + REG_STATUS);
    if (!(status & STATUS_BUSY)) {
        return 0;
    }

    if (gdev->irq < 0) {
        int timeout = 10000;
        do {
            status = readl(gdev->mmio_base + REG_STATUS);
            if (!(status & STATUS_BUSY))
                return 0;
            udelay(1);
        } while (--timeout > 0);

        dev_err(gdev->dev, "GlandaGPU: Polled wait_idle timeout\n");
        return -ETIMEDOUT;
    }

    gdev->cmd_done = false;

    ret = wait_event_interruptible_timeout(
        gdev->cmd_wq,
        gdev->cmd_done || !(readl(gdev->mmio_base + REG_STATUS) & STATUS_BUSY),
        msecs_to_jiffies(500));

    if (ret == 0) {
        dev_err(gdev->dev, "GlandaGPU: IRQ wait_idle timeout\n");
        return -ETIMEDOUT;
    } else if (ret < 0) {
        return ret;
    }

    return 0;
}

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
    ctrl = CTRL_START | CMD_CLEAR;
    writel(ctrl, gdev->mmio_base + REG_CTRL);

    mutex_unlock(&gdev->lock);
    return 0;
}

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

    coord0 = (y << 16) | (x & 0x3FF);
    coord1 = (h << 16) | (w & 0x3FF);

    writel(coord0, gdev->mmio_base + REG_COORD0);
    writel(coord1, gdev->mmio_base + REG_COORD1);
    writel(color,  gdev->mmio_base + REG_COLOR);

    ctrl = CTRL_START | CMD_RECT;
    writel(ctrl, gdev->mmio_base + REG_CTRL);

    mutex_unlock(&gdev->lock);
    return 0;
}

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

    coord0 = (y1 << 16) | (x1 & 0x3FF);
    coord1 = (y2 << 16) | (x2 & 0x3FF);

    writel(coord0, gdev->mmio_base + REG_COORD0);
    writel(coord1, gdev->mmio_base + REG_COORD1);
    writel(color,  gdev->mmio_base + REG_COLOR);

    ctrl = CTRL_START | CMD_LINE;
    writel(ctrl, gdev->mmio_base + REG_CTRL);

    mutex_unlock(&gdev->lock);
    return 0;
}

static int glanda_drm_ioctl_clear(struct drm_device *dev, void *data,
                                  struct drm_file *file_priv)
{
    struct glanda_device *gdev = to_glanda(dev);
    struct glanda_clear_cmd *cmd = data;

    return glanda_hw_clear(gdev, cmd->color);
}

static int glanda_drm_ioctl_draw_rect(struct drm_device *dev, void *data,
                                      struct drm_file *file_priv)
{
    struct glanda_device *gdev = to_glanda(dev);
    struct glanda_draw_rect_cmd *cmd = data;

    if (cmd->x >= GLANDA_WIDTH || cmd->y >= GLANDA_HEIGHT ||
        cmd->w > GLANDA_WIDTH || cmd->h > GLANDA_HEIGHT ||
        cmd->x + cmd->w > GLANDA_WIDTH ||
        cmd->y + cmd->h > GLANDA_HEIGHT) {
        return -EINVAL;
    }

    return glanda_hw_draw_rect(gdev, cmd->x, cmd->y, cmd->w, cmd->h, cmd->color);
}

static int glanda_drm_ioctl_draw_line(struct drm_device *dev, void *data,
                                      struct drm_file *file_priv)
{
    struct glanda_device *gdev = to_glanda(dev);
    struct glanda_draw_line_cmd *cmd = data;

    if (cmd->x0 >= GLANDA_WIDTH || cmd->y0 >= GLANDA_HEIGHT ||
        cmd->x1 >= GLANDA_WIDTH || cmd->y1 >= GLANDA_HEIGHT) {
        return -EINVAL;
    }

    return glanda_hw_draw_line(gdev, cmd->x0, cmd->y0, cmd->x1, cmd->y1, cmd->color);
}


static void glanda_plane_atomic_update(struct drm_plane *plane,
                                       struct drm_atomic_state *state)
{
    struct drm_plane_state *new_state = drm_atomic_get_new_plane_state(state, plane);
    struct drm_framebuffer *fb = new_state->fb;
    struct glanda_device *gdev = to_glanda(plane->dev);
    struct drm_gem_shmem_object *shmem;
    struct iosys_map map;
    int ret;

    if (!fb)
        return;

    shmem = to_drm_gem_shmem_obj(fb->obj[0]);
    if (!shmem)
        return;

    ret = drm_gem_shmem_vmap(shmem, &map);
    if (ret) {
        dev_err(gdev->dev, "Fehler beim vmap des GEM Shmem Objekts\n");
        return;
    }

    {
        uint32_t *src = (uint32_t *)map.vaddr;
        uint16_t __iomem *dst = (uint16_t __iomem *)gdev->vram_base;
        int i;

        for (i = 0; i < GLANDA_WIDTH * GLANDA_HEIGHT; i++) {
            uint32_t pixel = src[i];

            uint16_t packed = ((pixel >> 12) & 0x0F00) | // red
                              ((pixel >> 8)  & 0x00F0) | // green
                              ((pixel >> 4)  & 0x000F);  // blue

            writew(packed, &dst[i]);
        }
    }

    drm_gem_shmem_vunmap(shmem, &map);
}

static const struct drm_plane_helper_funcs glanda_plane_helper_funcs = {
    .atomic_update = glanda_plane_atomic_update,
};

static const struct drm_plane_funcs glanda_plane_funcs = {
    .update_plane           = drm_atomic_helper_update_plane,
    .disable_plane          = drm_atomic_helper_disable_plane,
    .destroy                = drm_plane_cleanup,
    .reset                  = drm_atomic_helper_plane_reset,
    .atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state,
    .atomic_destroy_state   = drm_atomic_helper_plane_destroy_state,
};

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

    return 1;
}

static enum drm_connector_status glanda_connector_detect(struct drm_connector *connector, bool force)
{
    return connector_status_connected;
}

static int glanda_crtc_enable_vblank(struct drm_crtc *crtc)
{
    struct glanda_device *gdev = to_glanda(crtc->dev);
    u32 ier;

    ier = readl(gdev->mmio_base + REG_IER);
    writel(ier | INT_VSYNC, gdev->mmio_base + REG_IER);

    return 0;
}

static void glanda_crtc_disable_vblank(struct drm_crtc *crtc)
{
    struct glanda_device *gdev = to_glanda(crtc->dev);
    u32 ier = readl(gdev->mmio_base + REG_IER);

    writel(ier & ~INT_VSYNC, gdev->mmio_base + REG_IER);
}

static void glanda_crtc_atomic_enable(struct drm_crtc *crtc,
                                      struct drm_atomic_state *state)
{
    drm_crtc_vblank_on(crtc);
}

static void glanda_crtc_atomic_disable(struct drm_crtc *crtc,
                                       struct drm_atomic_state *state)
{
    drm_crtc_vblank_off(crtc);
}

static void glanda_crtc_atomic_flush(struct drm_crtc *crtc,
                                     struct drm_atomic_state *state)
{
    struct drm_crtc_state *new_state = drm_atomic_get_new_crtc_state(state, crtc);
    struct drm_pending_vblank_event *event;

    if (new_state && new_state->event) {
        event = new_state->event;
        
        new_state->event = NULL;

        spin_lock_irq(&crtc->dev->event_lock);
        
        if (drm_crtc_vblank_get(crtc) == 0) {
            drm_crtc_arm_vblank_event(crtc, event);
        } else {
            drm_crtc_send_vblank_event(crtc, event);
        }
        
        spin_unlock_irq(&crtc->dev->event_lock);
    }
}

static const struct drm_crtc_funcs glanda_crtc_funcs = {
    .destroy                = drm_crtc_cleanup,
    .set_config             = drm_atomic_helper_set_config,
    .page_flip              = drm_atomic_helper_page_flip,
    .reset                  = drm_atomic_helper_crtc_reset,
    .atomic_duplicate_state = drm_atomic_helper_crtc_duplicate_state,
    .atomic_destroy_state   = drm_atomic_helper_crtc_destroy_state,
    .enable_vblank          = glanda_crtc_enable_vblank,
    .disable_vblank         = glanda_crtc_disable_vblank,
};

static const struct drm_crtc_helper_funcs glanda_crtc_helper_funcs = {
    .atomic_enable  = glanda_crtc_atomic_enable,
    .atomic_disable = glanda_crtc_atomic_disable,
    .atomic_flush   = glanda_crtc_atomic_flush,
};

static const struct drm_connector_helper_funcs glanda_connector_helper_funcs = {
    .get_modes = glanda_connector_get_modes,
};

static const struct drm_connector_funcs glanda_connector_funcs = {
    .fill_modes             = drm_helper_probe_single_connector_modes,
    .destroy                = drm_connector_cleanup,
    .detect                 = glanda_connector_detect,
    .reset                  = drm_atomic_helper_connector_reset,
    .atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
    .atomic_destroy_state   = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_mode_config_funcs glanda_mode_config_funcs = {
    .fb_create      = drm_gem_fb_create, 
    .atomic_check   = drm_atomic_helper_check,
    .atomic_commit  = drm_atomic_helper_commit,
};

static const struct drm_ioctl_desc glanda_ioctls[] = {
    DRM_IOCTL_DEF_DRV(GLANDA_CLEAR, glanda_drm_ioctl_clear, DRM_AUTH | DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF_DRV(GLANDA_DRAW_RECT, glanda_drm_ioctl_draw_rect, DRM_AUTH | DRM_RENDER_ALLOW),
    DRM_IOCTL_DEF_DRV(GLANDA_DRAW_LINE, glanda_drm_ioctl_draw_line, DRM_AUTH | DRM_RENDER_ALLOW),
};

DEFINE_DRM_GEM_FOPS(glanda_drm_fops);

static const struct drm_driver glanda_drm_driver = {
    .driver_features    = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
    .name               = "glandagpu",
    .desc               = "GlandaGPU Hardware Accelerated DRM Driver",
    .major              = 1,
    .minor              = 0,
    .fops               = &glanda_drm_fops,
    .dumb_create        = drm_gem_shmem_dumb_create,
    .ioctls             = glanda_ioctls,
    .num_ioctls         = ARRAY_SIZE(glanda_ioctls),
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

    if (isr & INT_VSYNC) {
        drm_crtc_handle_vblank(&gdev->crtc);
    }

    // Clear interrupt(W1C)
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
    writel(0xFFFFFFFF, gdev->mmio_base + REG_ISR); //clear flags

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

    // DRM mode config
    drm_mode_config_init(&gdev->drm);
    gdev->drm.mode_config.min_width = 640;
    gdev->drm.mode_config.min_height = 480;
    gdev->drm.mode_config.max_width = 640;
    gdev->drm.mode_config.max_height = 480;
    gdev->drm.mode_config.funcs = &glanda_mode_config_funcs;

    ret = drm_universal_plane_init(&gdev->drm, &gdev->primary_plane, 1 << 0,
                                   &glanda_plane_funcs,
                                   glanda_plane_formats, ARRAY_SIZE(glanda_plane_formats),
                                   NULL, DRM_PLANE_TYPE_PRIMARY, NULL);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize primary plane\n");
        goto err_mode_cleanup;
    }
    drm_plane_helper_add(&gdev->primary_plane, &glanda_plane_helper_funcs);

    // VBlank init
    ret = drm_vblank_init(&gdev->drm, 1);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize vblank\n");
        goto err_mode_cleanup;
    }

    // CRTC init
    ret = drm_crtc_init_with_planes(&gdev->drm, &gdev->crtc,
                                    &gdev->primary_plane, NULL,
                                    &glanda_crtc_funcs, NULL);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize CRTC with planes\n");
        goto err_mode_cleanup;
    }
    drm_crtc_helper_add(&gdev->crtc, &glanda_crtc_helper_funcs);

    // Encoder init
    ret = drm_simple_encoder_init(&gdev->drm, &gdev->encoder, DRM_MODE_ENCODER_DAC);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize encoder\n");
        goto err_mode_cleanup;
    }
    gdev->encoder.possible_crtcs = 1; 

    // Connector init
    ret = drm_connector_init(&gdev->drm, &gdev->connector, &glanda_connector_funcs, DRM_MODE_CONNECTOR_VGA);
    if (ret) {
        dev_err(&pdev->dev, "Failed to initialize connector\n");
        goto err_mode_cleanup;
    }
    drm_connector_helper_add(&gdev->connector, &glanda_connector_helper_funcs);

    drm_connector_attach_encoder(&gdev->connector, &gdev->encoder);

    //important for sysfs
    mutex_lock(&gdev->drm.mode_config.mutex);
    drm_helper_probe_single_connector_modes(&gdev->connector, 1024, 768);
    mutex_unlock(&gdev->drm.mode_config.mutex);

    drm_mode_config_reset(&gdev->drm);

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