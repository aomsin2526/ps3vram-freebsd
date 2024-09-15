#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <sys/disk.h>
#include <sys/bio.h>
#include <geom/geom_disk.h>

#include <powerpc/ps3/ps3-hvcall.h>

#define PS3VRAM_LOCK_INIT(_lock)		\
	mtx_init(_lock, "ps3vram", "ps3vram", MTX_DEF)
#define PS3VRAM_LOCK_DESTROY(_lock)	mtx_destroy(_lock);
#define PS3VRAM_LOCK(_lock)		mtx_lock(_lock)
#define	PS3VRAM_UNLOCK(_lock)		mtx_unlock(_lock)

static void map_lpar_to_ea(uint64_t lpar_addr, uint64_t ea_addr, uint64_t size)
{
    //if (size % PAGE_SIZE)
        //panic("bad size!\n");

    for (uint64_t i = 0; i < size; i += PAGE_SIZE)
    {
        pmap_kenter_attr(ea_addr + i, lpar_addr + i,
		    VM_MEMATTR_UNCACHEABLE);
    }
}

static void unmap_lpar_from_ea(uint64_t ea_addr, uint64_t size)
{
    //if (size % PAGE_SIZE)
        //panic("bad size!\n");

    for (uint64_t i = 0; i < size; i += PAGE_SIZE)
    {
        pmap_kremove(ea_addr + i);
    }
}

struct ps3vram_context_s
{
    uint64_t ddr_size;

    uint64_t memory_handle;
    uint64_t ddr_lpar;

    uint8_t* virt_addr;

    uint64_t disk_blocksize;

    struct disk* dsk;

    struct mtx lock;
};

struct ps3vram_context_s context;

static int ps3vram_disk_open(struct disk *dp)
{
    return 0;
}

static int ps3vram_disk_close(struct disk *dp)
{
    return 0;
}

static void ps3vram_disk_strategy(struct bio *bp)
{
    uint64_t offset = 0;

    bool bad = bp->bio_cmd != BIO_READ && bp->bio_cmd != BIO_WRITE;

    if (bad)
    {
        panic("bad!\n");

        bp->bio_error = EINVAL;
        bp->bio_flags |= BIO_ERROR;

        biodone(bp);
        return;
    }

    offset = bp->bio_offset;

    //printf("kuy bio_cmd = %d, offset = %d, length = %d, pblkno = %d, bp->bio_offset = %d\n", (int)bp->bio_cmd, (int)offset, (int)bp->bio_length, (int)bp->bio_pblkno, (int)bp->bio_offset);

    PS3VRAM_LOCK(&context.lock);

    map_lpar_to_ea(context.ddr_lpar + offset, (uint64_t)context.virt_addr, bp->bio_length);

    if (bp->bio_cmd == BIO_READ)
        memcpy(bp->bio_data, context.virt_addr, bp->bio_length);
    else if (bp->bio_cmd == BIO_WRITE)
        memcpy(context.virt_addr, bp->bio_data, bp->bio_length);
    else
        panic("wtf?\n");

    unmap_lpar_from_ea((uint64_t)context.virt_addr, bp->bio_length);

    PS3VRAM_UNLOCK(&context.lock);

    bp->bio_error = 0;
    bp->bio_resid = 0;
    bp->bio_flags |= BIO_DONE;

    biodone(bp);
}

static int32_t ps3vram_init(void)
{
    printf("ps3vram_init()\n");

    PS3VRAM_LOCK_INIT(&context.lock);

    context.ddr_size = 256 * 1024 * 1024;
    
    while (1)
    {
        if (context.ddr_size == 0)
            panic("ps3vram: allocate gpu memory failed!\n");

        int32_t err = lv1_gpu_memory_allocate(context.ddr_size, 0, 0, 0, 0, &context.memory_handle, &context.ddr_lpar);

        if (err == 0)
            break;

        context.ddr_size -= 1 * 1024 * 1024;
    }

    printf("ps3vram: ddr_size = %lu MiB (%lu Bytes), memory_handle = 0x%lx, ddr_lpar = 0x%lx\n",
        context.ddr_size / 1024 / 1024, context.ddr_size, context.memory_handle, context.ddr_lpar);

    context.virt_addr = (uint8_t*)0x100000000;

    printf("ps3vram: virt_addr = %p\n", context.virt_addr);

    {
        context.disk_blocksize = PAGE_SIZE;
        printf("ps3vram: block size = %lu\n", (uint64_t)context.disk_blocksize);

        context.dsk = disk_alloc();

        context.dsk->d_open = ps3vram_disk_open;
	context.dsk->d_close = ps3vram_disk_close;
	context.dsk->d_strategy = ps3vram_disk_strategy;
	context.dsk->d_name = "ps3vram";
	context.dsk->d_maxsize = 1 * 1024 * 1024;
	context.dsk->d_sectorsize = context.disk_blocksize;
	context.dsk->d_unit = 0;
	context.dsk->d_mediasize = context.ddr_size;
	context.dsk->d_flags = 0;

        disk_create(context.dsk, DISK_VERSION);
    }

    printf("ps3vram: ready\n");
    return 0;
}

static int32_t ps3vram_destroy(void)
{
    printf("ps3vram_destroy()\n");

    disk_destroy(context.dsk);

    int32_t err = lv1_gpu_memory_free(context.memory_handle);

    if (err != 0)
        panic("ps3vram: free gpu memory failed! (err = %d)\n", err);

    PS3VRAM_LOCK_DESTROY(&context.lock);

    printf("ps3vram: destroyed\n");
    return 0;
}

static int32_t ps3vram_module_event_handler(struct module *module, int event_type, void *arg)
{
    if (event_type == MOD_LOAD)
        return ps3vram_init();
    else if (event_type == MOD_UNLOAD)
        return ps3vram_destroy();

    return EOPNOTSUPP;
}

static moduledata_t ps3vram_module_data = {
    "ps3vram",
    ps3vram_module_event_handler,
    NULL
};

DECLARE_MODULE(ps3vram, ps3vram_module_data, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);