// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void* superalloc(void);
void  superfree(void *pa);

// ==== superpage pool (2MB chunks) ====
struct superrun {
  struct superrun *next;
};

static struct {
  struct spinlock lock;
  struct superrun *freelist;   // 每个节点代表一个 2MB 大块（对齐）
  int nfree;
} sp;

#define NSUPER_RESERVE 8  // 预留的 2MB 大块个数（足够跑测试）


void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};


struct {
  struct spinlock lock;
  struct run *freelist;
} kmem, supermem;

void
superfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % SUPERPGSIZE) != 0 || (char*)pa < (char*)SUPERBASE || (uint64)pa >= PHYSTOP)
    panic("superfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, SUPERPGSIZE);

  r = (struct run*)pa;

  acquire(&supermem.lock);
  r->next = supermem.freelist;
  supermem.freelist = r;
  release(&supermem.lock);
}

// Allocate one 2MB page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
superalloc(void)
{
  struct run *r;

  acquire(&supermem.lock);
  r = supermem.freelist;
  if(r)
    supermem.freelist = r->next;
  release(&supermem.lock);

  if(r)
    memset((char*)r, 5, SUPERPGSIZE); // fill with junk
  return (void*)r;
}

void
superinit()
{
  initlock(&supermem.lock, "supermem");
  char *p = (char*) SUPERPGROUNDUP(SUPERBASE);
  for (; p + SUPERPGSIZE <= (char*)PHYSTOP; p += SUPERPGSIZE)
    superfree(p);
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)SUPERBASE);
  superinit();
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p = (char*)PGROUNDUP((uint64)pa_start);
  // 先预留若干 2MB 对齐的大块到 superpage 池里
  uint64 start = (uint64)p;
  uint64 s = ((start + SUPERPGSIZE - 1) / SUPERPGSIZE) * SUPERPGSIZE;
  int reserved = 0;

  while (reserved < NSUPER_RESERVE && s + SUPERPGSIZE <= (uint64)pa_end) {
    // 把 [s, s+2MB) 这 512 个页 **不**放入普通 freelist，而是作为一个 2MB 大块入 super 池
    acquire(&sp.lock);
    struct superrun *sr = (struct superrun*)s;
    sr->next = sp.freelist;
    sp.freelist = sr;
    sp.nfree++;
    release(&sp.lock);

    // 跳过这一段（不调用 kfree）
    s += SUPERPGSIZE;
    reserved++;
  }

  // 其余内存按 4KB 页加入普通 freelist（跳过已预留的 2MB 段）
  for (; (uint64)p + PGSIZE <= (uint64)pa_end; p += PGSIZE) {
    uint64 addr = (uint64)p;
    // 若落在任何已预留 2MB 区间内，跳过

    // 检查该 2MB 区间是不是我们的 super 池成员：方法是看它的首地址是否处于 [start, start+NSUPER*2MB)，且我们预留序列覆盖了它
    // 简化：只要 addr 位于 [first_reserved, first_reserved + NSUPER_RESERVE*2MB) 且 aligned2m 是 2MB 对齐，就认为在预留段内
    // 更稳妥：直接判断 addr 是否在任何 sr 起点到 +2MB 之间（成本略大）。因 NSUPER_RESERVE 很小，直接循环即可。
    int in_reserved = 0;
    acquire(&sp.lock);
    for (struct superrun* cur = sp.freelist; cur; cur = cur->next) {
      uint64 base = (uint64)cur;
      if (addr >= base && addr < base + SUPERPGSIZE) { in_reserved = 1; break; }
    }
    release(&sp.lock);
    if (in_reserved) continue;

    kfree(p);
  }
}



// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
