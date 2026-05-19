#ifndef VM_VM_H
#define VM_VM_H
#include <stdbool.h>
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "threads/palloc.h"

enum vm_type {
	/* 초기화되지 않은 page */
	VM_UNINIT = 0,
	/* 파일과 관련 없는 page, 즉 anonymous page */
	VM_ANON = 1,
	/* 파일과 관련된 page */
	VM_FILE = 2,
	/* page cache를 담는 page, project 4용 */
	VM_PAGE_CACHE = 3,

	/* 상태를 저장하는 bit flag */

	/* 정보를 저장하기 위한 보조 bit flag marker.
	 * 값이 int에 들어가는 한 더 추가해도 된다. */
	VM_MARKER_0 = (1 << 3),
	VM_MARKER_1 = (1 << 4),

	/* 이 값을 넘기지 말 것. */
	VM_MARKER_END = (1 << 31),
};

#include "vm/uninit.h"
#include "vm/anon.h"
#include "vm/file.h"
#ifdef EFILESYS
#include "filesys/page_cache.h"
#endif

struct page_operations;
struct thread;

#define VM_TYPE(type) ((type) & 7)

/* "page"의 표현.
 * 이는 일종의 "parent class"이며,
 * uninit_page, file_page, anon_page, page cache(project4)라는
 * 네 개의 "child class"를 가진다.
 * 이 구조체의 미리 정의된 멤버는 제거하거나 수정하지 말 것. */
struct page {
	const struct page_operations *operations;
	void *va;              /* user space 기준 주소 */
	struct frame *frame;   /* frame을 가리키는 역참조 */
	bool writable;

	struct hash_elem hash_elem;

	/* 타입별 데이터는 union에 묶여 있다.
	 * 각 함수는 현재 union을 자동으로 감지한다. */
	union {
		struct uninit_page uninit;
		struct anon_page anon;
		struct file_page file;
#ifdef EFILESYS
		struct page_cache page_cache;
#endif
	};
};

/* "frame"의 표현 */
struct frame {
	void *kva;
	struct page *page;
};

/* page 연산을 위한 함수 테이블.
 * 이는 C에서 "interface"를 구현하는 한 가지 방법이다.
 * "method" 테이블을 struct의 멤버에 넣어 두고,
 * 필요할 때마다 호출하면 된다. */
struct page_operations {
	bool (*swap_in) (struct page *, void *);
	bool (*swap_out) (struct page *);
	void (*destroy) (struct page *);
	enum vm_type type;
};

#define swap_in(page, v) (page)->operations->swap_in ((page), v)
#define swap_out(page) (page)->operations->swap_out (page)
#define destroy(page) \
	if ((page)->operations->destroy) (page)->operations->destroy (page)


struct mmap_record {
    void *addr;
    uint32_t page_count;
    struct list_elem elem; // mmap_record 구조체가 만들어지면 이것끼리 연결됨
	// list는 elem이 원소잖아!!!
};


/* 현재 process의 메모리 공간 표현.
 * 이 struct의 설계를 특정 방식으로 강제하고 싶지는 않다.
 * 설계는 전부 여러분에게 달려 있다. */

 // [추가] mmap list head 정보 여기에 추가
 // 이 프로세스의 가상 메모리 상태는 thread_current()->spt에 둔다. 로 통일
struct supplemental_page_table {
	struct hash table;
	// [수정] mmap 페이지 하나가 아니라 여러개의 페이지를 저장해야하기 때문에 list가 필요하다.
	struct list mmap_list;
	
};

#include "threads/thread.h"
void supplemental_page_table_init (struct supplemental_page_table *spt);
bool supplemental_page_table_copy (struct supplemental_page_table *dst,
		struct supplemental_page_table *src);
void supplemental_page_table_kill (struct supplemental_page_table *spt);
struct page *spt_find_page (struct supplemental_page_table *spt,
		void *va);
bool spt_insert_page (struct supplemental_page_table *spt, struct page *page);
void spt_remove_page (struct supplemental_page_table *spt, struct page *page);

void vm_init (void);
bool vm_try_handle_fault (struct intr_frame *f, void *addr, bool user,
		bool write, bool not_present);

#define vm_alloc_page(type, upage, writable) \
	vm_alloc_page_with_initializer ((type), (upage), (writable), NULL, NULL)
bool vm_alloc_page_with_initializer (enum vm_type type, void *upage,
		bool writable, vm_initializer *init, void *aux);
void vm_dealloc_page (struct page *page);
bool vm_claim_page (void *va);
enum vm_type page_get_type (struct page *page);

bool is_valid_stack_growth_request (bool user, struct intr_frame* f, void* addr, struct page* page);


#endif  /* VM_VM_H */
