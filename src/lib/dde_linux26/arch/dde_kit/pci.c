/*
 * \brief  DDE Linux 2.6 PCI backend
 * \author Björn Döbel
 * \author Christian Helmuth
 * \date   2008-11-11
 */

#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/list.h>

#include "pci.h"  /* drivers/pci/pci.h */

#include "local.h"


DECLARE_INITVAR(dde_linux26_pci);

/**************
 ** PCI data **
 **************/

/* PCI bus */
static struct pci_bus *pci_bus = NULL;

static int dde_linux26_pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val);
static int dde_linux26_pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val);


/* PCI operations for our virtual PCI bus */
static struct pci_ops dde_linux26_pcibus_ops = {
	.read = dde_linux26_pci_read,
	.write = dde_linux26_pci_write,
};


/*********************************
 ** Read/write PCI config space **
 *********************************/

static int dde_linux26_pci_read(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 *val)
{
	switch(size) {
		case 1: dde_kit_pci_readb(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where,  (u8 *)val); break;
		case 2: dde_kit_pci_readw(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, (u16 *)val); break;
		case 4: dde_kit_pci_readl(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where,        val); break;
	}

	return 0;
}


static int dde_linux26_pci_write(struct pci_bus *bus, unsigned int devfn, int where, int size, u32 val)
{
	switch(size) {
		case 1: dde_kit_pci_writeb(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, val); break;
		case 2: dde_kit_pci_writew(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, val); break;
		case 4: dde_kit_pci_writel(bus->number, PCI_SLOT(devfn), PCI_FUNC(devfn), where, val); break;
	}

	return 0;
}


int pcibios_enable_resources(struct pci_dev *dev, int mask)
{
	u16 cmd, old_cmd;
	int idx;
	struct resource *r;

	pci_read_config_word(dev, PCI_COMMAND, &cmd);
	old_cmd = cmd;
	for (idx = 0; idx < PCI_NUM_RESOURCES; idx++) {
		/* Only set up the requested stuff */
		if (!(mask & (1 << idx)))
			continue;

		r = &dev->resource[idx];
		if (!(r->flags & (IORESOURCE_IO | IORESOURCE_MEM)))
			continue;
		if ((idx == PCI_ROM_RESOURCE) &&
				(!(r->flags & IORESOURCE_ROM_ENABLE)))
			continue;
		if (!r->start && r->end) {
			printk(KERN_ERR "PCI: Device %s not available "
				"because of resource collisions\n",
				pci_name(dev));
			return -EINVAL;
		}
		if (r->flags & IORESOURCE_IO)
			cmd |= PCI_COMMAND_IO;
		if (r->flags & IORESOURCE_MEM)
			cmd |= PCI_COMMAND_MEMORY;
	}
	if (cmd != old_cmd) {
		printk("PCI: Enabling device %s (%04x -> %04x)\n",
			pci_name(dev), old_cmd, cmd);
		pci_write_config_word(dev, PCI_COMMAND, cmd);
	}
	return 0;
}


int pcibios_enable_device(struct pci_dev *dev, int mask)
{
	int err;

	if ((err = pcibios_enable_resources(dev, mask)) < 0)
		return err;

	/*
	 * Linux enables IRQs here.
	 */

	return 0;
}


void pcibios_set_master(struct pci_dev *dev)
{
	/*
	 * Linux does some workarounds for broken BIOSes here.
	 */
}


unsigned int pcibios_assign_all_busses(void)
{
	return 0;
}


void pci_fixup_device(enum pci_fixup_pass pass, struct pci_dev *dev)
{
	/*
	 * Linux does some fixups for broken devices here. Most of this must be
	 * done in the PCI driver, but there are some normal device fixups too.
	 */
}


int pci_create_sysfs_dev_files(struct pci_dev *pdev)
{
	return 0;
}


/********************
 ** Initialization **
 ********************/

static int dde_linux26_init_pci(void)
{
	/* initialize DDE kit to get virtual bus hierarchy with all PCI devices */
	dde_kit_pci_init(0, 0);

	/*
	 * TODO check if all devices on all buses are scanned/added here.
	 */

	struct pci_bus *pci_bus = pci_create_bus(0, 0, &dde_linux26_pcibus_ops, 0);
	pci_do_scan_bus(pci_bus);

	INITIALIZE_INITVAR(dde_linux26_pci);

	return 0;
}

subsys_initcall(dde_linux26_init_pci);
