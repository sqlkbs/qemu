/*
* This is for MIFE v1 that access the CIO-DAS08 ISA card via ISA-passthrough on MBATX-CS620-H310C motherboard from DFI
*/

#include "qemu/osdep.h"
#include "hw/qdev.h"
#include "hw/qdev-properties.h" // Required for DEFINE_PROP macros
#include "qemu/module.h"
#include "exec/address-spaces.h"
#include "exec/memory.h"
#include <sys/io.h>
#include <stdbool.h>

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
    uint64_t log_counter;	//sample count for logging to improve performance
} MifeHostISAProxyState;

/* 1. Read Callback */
static uint64_t mife_host_isa_proxy_read(void *opaque, hwaddr addr, unsigned size)
{
    // Cast the opaque pointer back to our state structure
    MifeHostISAProxyState *s = (MifeHostISAProxyState *)opaque;
    
    static __thread bool iopl_unlocked = false;
    if (!iopl_unlocked) {
        if (iopl(3) < 0) return 0xFF;
        iopl_unlocked = true;
    }
    
    uint8_t value;
    uint16_t target_host_port = s->host_port + addr;
    
    asm volatile("inb %%dx, %0" : "=a"(value) : "d"(target_host_port));
    
    // Conditional debugging flag
	if (s->debug) {
		s->log_counter++;
		// Only print once every 400 port interactions
		if (s->log_counter % 400 == 0) {
			fprintf(stderr, "[SAMPLED LOG] MIFE ISA PROXY: Host Port 0x%x -> Val: 0x%02X\n", 
					target_host_port, value);
			fflush(stderr); // Safe to keep here because it only fires occasionally
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
		// Only print once every 400 port interactions
		if (s->log_counter % 400 == 0) {
			fprintf(stderr, "[SAMPLED LOG] MIFE ISA PROXY: Host Port 0x%x -> Val: 0x%02X\n", 
					target_host_port, value);
			fflush(stderr); // Safe to keep here because it only fires occasionally
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

    // Pass 's' as the opaque object parameter so callbacks can read properties
    memory_region_init_io(&s->ioports, OBJECT(s), &mife_host_isa_proxy_ops, s, "mife-physical-isa-card", 16);
    
    // Bind dynamically to the configured guest_port instead of a hardcoded 0x390
    memory_region_add_subregion(get_system_io(), s->guest_port, &s->ioports);
}

/* Declare the property structures mapping string keys to variables with defaults */
static Property mife_host_isa_proxy_properties[] = {
    DEFINE_PROP_BOOL("debug", MifeHostISAProxyState, debug, false),
    DEFINE_PROP_UINT16("host-port", MifeHostISAProxyState, host_port, 0x5390),
    DEFINE_PROP_UINT16("guest-port", MifeHostISAProxyState, guest_port, 0x390),
    DEFINE_PROP_END_OF_LIST(),
};

/* 5. QOM Class Initializer */
static void mife_host_isa_proxy_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = mife_host_isa_proxy_realize;
    dc->user_creatable = true; 
    dc->props = mife_host_isa_proxy_properties; // Inject the properties array into the device class
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
