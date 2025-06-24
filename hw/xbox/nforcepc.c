/*
 * QEMU nForce PC emulation
 *
 * Based on Xbox machine implementation
 * Copyright (c) 2013 espes
 * Copyright (c) 2018-2021 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/loader.h"
#include "hw/i386/pc.h"
#include "hw/i386/x86.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_ids.h"
#include "hw/usb.h"
#include "net/net.h"
#include "hw/boards.h"
#include "hw/ide/pci.h"
#include "hw/dma/i8257.h"
#include "sysemu/sysemu.h"
#include "sysemu/kvm.h"
#include "hw/sysbus.h"
#include "sysemu/arch_init.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "cpu.h"
#include "qapi/error.h"
#include "qemu/error-report.h"

#include "hw/timer/i8254.h"
#include "hw/audio/pcspk.h"
#include "hw/rtc/mc146818rtc.h"

#include "hw/xbox/xbox_pci.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/smbus_eeprom.h"

#include "hw/xbox/xbox.h"
#include "smbus.h"

#define TYPE_NFORCEPC_MACHINE MACHINE_TYPE_NAME("nforcepc")

#define NFORCEPC_MACHINE(obj) \
    OBJECT_CHECK(NForcePCMachineState, (obj), TYPE_NFORCEPC_MACHINE)

#define NFORCEPC_MACHINE_CLASS(klass) \
    OBJECT_CLASS_CHECK(NForcePCMachineClass, (klass), TYPE_NFORCEPC_MACHINE)

typedef struct NForcePCMachineState {
    /*< private >*/
    PCMachineState parent_obj;

    /*< public >*/
} NForcePCMachineState;

typedef struct NForcePCMachineClass {
    /*< private >*/
    PCMachineClass parent_class;

    /*< public >*/
} NForcePCMachineClass;

static void nforcepc_init_common(MachineState *machine,
                                 PCIBus **pci_bus_out,
                                 ISABus **isa_bus_out)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();

    PCIBus *pci_bus;
    ISABus *isa_bus;

    GSIState *gsi_state;

    MC146818RtcState *rtc_state;
    ISADevice *pit = NULL;
    int pit_isa_irq = 0;
    qemu_irq pit_alt_irq = NULL;

    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    MemoryRegion *rom_memory;

    I2CBus *smbus;
    PCIBus *agp_bus;

    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);
    rom_memory = pci_memory;

    /* allocate ram and load rom/bios - use standard PC BIOS loading */
    pc_memory_init(pcms, system_memory, rom_memory, 0);
    ram_memory = machine->ram;

    gsi_state = pc_gsi_create(&x86ms->gsi, pcmc->pci_enabled);

    xbox_pci_init(x86ms->gsi,
                  get_system_memory(), get_system_io(),
                  pci_memory, ram_memory, rom_memory,
                  &pci_bus,
                  &isa_bus,
                  &smbus,
                  &agp_bus);

    pcms->pcibus = pci_bus;

    isa_bus_register_input_irqs(isa_bus, x86ms->gsi);

    pc_i8259_create(isa_bus, gsi_state->i8259_irq);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    /* init basic PC hardware */
    rtc_state = mc146818_rtc_init(isa_bus, 2000, NULL);
    x86ms->rtc = ISA_DEVICE(rtc_state);

    if (kvm_pit_in_kernel()) {
        pit = kvm_pit_init(isa_bus, 0x40);
    } else {
        pit = i8254_pit_init(isa_bus, 0x40, pit_isa_irq, pit_alt_irq);
    }

    i8257_dma_init(OBJECT(machine), isa_bus, 0);

    object_property_set_link(OBJECT(pcms->pcspk), "pit",
                             OBJECT(pit), &error_fatal);
    isa_realize_and_unref(pcms->pcspk, isa_bus, &error_fatal);

    /* Standard PC IDE controller */
    PCIDevice *dev = pci_create_simple(pci_bus, PCI_DEVFN(9, 0), "piix3-ide");
    pci_ide_create_devs(dev);

    /* Standard USB controllers (fewer than Xbox) */
    PCIDevice *usb0 = pci_new(PCI_DEVFN(2, 0), "pci-ohci");
    qdev_prop_set_uint32(&usb0->qdev, "num-ports", 4);
    pci_realize_and_unref(usb0, pci_bus, &error_fatal);

    /* Standard network controller */
    PCIDevice *nvnet = pci_new(PCI_DEVFN(4, 0), "nvnet");
    qemu_configure_nic_device(DEVICE(nvnet), true, "nvnet");
    pci_realize_and_unref(nvnet, pci_bus, &error_fatal);

    /* Basic SMBus without Xbox-specific devices */
    /* Note: Skip Xbox SMC and video encoders as they're Xbox-specific */

    /* FIXME: Stub the memory controller */
    pci_create_simple(pci_bus, PCI_DEVFN(0, 3), "pci-testdev");

    if (pci_bus_out) {
        *pci_bus_out = pci_bus;
    }
    if (isa_bus_out) {
        *isa_bus_out = isa_bus;
    }
}

/* PC hardware initialisation */
static void nforcepc_init(MachineState *machine)
{
    nforcepc_init_common(machine, NULL, NULL);
}

static void nforcepc_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    m->desc = "nForce PC";
    m->max_cpus = 1;
    m->option_rom_has_mr = true;
    m->rom_file_has_mr = false;
    m->no_floppy = 0;  /* Enable floppy for PC compatibility */
    m->no_cdrom = 0;   /* Enable CD-ROM for PC compatibility */
    m->no_sdcard = 0;  /* Enable SD card for PC compatibility */
    m->default_cpu_type = X86_CPU_TYPE_NAME("pentium3");
    m->default_nic = "nvnet";

    pcmc->pci_enabled = true;
    pcmc->has_acpi_build = false;
    pcmc->smbios_defaults = false;
    pcmc->gigabyte_align = false;
    pcmc->smbios_legacy_mode = true;
    pcmc->has_reserved_memory = false;
    pcmc->default_nic_model = "nvnet";
}

static inline void nforcepc_machine_initfn(Object *obj)
{
    /* No additional properties needed for basic nForce PC */
}

static void nforcepc_machine_class_init(ObjectClass *oc, void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    nforcepc_machine_options(mc);
    mc->init = nforcepc_init;
}

static const TypeInfo pc_machine_type_nforcepc = {
    .name = TYPE_NFORCEPC_MACHINE,
    .parent = TYPE_PC_MACHINE,
    .abstract = false,
    .instance_size = sizeof(NForcePCMachineState),
    .instance_init = nforcepc_machine_initfn,
    .class_size = sizeof(NForcePCMachineClass),
    .class_init = nforcepc_machine_class_init,
    .interfaces = (InterfaceInfo[]) {
         { }
    },
};

static void pc_machine_init_nforcepc(void)
{
    type_register(&pc_machine_type_nforcepc);
}

type_init(pc_machine_init_nforcepc)