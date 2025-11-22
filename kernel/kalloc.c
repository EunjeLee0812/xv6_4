// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[(PHYSTOP-KERNBASE)/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
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
// pa4: kalloc function

void *
kalloc(void)
{
  struct run *r;

  for(;;) {
    // 1) 먼저 평범하게 freelist에서 한 번 뽑아봅니다.
    acquire(&kmem.lock);
    r = kmem.freelist;
    if(r){
      kmem.freelist = r->next;
      release(&kmem.lock);
      break;              // 페이지 하나 성공적으로 확보 → 루프 탈출
    }
    release(&kmem.lock);

    // 2) 여기서는 어떤 스핀락도 잡으면 안 됩니다!
    //    이 상태에서 swapout() 으로 물리 페이지 하나 디스크로 내보내기
    if(swapout() < 0){
      // 스왑할 것도 없어서 swapout 실패 → 그냥 포기
      r = 0;
      break;
    }
    // 3) swapout 성공했으면, 다시 freelist를 보러 루프 맨 앞으로
    //    (새로 free된 페이지가 freelist에 들어갔을 테니까)
  }

  if(r)
    memset((char*)r, 5, PGSIZE);  // 디버그용 패턴 채우기

  return (void*)r;
}

