/*
 * \brief  Emulation of Linux kernel interfaces
 * \author Norman Feske
 * \date   2010-07-20
 */

/* Linux includes */
#include <lx_emul.h>
#include <lx_emul_priv.h>

/* DDE kit includes */
extern "C" {
#include <dde_kit/lock.h>
#include <dde_kit/printf.h>
#include <dde_kit/semaphore.h>
#include <dde_kit/memory.h>
#include <dde_kit/pgtab.h>
#include <dde_kit/resources.h>
#include <dde_kit/interrupt.h>
}

/* Genode includes */
#include <util/string.h>
#include <util/misc_math.h>
#include <base/printf.h>
#include <base/env.h>
#include <pci_device/client.h>
#include <dataspace/client.h>
#include <base/allocator_avl.h>

/* DRM includes */
#include <drm/drmP.h>

#if VERBOSE_LX_EMUL
#define TRACE       dde_kit_printf("\033[35m%s\033[0m called\n",                __PRETTY_FUNCTION__)
#define UNSUPPORTED dde_kit_printf("\033[31m%s\033[0m unsupported arguments\n", __PRETTY_FUNCTION__)
#else
#define TRACE
#define UNSUPPORTED
#endif


/************************
 ** Locally used types **
 ************************/

class Page : public page
{
	public:

		Page(void *virt_addr)
		{
			/* init 'struct page' */
			virt = virt_addr;
		}
};


extern "C" int i915_gem_init_object(struct drm_gem_object *obj);


struct address_space : public drm_gem_object
{
	private:

		struct file _file;
		struct path _path;
		struct dentry _dentry;
		struct inode _inode;

		unsigned long _size;
		Genode::Dataspace_capability _ds_cap;
		void         *_virt_base;
		unsigned long _phys_base;

	public:

		address_space(struct drm_device *device, size_t obj_size)
		:
			_size(roundup(obj_size, PAGE_SIZE)),
			_ds_cap(Genode::env()->ram_session()->alloc(_size)),
			_virt_base(Genode::env()->rm_session()->attach(_ds_cap)),
			_phys_base(Genode::Dataspace_client(_ds_cap).phys_addr())
		{
			/* make virt-phys mapping known to the Linux environment */
			dde_kit_pgtab_set_region_with_size(_virt_base, _phys_base, _size);

			memset(static_cast<drm_gem_object *>(this), 0, sizeof(drm_gem_object));
			dev  = device;
			size = obj_size;
			if (i915_gem_init_object(this)) {
				PERR("i915_gem_init_object failed");
				return;
			}

			/* init 'filp', dereferenced by i915_gem.c, function 'i915_gem_object_get_pages' */
			_inode.i_op         = 0;
			_inode.i_mapping    = this;
			_dentry.d_inode     = &_inode;
			_file.f_path.dentry = &_dentry;
			filp                = &_file;

			atomic_inc(&dev->object_count);
		};

		Page *get_page(unsigned long index)
		{
			if (index*PAGE_SIZE >= size) {
				PERR("trying to obtain page outside of address space");
				return 0;
			}
			unsigned long virt_addr = (unsigned long)_virt_base + PAGE_SIZE*index;
			return new (Genode::env()->heap()) Page((void *)virt_addr);
		}
};


typedef address_space Gem_object;


struct Dma_handle : public drm_dma_handle_t
{
	Genode::Dataspace_capability _ds_cap;

	Dma_handle(size_t size, size_t align)
	:
		_ds_cap(Genode::env()->ram_session()->alloc(size))
	{
		size    = size;
		vaddr   = Genode::env()->rm_session()->attach(_ds_cap);
		busaddr = Genode::Dataspace_client(_ds_cap).phys_addr();
	};
};


/**********************
 ** Global variables **
 **********************/

/*
 * Used by agp/backend.c, function 'agp_find_max' to determine the maximum
 * number of pages to be spent for the AGP aperture.
 */
unsigned long totalram_pages = (256*1024*1024)/PAGE_SIZE;


/***********************
 ** Linux bureaucracy **
 ***********************/

/*
 * The function 'pci_register_driver' is called during 'agp_intel_init' for
 * registering the AGP driver interface.  We remember the pointer to the
 * registered driver such that the caller of 'agp_intel_init' can obtain it
 * via 'get_last_registered_pci_driver()'.
 */
static struct pci_driver *_last_registered_pci_driver;


/**
 * Custom interface function
 */
struct pci_driver *get_last_registered_pci_driver() {
	return _last_registered_pci_driver; }


/**
 * Linux function
 */
int pci_register_driver(struct pci_driver *driver)
{
	_last_registered_pci_driver = driver;
	return 0;
}


/**
 * Linux function
 *
 * Called by agp/backend.c in function agp_add_bridge. The AGP driver
 * tries to obtain the reference to the chipset driver. Let it succeed.
 */
int try_module_get(struct module *module) { TRACE; return 1; }


/***********************
 ** Atomic operations **
 ***********************/

/*
 * Actually not atomic, for now
 */

void atomic_set(atomic_t *p, atomic_t v) { *p = v; }

atomic_t atomic_read(atomic_t *p) { return *(volatile int *)p; }

void atomic_inc(atomic_t *v) { (*(volatile int *)v)++; }
void atomic_dec(atomic_t *v) { (*(volatile int *)v)--; }

void atomic_add(int i, atomic_t *v) { (*(volatile int *)v) += i; }
void atomic_sub(int i, atomic_t *v) { (*(volatile int *)v) -= i; }


/********************
 ** Bit operations **
 ********************/

/**
 * Find first zero bit at specified memory region
 *
 * \param  size_bits memory region size in bits
 */
unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size_bits)
{
	TRACE;

	enum { BITS_PER_LONG = sizeof(unsigned long)*8 };
	unsigned long max_long = size_bits /= BITS_PER_LONG;
	for (unsigned long i = 0; i < max_long; i++)
		for (unsigned long j = 0, v = addr[i]; BITS_PER_LONG; j++)
			if ((v & (1 << j)) == 0)
				return i*BITS_PER_LONG + j;

	return size_bits;  /* no bit found */
}


/**
 * Set specified bit in memory region
 */
void __set_bit(int nr, volatile long *addr)
{
	TRACE;
	unsigned long mask = sizeof(*addr)*8 - 1;
	addr[nr / sizeof(*addr)] |= 1 << (nr & mask);
}


/**********************
 ** linux/spinlock.h **
 **********************/

void spin_lock_init(spinlock_t *lock) { dde_kit_lock_init(lock); }
void spin_lock(spinlock_t *lock)
{
	/*
	 * If the spinlock was declared via the 'DEFINE_SPINLOCK' macro, it is not
	 * yet initialized when first locked. So we do the initialization here as
	 * needed.
	 */
	if (!*lock)
		spin_lock_init(lock);
	dde_kit_lock_lock(*lock);
}
void spin_unlock(spinlock_t *lock) { dde_kit_lock_unlock(*lock); }

void spin_lock_irqsave(spinlock_t *lock, unsigned long flags) { spin_lock(lock); }
void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags) { spin_unlock(lock); }


/*******************
 ** linux/mutex.h **
 *******************/

void mutex_init  (struct mutex *m) { dde_kit_lock_init  (&m->lock); }
void mutex_lock  (struct mutex *m) { dde_kit_lock_lock  ( m->lock); }
void mutex_unlock(struct mutex *m) { dde_kit_lock_unlock( m->lock); }


/*************************************
 ** Memory allocation, linux/slab.h **
 *************************************/

void *kmalloc(size_t size, gfp_t flags)
{
	return dde_kit_large_malloc(size);
}


void *kzalloc(size_t size, gfp_t flags)
{
	void *addr = dde_kit_large_malloc(size);
	if (addr)
		Genode::memset(addr, 0, size);
	return addr;
}


void kfree(const void *p)
{
	dde_kit_large_free((void *)p);
}


dma_addr_t page_to_phys(void *p)
{
	struct page *page = (struct page *)p;
	dde_kit_addr_t ret = dde_kit_pgtab_get_physaddr(page->virt);
	if (ret == 0)
		PWRN("phys addr of page %p (virt=%p) is %lx", page, page->virt, ret);
	return ret;
}


unsigned long do_mmap(struct file *file, unsigned long addr,
                      unsigned long len, unsigned long prot,
                      unsigned long flag, unsigned long offset)
{
	TRACE;
	PDBG("\naddr=0x%lx, len=0x%lx, offset=0x%lx", addr, len, offset);
	return 0;
}


void *kmap_atomic(struct page *page, enum km_type idx)
{
	TRACE;
	return page->virt;
}


/*********************
 ** linux/pagemap.h **
 *********************/

/*
 * Called by i915_gem.c, function 'i915_gem_object_get_pages'
 * to aquire pages to be mapped into the GTT.
 */
struct page *read_cache_page_gfp(struct address_space *mapping,
                                 pgoff_t index, gfp_t gfp_mask)
{
	/*
	 * Call 'Gem_object'
	 */
	return mapping->get_page(index);
}


/*
 * The return value of this function is used as argument for
 * 'read_cache_page_gfp'. Since we ignore this argument, it is safe to
 * return 0.
 */
gfp_t mapping_gfp_mask(struct address_space * mapping) { return 0; }



/*********************
 ** linux/vmalloc.h **
 *********************/

void *vmalloc(unsigned long size)
{
	return dde_kit_simple_malloc(size);
}


/*********************
 ** linux/uaccess.h **
 *********************/

size_t copy_to_user(void *dst, void *src, size_t len)
{
	if (dst && src && len)
		memcpy(dst, src, len);
	return 0;
}


size_t copy_from_user(void *dst, void *src, size_t len)
{
	if (dst && src && len)
		memcpy(dst, src, len);
	return 0;
}


bool access_ok(int access, void *addr, size_t size) { return 1; }


/***********************
 ** linux/workqueue.h **
 ***********************/

struct workqueue_struct { };


struct workqueue_struct *create_singlethread_workqueue(const char *name)
{
	TRACE;
	return new (Genode::env()->heap()) workqueue_struct();
}


void INIT_DELAYED_WORK(struct delayed_work *, void (*)(struct work_struct *))
{
	TRACE;
	PWRN("not implemented");
}


/*******************
 ** linux/sched.h **
 *******************/

static struct mm_struct current_mm;
static struct task_struct current_task = { &current_mm };
struct task_struct *current = &current_task;


/*****************
 ** linux/gfp.h **
 *****************/

struct page *alloc_pages(gfp_t gfp_mask, unsigned int order)
{
	TRACE;
	struct page *ret = (struct page *)dde_kit_simple_malloc(sizeof(struct page));

	/*
	 * By using 'dde_kit_large_malloc', we can use DDE-Kit's
	 * virt-to-phys and phys-to-virt translations.
	 */
	ret->virt = dde_kit_large_malloc(PAGE_SIZE << order);
	return ret;
}


/****************
 ** linux/io.h **
 ****************/

void *ioremap(resource_size_t offset, unsigned long size)
{
	TRACE;

	dde_kit_addr_t vaddr = 0;
	int ret = dde_kit_request_mem(offset, size, 0L, &vaddr);
	if (ret) {
		PERR("dde_kit_request_mem failed");
		return 0;
	}

	return (void *)vaddr;
}


void *ioremap_wc(resource_size_t offset, unsigned long size)
{
	return ioremap(offset, size);
}


struct io_mapping
{
	private:

		resource_size_t _base;
		unsigned long   _size;

	public:

		/**
		 * Constructor
		 */
		io_mapping(resource_size_t base, unsigned long size) :
			_base(base), _size(size) { }

		resource_size_t base() const { return _base; }
};


/**
 * I/O mapping used by i915_dma.c to map the GTT aperture
 */
struct io_mapping *io_mapping_create_wc(resource_size_t base, unsigned long size)
{
	static int called;
	TRACE;

	if (called++ != 0) {
		PERR("io_mapping_create_wc unexpectedly called twice");
		return 0;
	}

	io_mapping *mapping = new (Genode::env()->heap()) io_mapping(base, size);
	return mapping;
}


void *io_mapping_map_atomic_wc(struct io_mapping *mapping, unsigned long offset)
{
	return ioremap_wc(mapping->base() + offset, PAGE_SIZE);
}


/*********
 ** PCI **
 *********/

/**
 * Linux function
 *
 * Called by intel-agp.c, function 'find_gmch' with 'from' set to zero,
 * the AGP driver expects to find the bridge device.
 */
struct pci_dev *pci_get_device(unsigned int vendor, unsigned int device,
                               struct pci_dev *from)
{
	if (vendor == bridge_pdev.vendor && device == bridge_pdev.device)
		return &bridge_pdev;

	if (vendor == gpu_pdev.vendor && device == gpu_pdev.device)
		return &gpu_pdev;

	PDBG("vendor=%x, device=%x", vendor, device);
	UNSUPPORTED; return 0;
}


size_t pci_resource_start(struct pci_dev *dev, unsigned bar)
{
	if (bar < DEVICE_COUNT_RESOURCE)
		return dev->resource[bar].start;

	UNSUPPORTED; return 0;
}


size_t pci_resource_len(struct pci_dev *dev, unsigned bar)
{
	if (bar < DEVICE_COUNT_RESOURCE)
		return dev->resource[bar].end - dev->resource[bar].start + 1;

	UNSUPPORTED; return 0;
}

/**
 * Linux function
 *
 * Called by i915_dma.c, function 'i915_get_bridge_dev' with both
 * arguments set to zero. The function gets called to obtain the
 * 'pci_dev' structure for the bridge.
 */
struct pci_dev *pci_get_bus_and_slot(unsigned int bus, unsigned int devfn)
{
	TRACE;

	if (bus == 0 && devfn == 0)
		return &bridge_pdev;

	UNSUPPORTED; return 0;
}


/**
 * Utility: lookup PCI device capability by dev/fn ID
 */
static inline
Pci::Device_capability pci_cap(unsigned int devfn)
{
	if (devfn == bridge_pdev.devfn) return bridge_pdev.cap;
	if (devfn == gpu_pdev.devfn)    return gpu_pdev.cap;

	UNSUPPORTED; return Pci::Device_capability();
};


enum { PCIBIOS_SUCCESSFUL = 0 };

int pci_bus_read_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 *val)
{
	*val = Pci::Device_client(pci_cap(devfn)).config_read(where, Pci::Device::ACCESS_8BIT);
	return PCIBIOS_SUCCESSFUL;
}


int pci_bus_read_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 *val)
{
	*val = Pci::Device_client(pci_cap(devfn)).config_read(where, Pci::Device::ACCESS_16BIT);
	return PCIBIOS_SUCCESSFUL;
}


int pci_bus_read_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 *val)
{
	*val = Pci::Device_client(pci_cap(devfn)).config_read(where, Pci::Device::ACCESS_32BIT);
	return PCIBIOS_SUCCESSFUL;
}


int pci_bus_write_config_byte(struct pci_bus *bus, unsigned int devfn, int where, u8 val)
{
	Pci::Device_client(pci_cap(devfn)).config_write(where, val, Pci::Device::ACCESS_8BIT);
	return PCIBIOS_SUCCESSFUL;
}


int pci_bus_write_config_word(struct pci_bus *bus, unsigned int devfn, int where, u16 val)
{
	Pci::Device_client(pci_cap(devfn)).config_write(where, val, Pci::Device::ACCESS_16BIT);
	return PCIBIOS_SUCCESSFUL;
}


int pci_bus_write_config_dword(struct pci_bus *bus, unsigned int devfn, int where, u32 val)
{
	Pci::Device_client(pci_cap(devfn)).config_write(where, val, Pci::Device::ACCESS_32BIT);
	return PCIBIOS_SUCCESSFUL;
}


/**************************
 ** DRM-specific support **
 **************************/

/**
 * Allocate DMA-capable memory
 *
 * This function is called by i915_dma.c, function 'i915_init_phys_hws'.
 * The returned data structure contains memory to be used for DMA. Hence
 * it has a virt and phys member.
 */
drm_dma_handle_t *drm_pci_alloc(struct drm_device *dev, size_t size, size_t align)
{
	TRACE;
	return new (Genode::env()->heap()) Dma_handle(size, align);
}


class Mm_node : public drm_mm_node
{
	private:

		unsigned long _size;
		bool _used;

	public:

		Mm_node(unsigned long in_start, unsigned long size)
		: _size(size), _used(false)
		{
			memset(static_cast<drm_mm_node *>(this), 0, sizeof(drm_mm_node));
			start = in_start;
		}

		Mm_node *get_block(unsigned long size, unsigned alignment)
		{
			if (_used) {
				PERR("node is already in use");
				return 0;
			}

			Mm_node *child = new (Genode::env()->heap()) Mm_node(start, _size);
			_used = true;
			return child;
		}
};


class drm_mm_priv
{
	private:

		Genode::Allocator_avl _alloc;

	public:

		drm_mm_priv(unsigned long start, unsigned long size) :
			_alloc(Genode::env()->heap())
		{
			_alloc.add_range(start, size);
		}

		Mm_node *search_free(unsigned long size, unsigned alignment)
		{
			/* convert alignment parameter to log2 */
			unsigned align_log2 = alignment ? Genode::log2(alignment) : 0;

			void *addr = 0;
			if (_alloc.alloc_aligned(size, &addr, align_log2).is_error()) {
				PERR("allocation failed");
				return 0;
			}

			return new (Genode::env()->heap()) Mm_node((long)addr, size);
		}
};


int drm_mm_init(struct drm_mm *mm, unsigned long start, unsigned long size)
{
	TRACE;
	mm->priv = new (Genode::env()->heap()) drm_mm_priv(start, size);
	return 0;
}


struct drm_mm_node *drm_mm_search_free(const struct drm_mm *mm,
                                       unsigned long size,
                                       unsigned alignment,
                                       int best_match)
{
	return mm->priv->search_free(size, alignment);
}


struct drm_mm_node *drm_mm_get_block(struct drm_mm_node *parent,
                                     unsigned long size,
                                     unsigned alignment)
{
	Mm_node *parent_node = static_cast<Mm_node *>(parent);
	return parent_node->get_block(size, alignment);
}


struct drm_gem_object *drm_gem_object_alloc(struct drm_device *dev, size_t size)
{
	TRACE;
	return new (Genode::env()->heap()) Gem_object(dev, size);
}


void *drm_calloc_large(size_t nmemb, size_t size)
{
	return dde_kit_large_malloc(nmemb*size);
}


extern "C" irqreturn_t i915_driver_irq_handler(DRM_IRQ_ARGS);
extern "C" void i915_driver_irq_preinstall(struct drm_device * dev);
extern "C" int i915_driver_irq_postinstall(struct drm_device *dev);


/* normally implemented in drm_irq.c */
int drm_irq_install(struct drm_device *dev)
{
	TRACE;
	return 0; /* XXX not used yet - to be implemented when starting to use vblank */

	mutex_lock(&dev->struct_mutex);
	dev->irq_enabled = 1;
	mutex_unlock(&dev->struct_mutex);

	i915_driver_irq_preinstall(dev);

	enum { IRQ_SHARED = 1 };
	void (*handler)(void *) = (void (*)(void *))i915_driver_irq_handler;
	int ret = dde_kit_interrupt_attach(dev->pdev->irq, IRQ_SHARED, 0,
	                                   handler, dev);
	if (ret) {
		PERR("could not handler attach to irq %d", dev->pdev->irq);
		return -1;
	}

	dde_kit_interrupt_enable(dev->pdev->irq);
	i915_driver_irq_postinstall(dev);
	return 0;
}


void  drm_free_large(void *ptr)
{
	kfree(ptr);
}


void *drm_malloc_ab(size_t nmemb, size_t size)
{
	return kmalloc(nmemb*size, GFP_KERNEL);
}


void drm_clflush_pages(struct page *pages[], unsigned long num_pages)
{
	asm volatile ("": : :"memory");
	for (unsigned i = 0; i < num_pages; i++) {
		struct page *page = pages[i];
		if (!page) continue;

		volatile char *virt_addr = (volatile char *)page->virt;

		enum { CACHE_LINE_SIZE = 16 /* hard-coded for now */ };
		for (int i = 0; i < PAGE_SIZE; i += CACHE_LINE_SIZE)
			asm volatile("clflush %0" : "+m" (*(virt_addr + i)));
	}
	asm volatile ("": : :"memory");
}
