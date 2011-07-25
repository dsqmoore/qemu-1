/* Copyright (C) 2007-2008 The Android Open Source Project
**
** This software is licensed under the terms of the GNU General Public
** License version 2, as published by the Free Software Foundation, and
** may be copied, distributed, and modified under those terms.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/
#include "hw.h"
#include "arm-misc.h"
#include "goldfish_device.h"
#ifdef TARGET_I386
#include "kvm.h"
#endif

#define PDEV_BUS_OP_DONE        (0x00)
#define PDEV_BUS_OP_REMOVE_DEV  (0x04)
#define PDEV_BUS_OP_ADD_DEV     (0x08)

#define PDEV_BUS_OP_INIT        (0x00)

#define PDEV_BUS_OP             (0x00)
#define PDEV_BUS_GET_NAME       (0x04)
#define PDEV_BUS_NAME_LEN       (0x08)
#define PDEV_BUS_ID             (0x0c)
#define PDEV_BUS_IO_BASE        (0x10)
#define PDEV_BUS_IO_SIZE        (0x14)
#define PDEV_BUS_IRQ            (0x18)
#define PDEV_BUS_IRQ_COUNT      (0x1c)

#include "hw/sysbus.h"
static struct BusInfo goldfish_bus_info = {
    .name       = "goldfish-bus",
    .size       = sizeof(GoldfishBus),
};

/*
struct bus_state {
    struct goldfish_device dev;
    struct goldfish_device *current;
};
*/

typedef struct GoldfishDeviceBusDevice {
    GoldfishDevice dev;
} GoldfishDeviceBusDevice;

qemu_irq *goldfish_pic;
static GoldfishDevice *first_device;
static GoldfishDevice *last_device;
uint32_t goldfish_free_base;
uint32_t goldfish_free_irq;

void goldfish_device_set_irq(GoldfishDevice *dev, int irq, int level)
{
    if(irq >= dev->irq_count)
        cpu_abort (cpu_single_env, "goldfish_device_set_irq: Bad irq %d >= %d\n", irq, dev->irq_count);
    else
        qemu_set_irq(goldfish_pic[dev->irq + irq], level);
}

static int goldfish_add_device_no_io(GoldfishDevice *dev)
{
    if(dev->base == 0) {
        dev->base = goldfish_free_base;
        goldfish_free_base += dev->size;
    }
    if(dev->irq == 0 && dev->irq_count > 0) {
        dev->irq = goldfish_free_irq;
        goldfish_free_irq += dev->irq_count;
    }
    //printf("goldfish_add_device: %s, base %x %x, irq %d %d\n",
    //       dev->name, dev->base, dev->size, dev->irq, dev->irq_count);
    dev->next = NULL;
    if(last_device) {
        last_device->next = dev;
    }
    else {
        first_device = dev;
    }
    last_device = dev;
    return 0;
}

static int goldfish_device_add(GoldfishDevice *dev,
                       CPUReadMemoryFunc **mem_read,
                       CPUWriteMemoryFunc **mem_write,
                       void *opaque)
{
    int iomemtype;
    goldfish_add_device_no_io(dev);
    // TODO: make sure that is the correct endian format
    iomemtype = cpu_register_io_memory(mem_read, mem_write, opaque, DEVICE_NATIVE_ENDIAN);
    cpu_register_physical_memory(dev->base, dev->size, iomemtype);
    return 0;
}

static uint32_t goldfish_device_bus_read(void *opaque, target_phys_addr_t offset)
{
    GoldfishBus *s = (GoldfishBus *)opaque;

    switch (offset) {
        case PDEV_BUS_OP:
            if(s->current) {
                s->current->reported_state = 1;
                s->current = s->current->next;
            }
            else {
                s->current = first_device;
            }
            while(s->current && s->current->reported_state == 1)
                s->current = s->current->next;
            if(s->current)
                return PDEV_BUS_OP_ADD_DEV;
            else {
                goldfish_device_set_irq(&s->dev, 0, 0);
                return PDEV_BUS_OP_DONE;
            }

        case PDEV_BUS_NAME_LEN:
            return s->current ? strlen(s->current->name) : 0;
        case PDEV_BUS_ID:
            return s->current ? s->current->id : 0;
        case PDEV_BUS_IO_BASE:
            return s->current ? s->current->base : 0;
        case PDEV_BUS_IO_SIZE:
            return s->current ? s->current->size : 0;
        case PDEV_BUS_IRQ:
            return s->current ? s->current->irq : 0;
        case PDEV_BUS_IRQ_COUNT:
            return s->current ? s->current->irq_count : 0;
    default:
        cpu_abort (cpu_single_env, "goldfish_bus_read: Bad offset %x\n", offset);
        return 0;
    }
}

static void goldfish_device_bus_op_init(GoldfishBus *s)
{
    GoldfishDevice *dev = first_device;
    while(dev) {
        dev->reported_state = 0;
        dev = dev->next;
    }
    s->current = NULL;
    goldfish_device_set_irq(&s->dev, 0, first_device != NULL);
}

static void goldfish_device_bus_write(void *opaque, target_phys_addr_t offset, uint32_t value)
{
    GoldfishBus *s = (GoldfishBus *)opaque;

    switch(offset) {
        case PDEV_BUS_OP:
            switch(value) {
                case PDEV_BUS_OP_INIT:
                    goldfish_device_bus_op_init(s);
                    break;
                default:
                    cpu_abort (cpu_single_env, "goldfish_bus_write: Bad PDEV_BUS_OP value %x\n", value);
            };
            break;
        case PDEV_BUS_GET_NAME:
            if(s->current) {
#ifdef TARGET_I386
                if(kvm_enabled())
                    cpu_synchronize_state(cpu_single_env, 0);
#endif
                cpu_memory_rw_debug(cpu_single_env, value, (void*)s->current->name, strlen(s->current->name), 1);
            }
            break;
        default:
            cpu_abort (cpu_single_env, "goldfish_bus_write: Bad offset %x\n", offset);
    }
}

static CPUReadMemoryFunc *goldfish_device_bus_readfn[] = {
    goldfish_device_bus_read,
    goldfish_device_bus_read,
    goldfish_device_bus_read
};

static CPUWriteMemoryFunc *goldfish_device_bus_writefn[] = {
    goldfish_device_bus_write,
    goldfish_device_bus_write,
    goldfish_device_bus_write
};

/*
static struct bus_state bus_state = {
    .dev = {
        .name = "goldfish_device_bus",
        .id = -1,
        .base = 0x10001000,
        .size = 0x1000,
        .irq = 1,
        .irq_count = 1,
    }
};

void goldfish_device_init(qemu_irq *pic, uint32_t base, uint32_t size, uint32_t irq, uint32_t irq_count)
{
    goldfish_pic = pic;
    goldfish_free_base = base;
    goldfish_free_irq = irq;
}

int goldfish_device_bus_init(uint32_t base, uint32_t irq)
{
    bus_state.dev.base = base;
    bus_state.dev.irq = irq;

    return goldfish_device_add(&bus_state.dev, goldfish_bus_readfn, goldfish_bus_writefn, &bus_state);
}
*/
void goldfish_device_init(qemu_irq *pic, uint32_t base, uint32_t irq)
{
    goldfish_pic = pic;
    goldfish_free_base = base;
    goldfish_free_irq = irq;
}
static int goldfish_device_bus_init(GoldfishDevice *dev)
{
    //GoldfishDeviceBusDevice *tdev = (GoldfishDeviceBusDevice *)dev;

    return 0;
}

DeviceState *goldfish_device_bus_create(GoldfishBus *gbus, uint32_t base, uint32_t irq)
{
    DeviceState *dev;
    char *name = (char *)"goldfish-device-bus";

    dev = qdev_create(&gbus->bus, name);
    qdev_prop_set_string(dev, "name", name);
    qdev_prop_set_uint32(dev, "base", base);
    qdev_prop_set_uint32(dev, "irq", irq);
    qdev_init_nofail(dev);

    return dev;
}

static GoldfishDeviceInfo goldfish_device_bus_info = {
    .init = goldfish_device_bus_init,
    .readfn = goldfish_device_bus_readfn,
    .writefn = goldfish_device_bus_writefn,
    .qdev.name  = "goldfish-device-bus",
    .qdev.size  = sizeof(GoldfishDeviceBusDevice),
    .qdev.props = (Property[]) {
        DEFINE_PROP_UINT32("base", GoldfishDevice, base, 0x10001000),
        DEFINE_PROP_UINT32("id", GoldfishDevice, id, -1),
        DEFINE_PROP_UINT32("size", GoldfishDevice, size, 0x1000),
        DEFINE_PROP_UINT32("irq", GoldfishDevice, irq, 1),
        DEFINE_PROP_UINT32("irq_count", GoldfishDevice, irq_count, 1),
        DEFINE_PROP_STRING("name", GoldfishDevice, name),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void goldfish_device_bus_register(void)
{
    goldfish_bus_register_withprop(&goldfish_device_bus_info);
} device_init(goldfish_device_bus_register);

static int goldfish_busdev_init(DeviceState *qdev, DeviceInfo *qinfo)
{
    GoldfishDeviceInfo *info = (GoldfishDeviceInfo *)qinfo;
    GoldfishDevice *dev = DO_UPCAST(GoldfishDevice, qdev, qdev);
    goldfish_device_add(dev, info->readfn, info->writefn, dev);
/*    char *id;

    if (asprintf(&id, "%s@%x", info->dt_name, dev->reg) < 0) {
        return -1;
    }

    dev->qdev.id = id;
*/
    return info->init(dev);
}

void goldfish_bus_register_withprop(GoldfishDeviceInfo *info)
{
    info->qdev.init = goldfish_busdev_init;
    info->qdev.bus_info = &goldfish_bus_info;

    assert(info->qdev.size >= sizeof(GoldfishDevice));
    qdev_register(&info->qdev);
}

GoldfishBus *goldfish_bus_init(uint32_t base, uint32_t irq)
{
    GoldfishBus *bus;
    BusState *qbus;
    DeviceState *dev;
    //DeviceInfo *qinfo;

    /* Create bridge device */
    dev = qdev_create(NULL, "goldfish-bridge");
    qdev_init_nofail(dev);

    /* Create bus on bridge device */

    qbus = qbus_create(&goldfish_bus_info, dev, "goldfish-bus");
    bus = DO_UPCAST(GoldfishBus, bus, qbus);

    dev = goldfish_device_bus_create(bus, base, irq);
    GoldfishDevice *gdev = DO_UPCAST(GoldfishDevice, qdev, dev);
    bus->dev = *gdev;

    goldfish_device_add(&bus->dev, goldfish_device_bus_readfn, goldfish_device_bus_writefn, bus);

    return bus;
}

static int goldfish_bridge_init(SysBusDevice *dev)
{
    /* nothing */
    return 0;
}

static SysBusDeviceInfo goldfish_bridge_info = {
    .init = goldfish_bridge_init,
    .qdev.name  = "goldfish-bridge",
    .qdev.size  = sizeof(SysBusDevice),
    .qdev.no_user = 1,
};

static void goldfish_register_devices(void)
{
    sysbus_register_withprop(&goldfish_bridge_info);
}

device_init(goldfish_register_devices)

