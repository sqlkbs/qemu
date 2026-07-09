/*
 * This is for MIFE v1 that accesses the CIO-DAS08 ISA card via ISA-passthrough on MBATX-CS620-H310C motherboard from DFI
 */

#include "qemu/osdep.h"  /* CRITICAL: This must always be first! */
#include "qapi/error.h"  /* ADD THIS LINE to fix the implicit declaration error */
#include "hw/qdev.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include "hw/isa/isa.h"          // Required for isa_get_irq()
#include "qemu/thread.h"        // Required for QemuThread infrastructure
#include "qemu/main-loop.h"     // Required for QEMUBH (Bottom Half) main-loop scheduling
#include <sys/io.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>

#define TYPE_MIFE_HOST_ISA_PROXY "mife-host-isa-proxy"

#define MIFE_HOST_ISA_PROXY(obj) \
    OBJECT_CHECK(MifeHostISAProxyState, (obj), TYPE_MIFE_HOST_ISA_PROXY)

typedef struct MifeHostISAProxyState {
    DeviceState parent_obj;
    MemoryRegion ioports;

    /* Configurable fields bound to QEMU properties */
    bool debug;
    uint16_t host_port;
    uint16_t guest_port;
    uint16_t guest_irq;         /* Configurable target Guest IRQ (e.g., 5 or 7) */
    char *host_irq_device;      /* Path to host interrupt engine (e.g., "/dev/uio0") */
    uint16_t log_sample_rate;

    uint64_t log_counter;       // sample count for logging to improve performance

    /* Interrupt Pipeline Mechanics */
    qemu_irq irq;               /* Managed virtual IRQ descriptor pin */
    QemuThread irq_thread;      /* Host hardware loop monitor thread */
    int irq_fd;                 /* Open file handle capturing host events */
    bool thread_running;        /* Execution lifecycle circuit flag */
    QEMUBH *irq_bh;             /* Bottom Half placeholder to synchronize context */
} MifeHostISAProxyState;

/* Safe Main-Loop Context Interrupt Injection */
static void mife_host_isa_proxy_bh_cb(void *opaque)
{
    MifeHostISAProxyState *s = (MifeHostISAProxyState *)opaque;

    if (s->debug) {
        fprintf(stderr, "[IRQ EVENT] MIFE ISA PROXY: Pulse Virtual Guest IRQ %d\n", s->guest_irq);
        fflush(stderr);
    }

    /* Simulate edge-triggered ISA interrupt behavior */
    qemu_set_irq(s->irq, 1);
    qemu_set_irq(s->irq, 0);
}

/* Background Worker Monitoring Physical Motherboard Events */
static void *mife_host_isa_irq_worker(void *opaque)
{
    MifeHostISAProxyState *s = (MifeHostISAProxyState *)opaque;
    uint32_t irq_count_payload; // Standard Linux UIO data container structure

    while (s->thread_running) {
        /* Blocking read waits passively for the physical slot line to signal */
        ssize_t target_bytes = read(s->irq_fd, &irq_count_payload, sizeof(irq_count_payload));

        if (target_bytes > 0) {
            /* Host IRQ detected! Safe-schedule work onto QEMU's central processing loop */
            qemu_bh_schedule(s->irq_bh);
        } else if (target_bytes < 0 && errno != EINTR) {
            if (s->debug) {
                fprintf(stderr, "[IRQ ERROR] Host tracking failed inside listener worker thread loop\n");
            }
            g_usleep(1000); /* Intercept active spinning states during physical line faults */
        }
    }
    return NULL;
}

/* 1. Read Callback */
static uint64_t mife_host_isa_proxy_read(void *opaque, hwaddr addr, unsigned size)
{
    MifeHostISAProxyState *s = (MifeHostISAProxyState *)opaque;

    static __thread bool iopl_unlocked = false;
    if (!iopl_unlocked) {
        if (iopl(3) < 0) return 0xFF;
        iopl_unlocked = true;
    }

    uint8_t value;
    uint16_t target_host_port = s->host_port + addr;

    asm volatile("inb %%dx, %0" : "=a"(value) : "d"(target_host_port));

    if (s->debug) {
        s->log_counter++;
        if (s->log_counter % s->log_sample_rate == 0) {
            fprintf(stderr, "[SAMPLED LOG] MIFE ISA PROXY: Host Port 0x%x -> Val: 0x%02X\n",
                    target_host_port, value);
            fflush(stderr);
        }
    }

    return value;
}

/* 2. Write Callback */
static void mife_host_isa_proxy_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    MifeHostISAProxyState *s = (MifeHostISAProxyState *)opaque;

    static __thread bool iopl_unlocked = false;
    if (!iopl_unlocked) {
        if (iopl(3) < 0) return;
        iopl_unlocked = true;
    }

    uint8_t value = (uint8_t)val;
    uint16_t target_host_port = s->host_port + addr;

    asm volatile("outb %0, %%dx" : : "a"(value), "d"(target_host_port));

    if (s->debug) {
        s->log_counter++;
        if (s->log_counter % s->log_sample_rate == 0) {
            fprintf(stderr, "[SAMPLED LOG] MIFE ISA PROXY: Host Port 0x%x -> Val: 0x%02X\n",
                    target_host_port, value);
            fflush(stderr);
        }
    }
}

/* 3. Memory Operations Glue */
static const MemoryRegionOps mife_host_isa_proxy_ops = {
    .read = mife_host_isa_proxy_read,
    .write = mife_host_isa_proxy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* 4. Device Realization */
static void mife_host_isa_proxy_realize(DeviceState *dev, Error **errp)
{
    MifeHostISAProxyState *s = MIFE_HOST_ISA_PROXY(dev);

    /* Allocate full 16-port layout size matching standard CIO-DAS08 layout definitions */
    memory_region_init_io(&s->ioports, OBJECT(s), &mife_host_isa_proxy_ops, s, "mife-physical-isa-card", 16);
    memory_region_add_subregion(get_system_io(), s->guest_port, &s->ioports);

    /* Construct Virtual System Routing Paths */
    s->irq = isa_get_irq(NULL, s->guest_irq);

    /* Create bottom-half token container keeping asynchronous triggers decoupled from thread limits */
    s->irq_bh = qemu_bh_new(mife_host_isa_proxy_bh_cb, s);

    /* Conditionally initialize async worker if physical path property is actively configured */
    if (s->host_irq_device && s->host_irq_device[0] != '\0') {
        s->irq_fd = open(s->host_irq_device, O_RDONLY);
        if (s->irq_fd < 0) {
            error_setg_errno(errp, errno, "Failed to mount physical host tracking address target '%s'", s->host_irq_device);
            return;
        }

        s->thread_running = true;
        qemu_thread_create(&s->irq_thread, "mife-irq-worker",
                           mife_host_isa_irq_worker, s, QEMU_THREAD_JOINABLE);
    }
}

/* Declare the property structures mapping string keys to variables with defaults */
static Property mife_host_isa_proxy_properties[] = {
    DEFINE_PROP_UINT16("host-port", MifeHostISAProxyState, host_port, 0x5390),
    DEFINE_PROP_UINT16("guest-port", MifeHostISAProxyState, guest_port, 0x390),
    DEFINE_PROP_UINT16("guest-irq", MifeHostISAProxyState, guest_irq, 5),
    DEFINE_PROP_STRING("host-irq-device", MifeHostISAProxyState, host_irq_device),
    DEFINE_PROP_UINT16("log_sample_rate", MifeHostISAProxyState, log_sample_rate, 400),
    DEFINE_PROP_BOOL("debug", MifeHostISAProxyState, debug, false),
    DEFINE_PROP_END_OF_LIST(),
};

/* 5. QOM Class Initializer */
static void mife_host_isa_proxy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = mife_host_isa_proxy_realize;
    dc->user_creatable = true;
    dc->props = mife_host_isa_proxy_properties; 
}

static const TypeInfo mife_host_isa_proxy_info = {
    .name          = TYPE_MIFE_HOST_ISA_PROXY,
    .parent        = TYPE_DEVICE,
    .instance_size = sizeof(MifeHostISAProxyState),
    .class_init    = mife_host_isa_proxy_class_init,
};

static void mife_host_isa_proxy_register_types(void)
{
    type_register_static(&mife_host_isa_proxy_info);
}

type_init(mife_host_isa_proxy_register_types)
