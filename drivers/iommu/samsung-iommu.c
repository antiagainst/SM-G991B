// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Samsung Electronics Co., Ltd.
 */

#define pr_fmt(fmt) "sysmmu: " fmt

#include <linux/dma-iommu.h>
#include <linux/kmemleak.h>
#include <linux/module.h>
#include <linux/of_iommu.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>

#include "samsung-iommu.h"

#define FLPD_SHAREABLE_FLAG	BIT(6)
#define SLPD_SHAREABLE_FLAG	BIT(4)
#define FLPD_PBHA_SHIFT		4
#define SLPD_PBHA_SHIFT		2

#define REG_MMU_INV_ALL			0x010
#define REG_MMU_INV_RANGE		0x018
#define REG_MMU_INV_START		0x020
#define REG_MMU_INV_END			0x024

#define MMU_TLB_CFG_MASK(reg)		((reg) & (GENMASK(7, 5) | GENMASK(3, 2) | GENMASK(1, 1)))
#define MMU_TLB_MATCH_CFG_MASK(reg)	((reg) & (GENMASK(31, 16) | GENMASK(9, 8)))

#define REG_MMU_TLB_CFG(n)		(0x2000 + ((n) * 0x20) + 0x4)
#define REG_MMU_TLB_MATCH_CFG(n)	(0x2000 + ((n) * 0x20) + 0x8)
#define REG_MMU_TLB_MATCH_ID(n)		(0x2000 + ((n) * 0x20) + 0x14)

#define DEFAULT_QOS_VALUE	-1
#define DEFAULT_TLB_NONE	~0U
#define UNUSED_TLB_INDEX	~0U

static const unsigned int sysmmu_reg_set[MAX_SET_IDX][MAX_REG_IDX] = {
	/* Default without VM */
	{
		/* FLPT base, TLB invalidation, Fault information */
		0x000C,	0x0010,	0x0014,	0x0018,
		0x0020,	0x0024,	0x0070,	0x0078,
		/* TLB information */
		0x8000,	0x8004,	0x8008,	0x800C,
		/* SBB information */
		0x8020,	0x8024,	0x8028,	0x802C,
		/* secure FLPT base (same as non-secure) */
		0x000C,
	},
	/* VM */
	{
		/* FLPT base, TLB invalidation, Fault information */
		0x800C,	0x8010,	0x8014,	0x8018,
		0x8020,	0x8024,	0x1000,	0x1004,
		/* TLB information */
		0x3000,	0x3004,	0x3008,	0x300C,
		/* SBB information */
		0x3020,	0x3024,	0x3028,	0x302C,
		/* secure FLPT base */
		0x000C,
	},
};

static struct iommu_ops samsung_sysmmu_ops;
static struct platform_driver samsung_sysmmu_driver;

struct samsung_sysmmu_domain {
	struct iommu_domain domain;
	struct iommu_group *group;
	sysmmu_pte_t *page_table;
	atomic_t *lv2entcnt;
	spinlock_t pgtablelock; /* serialize races to page table updates */
};

static bool sysmmu_global_init_done;
DEFINE_MUTEX(sysmmu_global_mutex);
static struct device sync_dev;
static struct kmem_cache *flpt_cache, *slpt_cache;

static inline u32 __sysmmu_get_tlb_num(struct sysmmu_drvdata *data)
{
	return MMU_CAPA1_NUM_TLB(readl_relaxed(data->sfrbase +
					       REG_MMU_CAPA1_V7));
}

static inline u32 __sysmmu_get_hw_version(struct sysmmu_drvdata *data)
{
	return MMU_RAW_VER(readl_relaxed(data->sfrbase + REG_MMU_VERSION));
}

static inline bool __sysmmu_has_capa1(struct sysmmu_drvdata *data)
{
	return MMU_CAPA1_EXIST(readl_relaxed(data->sfrbase + REG_MMU_CAPA0_V7));
}

static inline u32 __sysmmu_get_capa_type(struct sysmmu_drvdata *data)
{
	return MMU_CAPA1_TYPE(readl_relaxed(data->sfrbase + REG_MMU_CAPA1_V7));
}

static inline bool __sysmmu_get_capa_no_block_mode(struct sysmmu_drvdata *data)
{
	return MMU_CAPA1_NO_BLOCK_MODE(readl_relaxed(data->sfrbase +
						     REG_MMU_CAPA1_V7));
}

static inline bool __sysmmu_get_capa_vcr_enabled(struct sysmmu_drvdata *data)
{
	return MMU_CAPA1_VCR_ENABLED(readl_relaxed(data->sfrbase +
						   REG_MMU_CAPA1_V7));
}

static inline void __sysmmu_tlb_invalidate_all(struct sysmmu_drvdata *data)
{
	writel(0x1, MMU_REG(data, IDX_ALL_INV));
}

static inline void __sysmmu_tlb_invalidate(struct sysmmu_drvdata *data,
					   dma_addr_t start, dma_addr_t end)
{
	writel_relaxed(ALIGN_DOWN(start, SPAGE_SIZE), MMU_REG(data, IDX_RANGE_INV_START));
	writel_relaxed(ALIGN_DOWN(end - 1, SPAGE_SIZE), MMU_REG(data, IDX_RANGE_INV_END));
	writel(0x1, MMU_REG(data, IDX_RANGE_INV));
}

static inline void __sysmmu_disable(struct sysmmu_drvdata *data)
{
	if (data->no_block_mode) {
		__sysmmu_tlb_invalidate_all(data);
	} else {
		u32 ctrl_val = readl_relaxed(data->sfrbase + REG_MMU_CTRL);

		ctrl_val &= ~CTRL_MMU_ENABLE;
		writel(ctrl_val | CTRL_MMU_BLOCK, data->sfrbase + REG_MMU_CTRL);
	}
}

static inline void __sysmmu_set_tlb(struct sysmmu_drvdata *data)
{
	struct tlb_props *tlb_props = &data->tlb_props;
	struct tlb_config *cfg = tlb_props->cfg;
	int id_cnt = tlb_props->id_cnt;
	unsigned int i, index;

	if (tlb_props->default_cfg != DEFAULT_TLB_NONE)
		writel_relaxed(MMU_TLB_CFG_MASK(tlb_props->default_cfg),
			       data->sfrbase + REG_MMU_TLB_CFG(0));

	for (i = 0; i < id_cnt; i++) {
		if (cfg[i].index == UNUSED_TLB_INDEX)
			continue;

		index = cfg[i].index;
		writel_relaxed(MMU_TLB_CFG_MASK(cfg[i].cfg),
			       data->sfrbase + REG_MMU_TLB_CFG(index));
		writel_relaxed(MMU_TLB_MATCH_CFG_MASK(cfg[i].match_cfg),
			       data->sfrbase + REG_MMU_TLB_MATCH_CFG(index));
		writel_relaxed(cfg[i].match_id,
			       data->sfrbase + REG_MMU_TLB_MATCH_ID(index));
	}
}

static inline void __sysmmu_init_config(struct sysmmu_drvdata *data)
{
	u32 cfg = readl_relaxed(data->sfrbase + REG_MMU_CFG);

	if (data->qos != DEFAULT_QOS_VALUE) {
		cfg &= ~CFG_QOS(0xF);
		cfg |= CFG_QOS_OVRRIDE | CFG_QOS(data->qos);
	}

	__sysmmu_set_tlb(data);

	writel_relaxed(cfg, data->sfrbase + REG_MMU_CFG);
}

static inline void __sysmmu_enable(struct sysmmu_drvdata *data)
{
	u32 ctrl_val = readl_relaxed(data->sfrbase + REG_MMU_CTRL);

	if (!data->no_block_mode)
		writel_relaxed(ctrl_val | CTRL_MMU_BLOCK, data->sfrbase + REG_MMU_CTRL);

	__sysmmu_init_config(data);

	writel_relaxed(data->pgtable / SPAGE_SIZE,
		       MMU_REG(data, IDX_FLPT_BASE));
	__sysmmu_tlb_invalidate_all(data);

	writel(ctrl_val | CTRL_MMU_ENABLE, data->sfrbase + REG_MMU_CTRL);

	if (data->has_vcr) {
		ctrl_val = readl_relaxed(data->sfrbase + REG_MMU_CTRL_VM);

		if (!data->async_fault_mode)
			ctrl_val |= CTRL_FAULT_STALL_MODE;
		writel(ctrl_val | CTRL_MMU_ENABLE, data->sfrbase + REG_MMU_CTRL_VM);
	}
}

static struct samsung_sysmmu_domain *to_sysmmu_domain(struct iommu_domain *dom)
{
	return container_of(dom, struct samsung_sysmmu_domain, domain);
}

static inline void pgtable_flush(void *vastart, void *vaend)
{
	dma_sync_single_for_device(&sync_dev, virt_to_phys(vastart),
				   vaend - vastart, DMA_TO_DEVICE);
}

static bool samsung_sysmmu_capable(enum iommu_cap cap)
{
	return cap == IOMMU_CAP_CACHE_COHERENCY;
}

static struct iommu_domain *samsung_sysmmu_domain_alloc(unsigned int type)
{
	struct samsung_sysmmu_domain *domain;

	if (type != IOMMU_DOMAIN_UNMANAGED &&
	    type != IOMMU_DOMAIN_DMA &&
	    type != IOMMU_DOMAIN_IDENTITY) {
		pr_err("invalid domain type %u\n", type);
		return NULL;
	}

	domain = kzalloc(sizeof(*domain), GFP_KERNEL);
	if (!domain)
		return NULL;

	domain->page_table =
		(sysmmu_pte_t *)kmem_cache_alloc(flpt_cache,
						 GFP_KERNEL | __GFP_ZERO);
	if (!domain->page_table)
		goto err_pgtable;

	domain->lv2entcnt = kcalloc(NUM_LV1ENTRIES, sizeof(*domain->lv2entcnt),
				    GFP_KERNEL);
	if (!domain->lv2entcnt)
		goto err_counter;

	if (type == IOMMU_DOMAIN_DMA) {
		int ret = iommu_get_dma_cookie(&domain->domain);

		if (ret) {
			pr_err("failed to get dma cookie (%d)\n", ret);
			goto err_get_dma_cookie;
		}
	}

	pgtable_flush(domain->page_table, domain->page_table + NUM_LV1ENTRIES);

	spin_lock_init(&domain->pgtablelock);

	return &domain->domain;

err_get_dma_cookie:
	kfree(domain->lv2entcnt);
err_counter:
	kmem_cache_free(flpt_cache, domain->page_table);
err_pgtable:
	kfree(domain);
	return NULL;
}

static void samsung_sysmmu_domain_free(struct iommu_domain *dom)
{
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);

	iommu_put_dma_cookie(dom);
	kmem_cache_free(flpt_cache, domain->page_table);
	kfree(domain->lv2entcnt);
	kfree(domain);
}

static inline void samsung_sysmmu_detach_drvdata(struct sysmmu_drvdata *data)
{
	unsigned long flags;

	spin_lock_irqsave(&data->lock, flags);
	if (--data->attached_count == 0) {
		if (pm_runtime_active(data->dev))
			__sysmmu_disable(data);

		list_del(&data->list);
		data->pgtable = 0;
		data->group = NULL;
	}
	spin_unlock_irqrestore(&data->lock, flags);
}

static int samsung_sysmmu_set_domain_range(struct iommu_domain *dom,
					   struct device *dev)
{
	struct iommu_domain_geometry *geom = &dom->geometry;
	dma_addr_t start, end;
	size_t size;

	if (of_get_dma_window(dev->of_node, NULL, 0, NULL, &start, &size))
		return 0;

	end = start + size;

	if (end > DMA_BIT_MASK(32))
		end = DMA_BIT_MASK(32);

	if (geom->force_aperture) {
		dma_addr_t d_start, d_end;

		d_start = max(start, geom->aperture_start);
		d_end = min(end, geom->aperture_end);

		if (d_start >= d_end) {
			dev_err(dev, "current range is [%pad..%pad]\n",
				&geom->aperture_start, &geom->aperture_end);
			dev_err(dev, "requested range [%zx @ %pad] is not allowed\n",
				size, &start);
			return -ERANGE;
		}

		geom->aperture_start = d_start;
		geom->aperture_end = d_end;
	} else {
		geom->aperture_start = start;
		geom->aperture_end = end;
		/*
		 * All CPUs should observe the change of force_aperture after
		 * updating aperture_start and aperture_end because dma-iommu
		 * restricts dma virtual memory by this aperture when
		 * force_aperture is set.
		 * We allow allocating dma virtual memory during changing the
		 * aperture range because the current allocation is free from
		 * the new restricted range.
		 */
		smp_wmb();
		geom->force_aperture = true;
	}

	dev_info(dev, "changed DMA range [%pad..%pad] successfully.\n",
		 &geom->aperture_start, &geom->aperture_end);

	return 0;
}

static int samsung_sysmmu_attach_dev(struct iommu_domain *dom,
				     struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct sysmmu_clientdata *client;
	struct samsung_sysmmu_domain *domain;
	struct list_head *group_list;
	struct sysmmu_drvdata *drvdata;
	struct iommu_group *group = dev->iommu_group;
	unsigned long flags;
	phys_addr_t page_table;
	int i, ret = -EINVAL;

	if (!fwspec || fwspec->ops != &samsung_sysmmu_ops) {
		dev_err(dev, "failed to attach, IOMMU instance data %s.\n",
			!fwspec ? "is not initialized" : "has different ops");
		return -ENXIO;
	}

	if (!fwspec->iommu_priv) {
		dev_err(dev, "has no IOMMU\n");
		return -ENODEV;
	}

	domain = to_sysmmu_domain(dom);
	domain->group = group;
	group_list = iommu_group_get_iommudata(group);
	page_table = virt_to_phys(domain->page_table);

	client = fwspec->iommu_priv;
	for (i = 0; i < client->sysmmu_count; i++) {
		drvdata = client->sysmmus[i];

		spin_lock_irqsave(&drvdata->lock, flags);
		if (drvdata->attached_count++ == 0) {
			list_add(&drvdata->list, group_list);
			drvdata->group = group;
			drvdata->pgtable = page_table;

			if (pm_runtime_active(drvdata->dev))
				__sysmmu_enable(drvdata);
		} else if (drvdata->pgtable != page_table) {
			dev_err(dev, "%s is already attached to other domain\n",
				dev_name(drvdata->dev));
			spin_unlock_irqrestore(&drvdata->lock, flags);
			goto err_drvdata_add;
		}
		spin_unlock_irqrestore(&drvdata->lock, flags);
	}

	ret = samsung_sysmmu_set_domain_range(dom, dev);
	if (ret)
		goto err_drvdata_add;

	dev_info(dev, "attached with pgtable %pa\n", &domain->page_table);

	return 0;

err_drvdata_add:
	while (i-- > 0) {
		drvdata = client->sysmmus[i];

		samsung_sysmmu_detach_drvdata(drvdata);
	}

	return ret;
}

static void samsung_sysmmu_detach_dev(struct iommu_domain *dom,
				      struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct sysmmu_clientdata *client;
	struct samsung_sysmmu_domain *domain;
	struct list_head *group_list;
	struct sysmmu_drvdata *drvdata;
	struct iommu_group *group = dev->iommu_group;
	int i;

	domain = to_sysmmu_domain(dom);
	group_list = iommu_group_get_iommudata(group);

	client = fwspec->iommu_priv;
	for (i = 0; i < client->sysmmu_count; i++) {
		drvdata = client->sysmmus[i];

		samsung_sysmmu_detach_drvdata(drvdata);
	}

	dev_info(dev, "detached from pgtable %pa\n", &domain->page_table);
}

static inline sysmmu_pte_t make_sysmmu_pte(phys_addr_t paddr,
					   int pgsize, int attr)
{
	return ((sysmmu_pte_t)((paddr) >> PG_ENT_SHIFT)) | pgsize | attr;
}

static sysmmu_pte_t *alloc_lv2entry(struct samsung_sysmmu_domain *domain,
				    sysmmu_pte_t *sent, sysmmu_iova_t iova,
				    atomic_t *pgcounter)
{
	if (lv1ent_section(sent)) {
		WARN(1, "trying mapping on %#08x mapped with 1MiB page", iova);
		return ERR_PTR(-EADDRINUSE);
	}

	if (lv1ent_unmapped(sent)) {
		unsigned long flags;
		sysmmu_pte_t *pent;

		pent = kmem_cache_zalloc(slpt_cache, GFP_KERNEL);
		if (!pent)
			return ERR_PTR(-ENOMEM);

		spin_lock_irqsave(&domain->pgtablelock, flags);
		if (lv1ent_unmapped(sent)) {
			*sent = make_sysmmu_pte(virt_to_phys(pent),
						SLPD_FLAG, 0);
			kmemleak_ignore(pent);
			atomic_set(pgcounter, 0);
			pgtable_flush(pent, pent + NUM_LV2ENTRIES);
			pgtable_flush(sent, sent + 1);
		} else {
			/* allocated entry is not used, so free it. */
			kmem_cache_free(slpt_cache, pent);
		}
		spin_unlock_irqrestore(&domain->pgtablelock, flags);
	}

	return page_entry(sent, iova);
}

static inline void clear_lv2_page_table(sysmmu_pte_t *ent, int n)
{
	memset(ent, 0, sizeof(*ent) * n);
}

#define IOMMU_PRIV_PROT_TO_PBHA(val)	(((val) >> IOMMU_PRIV_SHIFT) & 0x3)
static int lv1set_section(struct samsung_sysmmu_domain *domain,
			  sysmmu_pte_t *sent, sysmmu_iova_t iova,
			  phys_addr_t paddr, int prot, atomic_t *pgcnt)
{
	int attr = !!(prot & IOMMU_CACHE) ? FLPD_SHAREABLE_FLAG : 0;
	bool need_sync = false;
	int pbha = IOMMU_PRIV_PROT_TO_PBHA(prot);

	if (lv1ent_section(sent)) {
		WARN(1, "Trying mapping 1MB@%#08x on valid FLPD", iova);
		return -EADDRINUSE;
	}

	if (lv1ent_page(sent)) {
		if (WARN_ON(atomic_read(pgcnt) != 0)) {
			WARN(1, "Trying mapping 1MB@%#08x on valid SLPD", iova);
			return -EADDRINUSE;
		}

		kmem_cache_free(slpt_cache, page_entry(sent, 0));
		atomic_set(pgcnt, NUM_LV2ENTRIES);
		need_sync = true;
	}

	attr |= pbha << FLPD_PBHA_SHIFT;
	*sent = make_sysmmu_pte(paddr, SECT_FLAG, attr);
	pgtable_flush(sent, sent + 1);

	if (need_sync) {
		struct iommu_iotlb_gather gather = {
			.start = iova,
			.end = iova + SECT_SIZE,
		};

		iommu_tlb_sync(&domain->domain, &gather);
	}

	return 0;
}

static int lv2set_page(sysmmu_pte_t *pent, phys_addr_t paddr,
		       size_t size, int prot, atomic_t *pgcnt)
{
	int attr = !!(prot & IOMMU_CACHE) ? SLPD_SHAREABLE_FLAG : 0;
	int pbha = IOMMU_PRIV_PROT_TO_PBHA(prot);

	attr |= pbha << SLPD_PBHA_SHIFT;

	if (size == SPAGE_SIZE) {
		if (WARN_ON(!lv2ent_unmapped(pent)))
			return -EADDRINUSE;

		*pent = make_sysmmu_pte(paddr, SPAGE_FLAG, attr);
		pgtable_flush(pent, pent + 1);
		atomic_inc(pgcnt);
	} else {	/* size == LPAGE_SIZE */
		unsigned long i;

		for (i = 0; i < SPAGES_PER_LPAGE; i++, pent++) {
			if (WARN_ON(!lv2ent_unmapped(pent))) {
				clear_lv2_page_table(pent - i, i);
				return -EADDRINUSE;
			}

			*pent = make_sysmmu_pte(paddr, LPAGE_FLAG, attr);
		}
		pgtable_flush(pent - SPAGES_PER_LPAGE, pent);
		atomic_add(SPAGES_PER_LPAGE, pgcnt);
	}

	return 0;
}

static int samsung_sysmmu_map(struct iommu_domain *dom, unsigned long l_iova,
			      phys_addr_t paddr, size_t size, int prot)
{
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);
	sysmmu_iova_t iova = (sysmmu_iova_t)l_iova;
	atomic_t *lv2entcnt = &domain->lv2entcnt[lv1ent_offset(iova)];
	sysmmu_pte_t *entry;
	int ret = -ENOMEM;

	/* Do not use IO coherency if iOMMU_PRIV exists */
	if (!!(prot & IOMMU_PRIV))
		prot &= ~IOMMU_CACHE;

	entry = section_entry(domain->page_table, iova);

	if (size == SECT_SIZE) {
		ret = lv1set_section(domain, entry, iova, paddr, prot,
				     lv2entcnt);
	} else {
		sysmmu_pte_t *pent;

		pent = alloc_lv2entry(domain, entry, iova, lv2entcnt);

		if (IS_ERR(pent))
			ret = PTR_ERR(pent);
		else
			ret = lv2set_page(pent, paddr, size, prot, lv2entcnt);
	}

	if (ret)
		pr_err("failed to map %#zx @ %#x, ret:%d\n", size, iova, ret);

	return ret;
}

static size_t samsung_sysmmu_unmap(struct iommu_domain *dom,
				   unsigned long l_iova, size_t size,
				   struct iommu_iotlb_gather *gather)
{
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);
	sysmmu_iova_t iova = (sysmmu_iova_t)l_iova;
	atomic_t *lv2entcnt = &domain->lv2entcnt[lv1ent_offset(iova)];
	sysmmu_pte_t *sent, *pent;
	size_t err_pgsize;

	sent = section_entry(domain->page_table, iova);

	if (lv1ent_section(sent)) {
		if (WARN_ON(size < SECT_SIZE)) {
			err_pgsize = SECT_SIZE;
			goto err;
		}

		*sent = 0;
		pgtable_flush(sent, sent + 1);
		size = SECT_SIZE;
		goto done;
	}

	if (unlikely(lv1ent_unmapped(sent))) {
		if (size > SECT_SIZE)
			size = SECT_SIZE;
		goto done;
	}

	/* lv1ent_page(sent) == true here */

	pent = page_entry(sent, iova);

	if (unlikely(lv2ent_unmapped(pent))) {
		size = SPAGE_SIZE;
		goto done;
	}

	if (lv2ent_small(pent)) {
		*pent = 0;
		size = SPAGE_SIZE;
		pgtable_flush(pent, pent + 1);
		atomic_dec(lv2entcnt);
		goto done;
	}

	/* lv1ent_large(pent) == true here */
	if (WARN_ON(size < LPAGE_SIZE)) {
		err_pgsize = LPAGE_SIZE;
		goto err;
	}

	clear_lv2_page_table(pent, SPAGES_PER_LPAGE);
	pgtable_flush(pent, pent + SPAGES_PER_LPAGE);
	size = LPAGE_SIZE;
	atomic_sub(SPAGES_PER_LPAGE, lv2entcnt);

done:
	iommu_iotlb_gather_add_page(dom, gather, iova, size);

	return size;

err:
	pr_err("failed: size(%#zx) @ %#x is smaller than page size %#zx\n",
	       size, iova, err_pgsize);

	return 0;
}

static void samsung_sysmmu_flush_iotlb_all(struct iommu_domain *dom)
{
	unsigned long flags;
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);
	struct list_head *sysmmu_list;
	struct sysmmu_drvdata *drvdata;

	/*
	 * domain->group might be NULL if flush_iotlb_all is called
	 * before attach_dev. Just ignore it.
	 */
	if (!domain->group)
		return;

	sysmmu_list = iommu_group_get_iommudata(domain->group);

	list_for_each_entry(drvdata, sysmmu_list, list) {
		spin_lock_irqsave(&drvdata->lock, flags);
		if (drvdata->attached_count && drvdata->rpm_count > 0)
			__sysmmu_tlb_invalidate_all(drvdata);
		spin_unlock_irqrestore(&drvdata->lock, flags);
	}
}

static void samsung_sysmmu_iotlb_sync(struct iommu_domain *dom,
				      struct iommu_iotlb_gather *gather)
{
	unsigned long flags;
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);
	struct list_head *sysmmu_list;
	struct sysmmu_drvdata *drvdata;

	/*
	 * domain->group might be NULL if iotlb_sync is called
	 * before attach_dev. Just ignore it.
	 */
	if (!domain->group)
		return;

	sysmmu_list = iommu_group_get_iommudata(domain->group);

	list_for_each_entry(drvdata, sysmmu_list, list) {
		spin_lock_irqsave(&drvdata->lock, flags);
		if (drvdata->attached_count && drvdata->rpm_count > 0)
			__sysmmu_tlb_invalidate(drvdata, gather->start, gather->end);
		spin_unlock_irqrestore(&drvdata->lock, flags);
	}
}

static phys_addr_t samsung_sysmmu_iova_to_phys(struct iommu_domain *dom,
					       dma_addr_t d_iova)
{
	struct samsung_sysmmu_domain *domain = to_sysmmu_domain(dom);
	sysmmu_iova_t iova = (sysmmu_iova_t)d_iova;
	sysmmu_pte_t *entry;
	phys_addr_t phys = 0;

	entry = section_entry(domain->page_table, iova);

	if (lv1ent_section(entry)) {
		phys = section_phys(entry) + section_offs(iova);
	} else if (lv1ent_page(entry)) {
		entry = page_entry(entry, iova);

		if (lv2ent_large(entry))
			phys = lpage_phys(entry) + lpage_offs(iova);
		else if (lv2ent_small(entry))
			phys = spage_phys(entry) + spage_offs(iova);
	}

	return phys;
}

void samsung_sysmmu_dump_pagetable(struct device *dev, dma_addr_t iova)
{
}

static int samsung_sysmmu_add_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct iommu_group *group;
	struct sysmmu_clientdata *client;
	int i;

	if (!fwspec) {
		dev_dbg(dev, "IOMMU instance data is not initialized\n");
		return -ENODEV;
	}

	if (fwspec->ops != &samsung_sysmmu_ops) {
		dev_err(dev, "has different IOMMU ops\n");
		return -ENODEV;
	}

	group = iommu_group_get_for_dev(dev);
	if (IS_ERR(group)) {
		dev_err(dev, "has no suitable IOMMU group. (err:%ld)\n",
			PTR_ERR(group));
		return PTR_ERR(group);
	}

	iommu_group_put(group);

	client = (struct sysmmu_clientdata *)fwspec->iommu_priv;
	if (client->dev_link) {
		dev_info(dev, "is already added. It's okay.\n");
		return 0;
	}
	client->dev_link = kcalloc(client->sysmmu_count,
				   sizeof(*client->dev_link), GFP_KERNEL);
	if (!client->dev_link)
		return -ENOMEM;

	for (i = 0; i < client->sysmmu_count; i++) {
		client->dev_link[i] =
			device_link_add(dev, client->sysmmus[i]->dev,
					DL_FLAG_STATELESS | DL_FLAG_PM_RUNTIME);
		if (!client->dev_link[i]) {
			dev_err(dev, "failed to add device link of %s\n",
				dev_name(client->sysmmus[i]->dev));
			while (i-- > 0)
				device_link_del(client->dev_link[i]);
			return -EINVAL;
		}
		dev_info(dev, "device link to %s\n",
			 dev_name(client->sysmmus[i]->dev));
	}

	return 0;
}

static void samsung_sysmmu_remove_device(struct device *dev)
{
	struct iommu_fwspec *fwspec = dev_iommu_fwspec_get(dev);
	struct sysmmu_clientdata *client;
	int i;

	if (!fwspec || fwspec->ops != &samsung_sysmmu_ops)
		return;

	client = (struct sysmmu_clientdata *)fwspec->iommu_priv;
	for (i = 0; i < client->sysmmu_count; i++)
		device_link_del(client->dev_link[i]);
	kfree(client->dev_link);

	iommu_group_remove_device(dev);
	iommu_fwspec_free(dev);
}

static void samsung_sysmmu_group_data_release(void *iommu_data)
{
	kfree(iommu_data);
}

static struct iommu_group *samsung_sysmmu_device_group(struct device *dev)
{
	struct iommu_group *group;
	struct device_node *np;
	struct platform_device *pdev;
	struct list_head *list;
	bool need_unmanaged_domain = false;

	if (device_iommu_mapped(dev))
		return iommu_group_get(dev);

	np = of_parse_phandle(dev->of_node, "samsung,iommu-group", 0);
	if (!np) {
		dev_err(dev, "group is not registered\n");
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(np);
	if (!pdev) {
		dev_err(dev, "no device in device_node[%s]\n", np->name);
		of_node_put(np);
		return ERR_PTR(-ENODEV);
	}

	if (of_property_read_bool(np, "samsung,unmanaged-domain"))
		need_unmanaged_domain = true;

	of_node_put(np);

	group = platform_get_drvdata(pdev);
	if (!group) {
		dev_err(dev, "no group in device_node[%s]\n", np->name);
		return ERR_PTR(-EPROBE_DEFER);
	}

	if (iommu_group_get_iommudata(group))
		return group;

	list = kzalloc(sizeof(*list), GFP_KERNEL);
	if (!list)
		return ERR_PTR(-ENOMEM);

	INIT_LIST_HEAD(list);
	iommu_group_set_iommudata(group, list,
				  samsung_sysmmu_group_data_release);

	if (need_unmanaged_domain) {
		int ret;
		struct iommu_domain *domain =
				iommu_domain_alloc(&platform_bus_type);

		ret = iommu_attach_group(domain, group);
		if (ret) {
			dev_err(dev, "failed to attach group, ret:%d\n", ret);
			return ERR_PTR(ret);
		}
	}

	return group;
}

static void samsung_sysmmu_clientdata_release(struct device *dev, void *res)
{
	struct sysmmu_clientdata *client = res;

	kfree(client->sysmmus);
}

static int samsung_sysmmu_of_xlate(struct device *dev,
				   struct of_phandle_args *args)
{
	struct platform_device *sysmmu = of_find_device_by_node(args->np);
	struct sysmmu_drvdata *data = platform_get_drvdata(sysmmu);
	struct sysmmu_drvdata **new_link;
	struct sysmmu_clientdata *client;
	struct iommu_fwspec *fwspec;
	unsigned int fwid = 0;
	int ret;

	iommu_device_link(&data->iommu, dev);
	ret = iommu_fwspec_add_ids(dev, &fwid, 1);
	if (ret) {
		dev_err(dev, "failed to add fwspec. (err:%d)\n", ret);
		iommu_device_unlink(&data->iommu, dev);
		return ret;
	}

	fwspec = dev_iommu_fwspec_get(dev);
	if (!fwspec->iommu_priv) {
		client = devres_alloc(samsung_sysmmu_clientdata_release,
				      sizeof(*client), GFP_KERNEL);
		if (!client)
			return -ENOMEM;
		client->dev = dev;
		fwspec->iommu_priv = client;
		devres_add(dev, client);
	}

	client = (struct sysmmu_clientdata *)fwspec->iommu_priv;
	new_link = krealloc(client->sysmmus,
			    sizeof(data) * (client->sysmmu_count + 1),
			    GFP_KERNEL);
	if (!new_link)
		return -ENOMEM;

	client->sysmmus = new_link;
	client->sysmmus[client->sysmmu_count++] = data;

	dev_info(dev, "has sysmmu %s (total count:%d)\n",
		 dev_name(data->dev), client->sysmmu_count);

	return ret;
}

static struct iommu_ops samsung_sysmmu_ops = {
	.capable		= samsung_sysmmu_capable,
	.domain_alloc		= samsung_sysmmu_domain_alloc,
	.domain_free		= samsung_sysmmu_domain_free,
	.attach_dev		= samsung_sysmmu_attach_dev,
	.detach_dev		= samsung_sysmmu_detach_dev,
	.map			= samsung_sysmmu_map,
	.unmap			= samsung_sysmmu_unmap,
	.flush_iotlb_all	= samsung_sysmmu_flush_iotlb_all,
	.iotlb_sync		= samsung_sysmmu_iotlb_sync,
	.iova_to_phys		= samsung_sysmmu_iova_to_phys,
	.add_device		= samsung_sysmmu_add_device,
	.remove_device		= samsung_sysmmu_remove_device,
	.device_group		= samsung_sysmmu_device_group,
	.of_xlate		= samsung_sysmmu_of_xlate,
	.pgsize_bitmap		= SECT_SIZE | LPAGE_SIZE | SPAGE_SIZE,
};

static int sysmmu_get_hw_info(struct sysmmu_drvdata *data)
{
	data->version = __sysmmu_get_hw_version(data);
	data->num_tlb = __sysmmu_get_tlb_num(data);

	/* Default value */
	data->reg_set = sysmmu_reg_set[REG_IDX_DEFAULT];

	if (__sysmmu_get_capa_vcr_enabled(data)) {
		data->reg_set = sysmmu_reg_set[REG_IDX_VM];
		data->has_vcr = true;
	}
	if (__sysmmu_get_capa_no_block_mode(data))
		data->no_block_mode = true;

	return 0;
}

static int sysmmu_parse_tlb_property(struct device *dev,
				     struct sysmmu_drvdata *drvdata)
{
	const char *default_props_name = "sysmmu,default_tlb";
	const char *props_name = "sysmmu,tlb_property";
	struct tlb_props *tlb_props = &drvdata->tlb_props;
	struct tlb_config *cfg;
	int i, readsize, cnt, ret;

	if (of_property_read_u32(dev->of_node, default_props_name,
				 &tlb_props->default_cfg))
		tlb_props->default_cfg = DEFAULT_TLB_NONE;

	cnt = of_property_count_elems_of_size(dev->of_node, props_name,
					      sizeof(*cfg));
	if (cnt <= 0)
		return 0;

	cfg = devm_kcalloc(dev, cnt, sizeof(*cfg), GFP_KERNEL);
	if (!cfg)
		return -ENOMEM;

	readsize = cnt * sizeof(*cfg) / sizeof(u32);
	ret = of_property_read_variable_u32_array(dev->of_node, props_name,
						  (u32 *)cfg,
						  readsize, readsize);
	if (ret < 0) {
		dev_err(dev, "failed to get tlb property, return %d\n", ret);
		return ret;
	}

	for (i = 0; i < cnt; i++) {
		if (cfg[i].index >= drvdata->num_tlb) {
			dev_err(dev, "invalid index %d is ignored. (max:%d)\n",
				cfg[i].index, drvdata->num_tlb);
			cfg[i].index = UNUSED_TLB_INDEX;
		}
	}

	tlb_props->id_cnt = cnt;
	tlb_props->cfg = cfg;

	return 0;
}

static int __sysmmu_secure_irq_init(struct device *sysmmu,
				    struct sysmmu_drvdata *data)
{
	struct platform_device *pdev = to_platform_device(sysmmu);
	int ret;

	ret = platform_get_irq(pdev, 1);
	if (ret <= 0) {
		dev_err(sysmmu, "unable to find secure IRQ resource\n");
		return -EINVAL;
	}
	data->secure_irq = ret;

	ret = devm_request_threaded_irq(sysmmu, data->secure_irq,
					samsung_sysmmu_irq,
					samsung_sysmmu_irq_thread,
					IRQF_ONESHOT, dev_name(sysmmu), data);
	if (ret) {
		dev_err(sysmmu, "failed to set secure irq handler %d, ret:%d\n",
			data->secure_irq, ret);
		return ret;
	}

	ret = of_property_read_u32(sysmmu->of_node, "sysmmu,secure_base",
				   &data->secure_base);
	if (ret) {
		dev_err(sysmmu, "failed to get secure base\n");
		return ret;
	}
	dev_info(sysmmu, "secure base = %#x\n", data->secure_base);

	return ret;
}

static int sysmmu_parse_dt(struct device *sysmmu, struct sysmmu_drvdata *data)
{
	unsigned int qos = DEFAULT_QOS_VALUE;
	int ret;

	/* Parsing QoS */
	ret = of_property_read_u32_index(sysmmu->of_node, "qos", 0, &qos);
	if (!ret && qos > 15) {
		dev_err(sysmmu, "Invalid QoS value %d, use default.\n", qos);
		qos = DEFAULT_QOS_VALUE;
	}

	data->qos = qos;

	/* Secure IRQ */
	if (of_find_property(sysmmu->of_node, "sysmmu,secure-irq", NULL)) {
		ret = __sysmmu_secure_irq_init(sysmmu, data);
		if (ret) {
			dev_err(sysmmu, "failed to init secure irq\n");
			return ret;
		}
	}

	/* use async fault mode */
	data->async_fault_mode = of_property_read_bool(sysmmu->of_node,
						       "sysmmu,async-fault");

	ret = sysmmu_parse_tlb_property(sysmmu, data);
	if (ret)
		dev_err(sysmmu, "Failed to parse TLB property\n");

	return ret;
}

static int samsung_sysmmu_init_global(void)
{
	int ret = 0;

	flpt_cache = kmem_cache_create("samsung-iommu-lv1table",
				       LV1TABLE_SIZE, LV1TABLE_SIZE,
				       0, NULL);
	if (!flpt_cache)
		return -ENOMEM;

	slpt_cache = kmem_cache_create("samsung-iommu-lv2table",
				       LV2TABLE_SIZE, LV2TABLE_SIZE,
				       0, NULL);
	if (!slpt_cache) {
		ret = -ENOMEM;
		goto err_init_slpt_fail;
	}

	bus_set_iommu(&platform_bus_type, &samsung_sysmmu_ops);

	device_initialize(&sync_dev);
	sysmmu_global_init_done = true;

	return 0;

err_init_slpt_fail:
	kmem_cache_destroy(flpt_cache);

	return ret;
}

static int samsung_sysmmu_device_probe(struct platform_device *pdev)
{
	struct sysmmu_drvdata *data;
	struct device *dev = &pdev->dev;
	struct resource *res;
	int irq, ret, err = 0;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		dev_err(dev, "failed to get resource info\n");
		return -ENOENT;
	}

	data->sfrbase = devm_ioremap_resource(dev, res);
	if (IS_ERR(data->sfrbase))
		return PTR_ERR(data->sfrbase);

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_threaded_irq(dev, irq, samsung_sysmmu_irq,
					samsung_sysmmu_irq_thread,
					IRQF_ONESHOT, dev_name(dev), data);
	if (ret) {
		dev_err(dev, "unabled to register handler of irq %d\n", irq);
		return ret;
	}

	data->clk = devm_clk_get(dev, "gate");
	if (PTR_ERR(data->clk) == -ENOENT) {
		dev_info(dev, "no gate clock exists. it's okay.\n");
		data->clk = NULL;
	} else if (IS_ERR(data->clk)) {
		dev_err(dev, "failed to get clock!\n");
		return PTR_ERR(data->clk);
	}

	ret = sysmmu_get_hw_info(data);
	if (ret) {
		dev_err(dev, "failed to get h/w info\n");
		return ret;
	}

	INIT_LIST_HEAD(&data->list);
	spin_lock_init(&data->lock);
	data->dev = dev;

	ret = sysmmu_parse_dt(data->dev, data);
	if (ret)
		return ret;

	ret = iommu_device_sysfs_add(&data->iommu, data->dev,
				     NULL, dev_name(dev));
	if (ret) {
		dev_err(dev, "failed to register iommu in sysfs\n");
		return ret;
	}

	iommu_device_set_ops(&data->iommu, &samsung_sysmmu_ops);
	iommu_device_set_fwnode(&data->iommu, dev->fwnode);

	err = iommu_device_register(&data->iommu);
	if (err) {
		dev_err(dev, "failed to register iommu\n");
		goto err_iommu_register;
	}

	mutex_lock(&sysmmu_global_mutex);
	if (!sysmmu_global_init_done) {
		err = samsung_sysmmu_init_global();
		if (err) {
			dev_err(dev, "failed to initialize global data\n");
			mutex_unlock(&sysmmu_global_mutex);
			goto err_global_init;
		}
	}
	mutex_unlock(&sysmmu_global_mutex);
	pm_runtime_enable(dev);

	platform_set_drvdata(pdev, data);

	dev_info(dev, "initialized IOMMU. Ver %d.%d.%d\n",
		 MMU_MAJ_VER(data->version),
		 MMU_MIN_VER(data->version),
		 MMU_REV_VER(data->version));
	return 0;

err_global_init:
	iommu_device_unregister(&data->iommu);
err_iommu_register:
	iommu_device_sysfs_remove(&data->iommu);
	return err;
}

static void samsung_sysmmu_device_shutdown(struct platform_device *pdev)
{
}

static int __maybe_unused samsung_sysmmu_runtime_suspend(struct device *sysmmu)
{
	unsigned long flags;
	struct sysmmu_drvdata *drvdata = dev_get_drvdata(sysmmu);

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->rpm_count--;
	if (drvdata->attached_count > 0)
		__sysmmu_disable(drvdata);
	spin_unlock_irqrestore(&drvdata->lock, flags);

	return 0;
}

static int __maybe_unused samsung_sysmmu_runtime_resume(struct device *sysmmu)
{
	unsigned long flags;
	struct sysmmu_drvdata *drvdata = dev_get_drvdata(sysmmu);

	spin_lock_irqsave(&drvdata->lock, flags);
	drvdata->rpm_count++;
	if (drvdata->attached_count > 0)
		__sysmmu_enable(drvdata);
	spin_unlock_irqrestore(&drvdata->lock, flags);

	return 0;
}

static int __maybe_unused samsung_sysmmu_suspend(struct device *dev)
{
	if (pm_runtime_status_suspended(dev))
		return 0;

	return samsung_sysmmu_runtime_suspend(dev);
}

static int __maybe_unused samsung_sysmmu_resume(struct device *dev)
{
	if (pm_runtime_status_suspended(dev))
		return 0;

	return samsung_sysmmu_runtime_resume(dev);
}

static const struct dev_pm_ops samsung_sysmmu_pm_ops = {
	SET_RUNTIME_PM_OPS(samsung_sysmmu_runtime_suspend,
			   samsung_sysmmu_runtime_resume, NULL)
	SET_LATE_SYSTEM_SLEEP_PM_OPS(samsung_sysmmu_suspend,
				     samsung_sysmmu_resume)
};

static const struct of_device_id sysmmu_of_match[] = {
	{ .compatible = "samsung,sysmmu-v8" },
	{ }
};

static struct platform_driver samsung_sysmmu_driver = {
	.driver	= {
		.name			= "samsung-sysmmu",
		.of_match_table		= of_match_ptr(sysmmu_of_match),
		.pm			= &samsung_sysmmu_pm_ops,
		.suppress_bind_attrs	= true,
	},
	.probe	= samsung_sysmmu_device_probe,
	.shutdown = samsung_sysmmu_device_shutdown,
};
module_platform_driver(samsung_sysmmu_driver);
MODULE_SOFTDEP("pre: samsung-iommu-group");
MODULE_LICENSE("GPL v2");
