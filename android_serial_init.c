#include "android_serial_init.h"
#include "qemu-common.h"
#include "hw/arm-misc.h"
#include "hw/goldfish_device.h"
#include "sysemu.h"

void android_serial_init(void)
{
    CPUState *env;
    qemu_irq *cpu_pic;
    qemu_irq *goldfish_pic;
    const char* cpu_model = "arm926";

    env = cpu_init(cpu_model);
    cpu_pic = arm_pic_init_cpu(env);
    goldfish_pic = goldfish_interrupt_init(0xff000000, cpu_pic[ARM_PIC_CPU_IRQ], cpu_pic[ARM_PIC_CPU_FIQ]);
    goldfish_device_init(goldfish_pic, 0xff010000, 0x7f0000, 10, 22);
    goldfish_device_bus_init(0xff001000, 1);
    goldfish_tty_add(serial_hds[0], 0, 0xff002000, 4);

}
