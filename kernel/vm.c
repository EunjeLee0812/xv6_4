#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "spinlock.h"

/*
 * the kernel's page table.
 */
struct page *lru_head = 0;
struct page *lru_tail = 0;
struct spinlock lru_lock;

static uint8 *swap_bitmap;
static uint32 swap_slots;
struct spinlock swap_lock;

extern struct page pages[(PHYSTOP-KERNBASE)/PGSIZE];

pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

static int
pa2pageidx(uint64 pa)
{
  if(pa < KERNBASE || pa >= PHYSTOP)
    panic("pa2pageidx: pa out of range");
  return (pa - KERNBASE) / PGSIZE;
}

static int
alloc_swap_slot(void)
{
    acquire(&swap_lock);
    for(int i = 0; i < swap_slots; i++){
        int byte = i / 8;
        int bit  = i % 8;
        if((swap_bitmap[byte] & (1 << bit)) == 0){
            swap_bitmap[byte] |= (1 << bit);
            release(&swap_lock);
            return i;
        }
    }
    release(&swap_lock);
    return -1;
}

static void
free_swap_slot(int idx)
{
    acquire(&swap_lock);
    int byte = idx / 8;
    int bit  = idx % 8;
    swap_bitmap[byte] &= ~(1 << bit);
    release(&swap_lock);
}

static uint64
page_to_pa(struct page *pg)
{
  int idx = pg - pages;
  return KERNBASE +  (uint64)idx * PGSIZE;
}

void
lru_add(struct page *pg)
{
    if(lru_head == 0) {
        lru_head = lru_tail = pg;
        pg->next = pg->prev = pg; // 자기 자신 가리키는 원형 리스트
    }
    else{
        pg->prev = lru_tail;
        pg->next = lru_head;
        lru_tail->next = pg;
        lru_head->prev = pg;
        lru_tail = pg;
    }
}

void
lru_remove(struct page *pg)
{
    if(pg->next == 0 || pg->prev == 0) {
        return;
    }
    if(pg == lru_head && pg == lru_tail) {
        lru_head = lru_tail = 0;
    }
    else{
        if(pg == lru_head) lru_head = pg->next;
        if(pg == lru_tail) lru_tail = pg->prev;
        pg->prev->next = pg->next;
        pg->next->prev = pg->prev;
    }
    pg->next = pg->prev = 0;
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
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
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

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
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

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    pte = walk(pagetable, a, 0);
    if(pte == 0)
      panic("uvmunmap: walk");

    // 1) 우선 PTE_V가 꺼져 있는 경우 (메모리에 없음)
    if((*pte & PTE_V) == 0){
      // 스왑된 페이지인 경우
      if(*pte & PTE_SWAPPED){
        if(do_free){
          // 이 PTE에는 (slot << 10) 이 들어있으므로
          // PTE2PA(*pte) = (slot << 12) => slot = PTE2PA(*pte) / PGSIZE;
          int slot = PTE2PA(*pte) / PGSIZE;
          free_swap_slot(slot);     // ✅ 스왑 슬롯만 반납
        }
        *pte = 0;                   // PTE 지우기
        continue;
      }

      // 스왑도 아니고 V도 안 켜져 있으면 진짜 이상한 상황
      panic("uvmunmap: not mapped");
    }

    // 2) 여기까지 왔으면 PTE_V == 1 → 메모리에 있는 leaf 페이지여야 함
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not leaf");

    if(do_free){
      uint64 pa = PTE2PA(*pte);

      // ✅ LRU에서 제거
      int idx = pa2pageidx(pa);
      struct page *pg = &pages[idx];
      acquire(&lru_lock);
      lru_remove(pg);
      release(&lru_lock);

      // 물리 페이지 free
      kfree((void*)pa);
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
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      // 지금까지 할당한 부분 되돌리기
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);

    if(mappages(pagetable, a, PGSIZE, (uint64)mem,
                PTE_R | PTE_U | xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }

    // ===== LRU 리스트에 등록 =====
    // kalloc이 리턴한 mem은 "해당 물리 페이지의 커널 주소"라고 보면 되고,
    // xv6에선 커널주소 == 물리주소라서 바로 /PGSIZE 해서 인덱스로 사용 가능.
    uint64 pa = (uint64)mem;
    int idx = pa2pageidx(pa);           // ✅ KERNBASE 보정

    struct page *pg = &pages[idx];
    pg->pagetable = pagetable;
    pg->vaddr     = (char*)a;

    acquire(&lru_lock);                 // ✅ 락 잡고
    lru_add(pg);
    release(&lru_lock);                 // ✅ 락 풀기

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

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");

    if(*pte & PTE_V){
      // 1) 메모리에 있는 페이지 복사
      pa    = PTE2PA(*pte);
      flags = PTE_FLAGS(*pte);

      if((mem = kalloc()) == 0)
        goto err;

      memmove(mem, (char*)pa, PGSIZE);

      if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
        kfree(mem);
        goto err;
      }

      // LRU 등록
      uint64 pa_child = (uint64)mem;
      int idx = pa2pageidx(pa_child);

      struct page *pg = &pages[idx];
      pg->pagetable = new;
      pg->vaddr     = (char*)i;
      acquire(&lru_lock);
      lru_add(pg);
      release(&lru_lock);

    } else if(*pte & PTE_SWAPPED){
      // 2) 부모가 swapout된 페이지인 경우

      // slot 번호 꺼내기: swapout 때 (slot<<10) 넣었으니까
      int slot = PTE2PA(*pte) / PGSIZE;

      // R/W/X/U 플래그만 남기고, PTE_SWAPPED 제거
      flags = PTE_FLAGS(*pte) & ~PTE_SWAPPED;

      // 자식용 물리 페이지
      if((mem = kalloc()) == 0)
        goto err;

      // swap 공간에서 자식 물리 페이지로 직접 읽어오기
      swapread((uint64)mem, slot);

      // 자식 PTE는 "메모리에 있는 정상 페이지"로 매핑
      if(mappages(new, i, PGSIZE, (uint64)mem, flags | PTE_V) != 0){
        kfree(mem);
        goto err;
      }

      // LRU 등록
      uint64 pa_child = (uint64)mem;
      int idx = pa2pageidx(pa_child);
      struct page *pg = &pages[idx];
      pg->pagetable = new;
      pg->vaddr     = (char*)i;
      acquire(&lru_lock);
      lru_add(pg);
      release(&lru_lock);

      // 부모 쪽은 그대로 스왑 상태 유지 (PTE는 건드리지 않음)

    } else {
      // V도 아니고 SWAPPED도 아니면 이 범위 안에서는 안 나와야 하는 케이스
      // (guard page 등을 쓰면 여기서 예외 처리해도 되는데, 과제 기준으로 panic이 안전)
      panic("uvmcopy: page not present");
    }
  }
  return 0;

err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}




// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
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
    if(va0 >= MAXVA)
      return -1;
    pte = walk(pagetable, va0, 0);
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0 ||
       (*pte & PTE_W) == 0)
      return -1;
    pa0 = PTE2PA(*pte);
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
swapinit(void)
{
    swap_bitmap = kalloc();
    if(swap_bitmap == 0) panic("swapinit");
    memset(swap_bitmap, 0, PGSIZE);
    const int BLKS_PER_PG = PGSIZE / BSIZE;
    swap_slots = SWAPMAX / BLKS_PER_PG;
    initlock(&swap_lock,"swap");
    initlock(&lru_lock, "lru");
}

int
swapout(void)
{
  // 1. LRU에서 victim 후보를 찾는다 (clock 알고리즘 비슷하게)
  acquire(&lru_lock);

  if(lru_head == 0){
    release(&lru_lock);
    return -1; // 스왑할 유저 페이지가 없다 → 진짜 OOM
  }

  struct page *cand = lru_head;

  while(1){
    pte_t *pte = walk(cand->pagetable, (uint64)cand->vaddr, 0);
    if(pte == 0)
      panic("swapout: no pte");

    if((*pte & PTE_V) == 0){
      // 이론상 LRU에는 present page만 있어야 하는데,
      // 혹시라도 이미 스왑된 게 섞였으면 그냥 다음으로 넘김
      cand = cand->next;
    } else if(*pte & PTE_A){
      // 최근에 접근된 페이지 → 한 번 봐주고 A 비트만 내림 (2nd chance)
      *pte &= ~PTE_A;

      // clock: head를 다음으로, cand를 tail로 보내는 효과
      lru_head = cand->next;
      lru_tail = cand;
      cand = lru_head;
    } else {
      // PTE_A == 0 이면 victim으로 사용
      break;
    }
  }

  struct page *victim = cand;

  // LRU 리스트에서 제거
  lru_remove(victim);
  release(&lru_lock);

  // 2. swap slot 하나 할당
  int slot = alloc_swap_slot();
  if(slot < 0){
    // 스왑 슬롯도 없다 → 다시 LRU에 되돌려놓고 실패
    acquire(&lru_lock);
    lru_add(victim);
    release(&lru_lock);
    return -1;
  }

  // 3. 물리주소 계산해서 디스크에 페이지 내용 쓰기
  uint64 pa = page_to_pa(victim);
  swapwrite(pa, slot);   // ptr = 커널 주소(pa), slot = swap slot 인덱스

  // 4. PTE 갱신: 메모리에서 빠지고 디스크에 있음 표시
  pte_t *pte = walk(victim->pagetable, (uint64)victim->vaddr, 0);
  if(pte == 0)
    panic("swapout: pte vanished");

  uint64 flags = PTE_FLAGS(*pte);

  // 이제 이 PTE는 "메모리에는 없음, 스왑에 있음" 상태로 만든다.
  flags &= ~PTE_V;  // 유효하지 않음 (page fault 발생시키기 위함)
  flags &= ~PTE_A;  // accessed 비트도 정리

  // PPN(물리주소 자리)에 slot 번호를 넣고, SWAPPED 플래그를 세운다.
  //   PTE2PA(*pte) = (PPN << 12) 이므로
  //   나중에 slot = PTE2PA(*pte) / PGSIZE 로 다시 꺼낼 수 있다.
  *pte = ((uint64)slot << 10) | flags | PTE_SWAPPED;

  // 5. 실제 물리 페이지 free
  kfree((void*)pa);

  // 6. page 메타데이터 정리(선택)
  victim->pagetable = 0;
  victim->vaddr = 0;
  // next/prev는 lru_remove에서 이미 0으로 만들어줌

  return 0;
}


int
swapin(pagetable_t pagetable, uint64 va)
{
  // 1. va를 페이지 경계로 내림 (페이지 시작 주소)
  va = PGROUNDDOWN(va);

  // 2. 해당 va의 PTE를 찾는다.
  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0)
    return -1;  // PTE 자체가 없으면 이상한 경우

  // 3. 정말로 "스왑된 페이지"인지 확인
  if((*pte & PTE_SWAPPED) == 0 || (*pte & PTE_V) != 0)
    return -1;  // swapin 대상이 아님

  // 4. PTE에 저장해 둔 slot 번호 꺼내기
  //    swapout에서  *pte = (slot<<10) | flags | PTE_SWAPPED;  했으므로
  //    PTE2PA(*pte) = (slot << 12) => slot = PTE2PA(*pte) / PGSIZE;
  int slot = PTE2PA(*pte) / PGSIZE;

  // 5. 새 물리 페이지 한 장 할당
  uint64 pa = (uint64)kalloc();
  if(pa == 0){
    // kalloc 안에서 swapout을 이미 시도했을 것이고,
    // 그래도 못 얻은 상황이면 실패로 처리
    return -1;
  }

  // 6. 디스크의 해당 slot에서 페이지 내용을 읽어와서 pa에 채운다.
  swapread(pa, slot);

  // 7. swap slot 반납 (이제 이 slot은 다시 재사용 가능)
  free_swap_slot(slot);

  // 8. PTE 갱신:
  //    - PPN 자리에 새 물리주소(pa)를 넣고
  //    - PTE_SWAPPED는 내리고
  //    - PTE_V는 세운다.
  uint64 flags = PTE_FLAGS(*pte);

  flags &= ~PTE_SWAPPED;  // 더 이상 스왑 상태 아님

  // 필요하다면 A비트는 0으로 둬도 되고, 그냥 flags에 포함된 그대로 놔둬도 됨.
  // 여기서는 그냥 그대로 사용.

  *pte = PA2PTE(pa) | flags | PTE_V;

  // 9. LRU 리스트에 다시 등록
  //    pa -> 몇 번째 페이지인지 계산해서 pages[]에서 struct page*를 찾는다.
  int idx = pa2pageidx(pa);
  struct page *pg = &pages[idx];

  pg->pagetable = pagetable;
  pg->vaddr     = (char*)va;

  acquire(&lru_lock);
  lru_add(pg);
  release(&lru_lock);

  return 0;
}

