//******************************************************************************
// Copyright (c) 2018, The Regents of the University of California (Regents).
// All Rights Reserved. See LICENSE for license details.
//------------------------------------------------------------------------------
#include "riscv64.h"
#include <linux/kernel.h>
#include "keystone.h"
#include <linux/dma-mapping.h>

void init_free_pages(struct list_head* pg_list, vaddr_t ptr, unsigned int count)
{
  unsigned int i;
  vaddr_t cur;
  cur = ptr;
  for(i=0; i<count; i++)
  {
    put_free_page(pg_list, cur);
    cur += RISCV_PGSIZE;
  }
  return;
}

vaddr_t get_free_page(struct list_head* pg_list)
{
  struct free_page_t* page;
  vaddr_t addr;

  if(list_empty(pg_list))
    return 0;

  page = list_first_entry(pg_list, struct free_page_t, freelist);
  addr = page->vaddr;
  list_del(&page->freelist);
  kfree(page);

  return addr;
}

void put_free_page(struct list_head* pg_list, vaddr_t page_addr)
{
  struct free_page_t* page = kmalloc(sizeof(struct free_page_t),GFP_KERNEL);
  page->vaddr = page_addr;
  list_add_tail(&page->freelist, pg_list);
  return;
}

/* Destroy all memory associated with an EPM */
int epm_destroy(epm_t* epm) {

  /* Clean anything in the free list */
  epm_clean_free_list(epm);

  if(!epm->ptr || !epm->size)
    return 0;

  //keystone_info("[epm_destroy] final encl size: 0x%lx\r\n",(epm->oversize - (epm->tmp_dynamic_pages << PAGE_SHIFT)));

  /* free the EPM hold by the enclave */
  /* START SIM MODIFICATIONS
   * need to free the whole thing
   */
  if (epm->is_cma) {
    dma_free_coherent(keystone_dev.this_device,
        epm->oversize,
        (void*) epm->ptr,
        epm->pa);
  } else {
    free_pages(epm->ptr, epm->order);
  }
  /* END SIM MODIFICCATIONS */

  return 0;
}

/* Create an EPM and initialize the free list */
int epm_init(epm_t* epm, unsigned int min_pages)
{
  pte_t* t;

  vaddr_t epm_vaddr = 0;
  unsigned long order = 0;
  unsigned long count = min_pages;
  phys_addr_t device_phys_addr = 0;

  /* Always init the head */
  INIT_LIST_HEAD(&epm->epm_free_list);

  /* try to allocate contiguous memory */
  epm->is_cma = 0;
  order = ilog2(min_pages - 1) + 1;

  /* START SIM MODIFICATIONS
   * allocate more pages to simulate dynamic page allocation, always CMA
   */
  epm->tmp_dynamic_pages = EPM_OVERSIZE - count;
  epm->oversize = EPM_OVERSIZE << PAGE_SHIFT;

  /* /\* prevent kernel from complaining about an invalid argument *\/ */
  /* if (order <= MAX_ORDER){ */
  /*   epm_vaddr = (vaddr_t) __get_free_pages(GFP_HIGHUSER, order); */
  /*   count = 0x1 << order; */
  /* } */
#ifdef CONFIG_CMA
  /* If buddy allocator fails, we fall back to the CMA */
  if (!epm_vaddr) {
    epm->is_cma = 1;

    /* SIM MODIFICATION: ALWAYS OVERSIZE */
    epm_vaddr = (vaddr_t) dma_alloc_coherent(keystone_dev.this_device,
      epm->oversize,
      &device_phys_addr,
      GFP_KERNEL);

    if(!device_phys_addr)
      epm_vaddr = 0;
  }
#endif
  /* STOP SIM MODIFICATIONS */

  if(!epm_vaddr) {
    keystone_err("failed to allocate %lu page(s) [CMA:%d]\n", count, epm->is_cma);
    return -ENOMEM;
  }

  /* zero out */
  memset((void*)epm_vaddr, 0, PAGE_SIZE*count);
  init_free_pages(&epm->epm_free_list, epm_vaddr, count);

  /* The first free page will be the enclave's top-level page table */
  t = (pte_t*) get_free_page(&epm->epm_free_list);
  if (!t) {
    return -ENOMEM;
  }

  epm->root_page_table = t;
  epm->pa = __pa(epm_vaddr);
  epm->order = order;
  epm->size = count << PAGE_SHIFT;

  epm->ptr = epm_vaddr;

  return 0;
}

int epm_clean_free_list(epm_t* epm)
{
  struct free_page_t* page;
  struct list_head* pg_list;
  pg_list = &epm->epm_free_list;
  while (!list_empty(&epm->epm_free_list))
  {
    page = list_first_entry(pg_list, struct free_page_t, freelist);
    list_del(&page->freelist);
    kfree(page);
  }
  return 0;
}

/* Returns the number of pages actually allocated */
int epm_request_extend(epm_t* epm, uintptr_t pages){
  // We doubled the initial allocation for testing, so assume there
  // are good pages available.

  //keystone_info("EXTEND happening for %lu (%u avail)\r\n", pages, epm->tmp_dynamic_pages);
  if(epm->tmp_dynamic_pages >= pages){
    epm->tmp_dynamic_pages = epm->tmp_dynamic_pages - pages;
    return pages;
  }
  else{
    keystone_err("Extend request failed, out of pages!\r\n");
    return 0;
  }
}

int utm_destroy(utm_t* utm){

  /* Clean anything in the free list */
  utm_clean_free_list(utm);

  if(utm->ptr != NULL){
    free_pages((vaddr_t)utm->ptr, utm->order);
  }

  return 0;
}

int utm_clean_free_list(utm_t* utm)
{
  struct free_page_t* page;
  struct list_head* pg_list;
  pg_list = &utm->utm_free_list;
  while (!list_empty(&utm->utm_free_list))
  {
    page = list_first_entry(pg_list, struct free_page_t, freelist);
    list_del(&page->freelist);
    kfree(page);
  }
  return 0;
}

int utm_init(utm_t* utm, size_t untrusted_size)
{
  unsigned long req_pages = 0;
  unsigned long order = 0;
  unsigned long count;
  req_pages += PAGE_UP(untrusted_size)/PAGE_SIZE;
  order = ilog2(req_pages - 1) + 1;
  count = 0x1 << order;

  utm->order = order;

  /* Currently, UTM does not utilize CMA.
   * It is always allocated from the buddy allocator */
  utm->ptr = (void*) __get_free_pages(GFP_HIGHUSER, order);
  if (!utm->ptr) {
    return -ENOMEM;
  }

  utm->size = count * PAGE_SIZE;
  if (utm->size != untrusted_size) {
    /* Instead of failing, we just warn that the user has to fix the parameter. */
    keystone_warn("shared buffer size is not multiple of PAGE_SIZE\n");
  }

  INIT_LIST_HEAD(&utm->utm_free_list);
  init_free_pages(&utm->utm_free_list, (vaddr_t)utm->ptr, utm->size/PAGE_SIZE);
  return 0;
}

static paddr_t pte_ppn(pte_t pte)
{
  return pte_val(pte) >> PTE_PPN_SHIFT;
}

static paddr_t ppn(vaddr_t addr)
{
  return __pa(addr) >> RISCV_PGSHIFT;
}

static size_t pt_idx(vaddr_t addr, int level)
{
  size_t idx = addr >> (RISCV_PGLEVEL_BITS*level + RISCV_PGSHIFT);
  return idx & ((1 << RISCV_PGLEVEL_BITS) - 1);
}

static pte_t* __ept_walk_create(struct list_head* pg_list, pte_t* root_page_table, vaddr_t addr);

static pte_t* __ept_continue_walk_create(struct list_head* pg_list, pte_t* root_page_table, vaddr_t addr, pte_t* pte)
{
  unsigned long free_ppn = ppn(get_free_page(pg_list));
  *pte = ptd_create(free_ppn);
  //pr_info("ptd_create: ppn = %u, pte = 0x%lx\n", free_ppn,  *pte);
  return __ept_walk_create(pg_list, root_page_table, addr);
}

static pte_t* __ept_walk_internal(struct list_head* pg_list, pte_t* root_page_table, vaddr_t addr, int create)
{
  pte_t* t = root_page_table;
  //pr_info("  page walk:\n");
  int i;
  for (i = (VA_BITS - RISCV_PGSHIFT) / RISCV_PGLEVEL_BITS - 1; i > 0; i--) {
    size_t idx = pt_idx(addr, i);
    //pr_info("    level %d: pt_idx %d (%x)\n", i, idx, idx);
    if (unlikely(!(pte_val(t[idx]) & PTE_V)))
      return create ? __ept_continue_walk_create(pg_list, root_page_table, addr, &t[idx]) : 0;
    t = (pte_t*) __va(pte_ppn(t[idx]) << RISCV_PGSHIFT);
  }
  return &t[pt_idx(addr, 0)];
}

static pte_t* __ept_walk(struct list_head* pg_list, pte_t* root_page_table, vaddr_t addr)
{
  return __ept_walk_internal(pg_list, root_page_table, addr, 0);
}

static pte_t* __ept_walk_create(struct list_head* pg_list, pte_t* root_page_table, vaddr_t addr)
{
  return __ept_walk_internal(pg_list, root_page_table, addr, 1);
}

/*
static int __ept_va_avail(epm_t* epm, vaddr_t vaddr)
{
  pte_t* pte = __ept_walk(epm, vaddr);
  return pte == 0 || pte_val(*pte) == 0;
}
*/

paddr_t epm_get_free_pa(epm_t* epm)
{
  struct free_page_t* page;
  struct list_head* pg_list;
  paddr_t addr;

  pg_list = &(epm->epm_free_list);

  if(list_empty(pg_list))
    return 0;

  page = list_first_entry(pg_list, struct free_page_t, freelist);
  return __pa(page->vaddr);
}

paddr_t epm_va_to_pa(epm_t* epm, vaddr_t addr)
{
  pte_t* pte = __ept_walk(NULL, epm->root_page_table,addr);
  if(pte)
    return pte_ppn(*pte) << RISCV_PGSHIFT;
  else
    return 0;
}

/* This function pre-allocates the required page tables so that
 * the virtual addresses are linearly mapped to the physical memory */
size_t epm_alloc_vspace(epm_t* epm, vaddr_t addr, size_t num_pages)
{
  vaddr_t walk;
  size_t count;

  for(walk=addr, count=0; count < num_pages; count++, addr += PAGE_SIZE)
  {
    pte_t* pte = __ept_walk_create(&epm->epm_free_list, epm->root_page_table, addr);
    if(!pte)
      break;
  }

  return count;
}


vaddr_t utm_alloc_page(utm_t* utm, epm_t* epm, vaddr_t addr, unsigned long flags)
{
  vaddr_t page_addr;
  pte_t* pte = __ept_walk_create(&epm->epm_free_list, epm->root_page_table, addr);

  /* if the page has been already allocated, return the page */
  if(pte_val(*pte) & PTE_V) {
    return (vaddr_t) __va(pte_ppn(*pte) << RISCV_PGSHIFT);
  }

	/* otherwise, allocate one from UTM freelist */
  page_addr = get_free_page(&utm->utm_free_list);
  *pte = pte_create(ppn(page_addr), flags | PTE_V);
  return page_addr;
}

vaddr_t epm_alloc_page(epm_t* epm, vaddr_t addr, unsigned long flags)
{
  vaddr_t page_addr;
  pte_t* pte = __ept_walk_create(&epm->epm_free_list, epm->root_page_table, addr);

	/* if the page has been already allocated, return the page */
  if(pte_val(*pte) & PTE_V) {
    return (vaddr_t) __va(pte_ppn(*pte) << RISCV_PGSHIFT);
  }

	/* otherwise, allocate one from EPM freelist */
  page_addr = get_free_page(&epm->epm_free_list);
  *pte = pte_create(ppn(page_addr), flags | PTE_V);
  return page_addr;
}

vaddr_t epm_alloc_rt_page_noexec(epm_t* epm, vaddr_t addr)
{
  return epm_alloc_page(epm, addr, PTE_D | PTE_A | PTE_R | PTE_W);
}

vaddr_t epm_alloc_rt_page(epm_t* epm, vaddr_t addr)
{
  return epm_alloc_page(epm, addr, PTE_D | PTE_A | PTE_R | PTE_W | PTE_X);
}

vaddr_t epm_alloc_user_page_noexec(epm_t* epm, vaddr_t addr)
{
  return epm_alloc_page(epm, addr, PTE_D | PTE_A | PTE_R | PTE_W | PTE_U);
}

vaddr_t epm_alloc_user_page(epm_t* epm, vaddr_t addr)
{
  return epm_alloc_page(epm, addr, PTE_D | PTE_A | PTE_R | PTE_X | PTE_W | PTE_U);
}
