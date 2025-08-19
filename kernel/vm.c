#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"

static pte_t* walkl1(pagetable_t pagetable, uint64 va, int alloc);

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// 把覆盖 va 的 L1 叶子超页降级成 L0 表（512 个 4KB PTE）
// 把覆盖 va 的 L1 叶子超页“复制式”降级成 L0：
// - 为 2MB 内的每个 4KB 重新分配一页并复制数据；
// - 将原 2MB 超页整块归还 super 池；
// - L1 由叶子改为指向新 L0 表的中间项（仅 PTE_V）。
static void
demote_superpage(pagetable_t pagetable, uint64 va)
{
  pte_t *pte1 = walkl1(pagetable, va, 0);
  if(pte1 == 0) return;
  if( ((*pte1 & PTE_V) == 0) || ((*pte1 & (PTE_R|PTE_W|PTE_X)) == 0) )
    return; // 非 L1 叶子，无需降级

  uint64 pa2m = PTE2PA(*pte1);
  int flags = PTE_FLAGS(*pte1) & (PTE_R|PTE_W|PTE_X|PTE_U);

  // 新建一个 L0 表
  pagetable_t l0 = (pagetable_t)kalloc();
  if(l0 == 0) panic("demote_superpage: kalloc l0");
  memset(l0, 0, PGSIZE);

  // 逐 4KB 分配并复制
  for(int i = 0; i < 512; i++){
    char *page = (char*)kalloc();
    if(page == 0) panic("demote_superpage: kalloc page");
    // 从原 2MB 区域拷贝这一页的数据
    memmove(page, (void*)(pa2m + (uint64)i * PGSIZE), PGSIZE);
    l0[i] = PA2PTE((uint64)page) | flags | PTE_V;
  }

  // 先把 L1 改成指向 L0 的中间项（避免窗口期的悬空）
  *pte1 = PA2PTE((uint64)l0) | PTE_V;

  // 最后把原 2MB 超页整块归还 super 池
  superfree((void*)pa2m);
}



// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

#ifdef LAB_NET
  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);
#endif  

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);

      if(PTE_LEAF(*pte)) {
        return pte;
      }

    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}


static pte_t*
walkl1(pagetable_t pagetable, uint64 va, int alloc)
{
  pte_t *pte2 = &pagetable[PX(2, va)];
  if((*pte2 & PTE_V) == 0){
    if(!alloc) return 0;
    pagetable_t newpt = (pagetable_t)kalloc();
    if(newpt == 0) return 0;
    memset(newpt, 0, PGSIZE);
    *pte2 = PA2PTE((uint64)newpt) | PTE_V; 
  }
  pagetable_t l1 = (pagetable_t)PTE2PA(*pte2);
  return &l1[PX(1, va)];
}

static inline int
is_l1_leaf(pte_t pte)
{
  return (pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X));
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  if(va >= MAXVA)
    return 0;

  pagetable_t pt = pagetable;

  // L2
  pte_t *pte2 = &pt[PX(2, va)];
  if((*pte2 & PTE_V) == 0) return 0;
  if((*pte2 & (PTE_R|PTE_W|PTE_X)) != 0){
    if((*pte2 & PTE_U) == 0) return 0;
    uint64 pa = PTE2PA(*pte2);
    uint64 off = va & ((1ULL<<(PGSHIFT + 2*9)) - 1);
    return pa + off;
  }
  pt = (pagetable_t)PTE2PA(*pte2);

  // L1
  pte_t *pte1 = &pt[PX(1, va)];
  if((*pte1 & PTE_V) == 0) return 0;
  if((*pte1 & (PTE_R|PTE_W|PTE_X)) != 0){
    // 命中 2MB 超页
    if((*pte1 & PTE_U) == 0) return 0;
    uint64 pa = PTE2PA(*pte1);
    uint64 off = va & ((1ULL<<(PGSHIFT + 1*9)) - 1); 
    return pa + off;
  }
  pt = (pagetable_t)PTE2PA(*pte1);

  // L0
  pte_t *pte0 = &pt[PX(0, va)];
  if((*pte0 & PTE_V) == 0) return 0;
  if((*pte0 & PTE_U) == 0) return 0;
  return PTE2PA(*pte0) + (va & (PGSIZE-1));
}


// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

pte_t *
superwalk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("superwalk");

  pte_t *pte = &pagetable[PX(2, va)];
  if(*pte & PTE_V) {
    pagetable = (pagetable_t)PTE2PA(*pte);
    return &pagetable[PX(1, va)];
  } else {
    if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
      return 0;
    memset(pagetable, 0, PGSIZE);
    *pte = PA2PTE(pagetable) | PTE_V;
    return &pagetable[PX(1, va)];
  }
}
// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  uint64 pgsize;
  pte_t *pte;
  if (pa >= SUPERBASE) pgsize = SUPERPGSIZE;
  else pgsize = PGSIZE;
  if((va % pgsize) != 0)
    panic("mappages: va not aligned");

  if((size % pgsize) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - pgsize;
  for(;;){
    if(pgsize == PGSIZE && (pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if (pgsize == SUPERPGSIZE && (pte = superwalk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += pgsize;
    pa += pgsize;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;
  int sz;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += sz){
    sz = PGSIZE;
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0) {
      printf("va=%ld pte=%ld\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    uint64 pa = PTE2PA(*pte);
    if (pa >= SUPERBASE){
      a += SUPERPGSIZE;
      a -= sz;
    }
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      if (pa >= SUPERBASE) superfree((void*)pa);
      else kfree((void*)pa);
    }
    *pte = 0;
  }
}



// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}


// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;
  int sz;

  if(newsz < oldsz)
    return oldsz;
  // page ... page superpage ... superpage page ... page
  oldsz = PGROUNDUP(oldsz);
  
  // 分配page直到对齐
  for(a = oldsz; a < SUPERPGROUNDUP(oldsz) && a < newsz; a += sz){
    sz = PGSIZE;
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
#ifndef LAB_SYSCALL
    memset(mem, 0, sz);
#endif
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
	
  // 尽可能多的分配superpage
  for (; a + SUPERPGSIZE < newsz; a += sz)
  {
    sz = SUPERPGSIZE;
    mem = superalloc();
    if (mem == 0){
      break;
    }
    memset(mem, 0, sz);
    if (mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      superfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  
  // 为剩余内存分配page
  for(; a < newsz; a += sz){
    sz = PGSIZE;
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, sz);
    if(mappages(pagetable, a, sz, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}


// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}
// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;
  int szinc;
  for(i = 0; i < sz; i += szinc){
    szinc = PGSIZE;
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if (pa >= SUPERBASE){
      szinc = SUPERPGSIZE;
      if ((mem = superalloc()) == 0)
        goto err;
    }
    else if ((mem = kalloc()) == 0) 
      goto err;
    memmove(mem, (char*)pa, szinc);
    if(mappages(new, i, szinc, (uint64)mem, flags) != 0){
      if (szinc == PGSIZE) kfree(mem);
      else superfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}


// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if (va0 >= MAXVA)
      return -1;
    if((pte = walk(pagetable, va0, 0)) == 0) {
      // printf("copyout: pte should exist 0x%x %d\n", dstva, len);
      return -1;
    }


    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
    
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;
  
  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

void
vmprinthelper(pagetable_t pagetable, int level, uint64 va)
{
  uint64 sz = 0;
  if (level == 2) sz = 512 * 512 * PGSIZE;
  else if (level == 1) sz = 512 * PGSIZE;
  else sz = PGSIZE;
  for(int i = 0; i < 512; i++, va += sz){
    pte_t pte = pagetable[i];
    if ((pte & PTE_V) == 0) continue;
    for (int j = 0; j < 3 - level; ++j) printf(" ..");
    printf("%p: ", (void*)va);
    printf("pte %p pa %p\n", (void*)pte, (void*)PTE2PA(pte));
    if ((pte & (PTE_R|PTE_W|PTE_X)) == 0)
      vmprinthelper((void*)PTE2PA(pte), level - 1, va);
  }
}

void
vmprint(pagetable_t pagetable) {
  // your code here
  printf("page table %p\n", pagetable);
  vmprinthelper(pagetable, 2, 0);
}




pte_t*
pgpte(pagetable_t pagetable, uint64 va) {
  return walk(pagetable, va, 0);
}

void
uvmclear(pagetable_t pagetable, uint64 va)
{
  // 若 va 落在 L1 叶子超页里，先把该 2MB 超页降级成 L0 512 个 4KB PTE
  pte_t *l1 = walkl1(pagetable, va, 0);
  if (l1 && (*l1 & PTE_V) && (*l1 & (PTE_R|PTE_W|PTE_X)))
    demote_superpage(pagetable, va);

  // 现在拿到 L0 的 PTE 再清用户位，只影响这一页（4KB）
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}
