/* file.c: memory backed file object(mmaped object)의 구현. */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

/* 이 struct를 수정하지 마십시오 */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* file vm의 초기화자 */
void
vm_file_init (void) {
}

/* file-backed page를 초기화한다 */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* handler를 설정한다 */
	page->operations = &file_ops;

	struct file_page *file_page = &page->file;
}

/* 파일에서 내용을 읽어 페이지를 swap in 한다. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
	struct file_page *file_page UNUSED = &page->file;
}

/* 내용을 파일로 writeback하여 페이지를 swap out 한다. */
static bool
file_backed_swap_out (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

/* file-backed page를 파괴한다. PAGE는 호출자가 해제한다. */
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}

struct mmap_aux {
	struct file *file; // 이 page가 읽을 backing file
	off_t ofs; // 이 page가 파일의 어느 offset과 연결되는지
	uint32_t read_bytes; // page fault 때 파일에서 몇 byte 읽을지
	uint32_t zero_bytes; // 나머지 몇 byte를 0으로 채울지
	// writable?
};

static bool
mmap_load_segment (struct page *page, void *aux) {
	struct mmap_aux *a = aux;

	page->file.file = a->file;
	page->file.ofs = a->ofs;
	page->file.read_bytes = a->read_bytes;
	page->file.zero_bytes = a->zero_bytes;

	void* kpage = page->frame->kva;
	free(aux);

	if (!read_file_exact_at (page->file.file, kpage, page->file.read_bytes, page->file.ofs)) {
		return false;
	}

	memset (kpage + page->file.read_bytes, 0, page->file.zero_bytes);
	return true;
}


/* mmap을 수행한다 */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) 
{
	struct file *reopen_file = file_reopen(file);

	void *start_addr = addr; // 성공시 반환할 원본 addr
	uint8_t *upage = addr; // 반복문 내에서 커서로 쓰일 addr
	size_t remaining_map = length; // 매핑해야할 남은 byte 수
	size_t file_len = file_length(file); // 매핑될 파일 길이
	off_t file_ofs = offset; // page가 연결될 offset
	size_t remaining_file; // offset부터 파일에서 읽을 byte 수

	if (file_len <= offset) { // remaining_file 길이 할당
		remaining_file = 0;
	} else {
		remaining_file = file_len - offset;
	}

	// 반복 1번마다 가상 페이지 1개씩 등록됨
	while (remaining_map > 0)
	{
		// 사용자가 요청한 virtual memory mapping 길이 - length를 이 파일에서 얼마나 담당하는가
		// 페이지 길이보다 크면 페이지 크기만큼만 읽어오고 작으면 remaining_map만큼 읽어옴
		// 요청 내용을 읽어오는거랑 크기 처리 하는건 또 다른 문제임.
		size_t page_map_bytes = remaining_map < PGSIZE ? remaining_map : PGSIZE;
		// length를 실제로 얼마나 읽을 것인가. 
		size_t page_read_bytes = min(page_map_bytes, remaining_file);
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		struct mmap_aux *aux = malloc (sizeof (struct mmap_aux));

		if (aux == NULL) {
			return NULL;
		}

		aux->file = reopen_file;
		aux->ofs = file_ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;

		if (!vm_alloc_page_with_initializer (VM_FILE, upage,
											writable, mmap_load_segment, aux)) 
		{
			free (aux);
			return NULL;
		}

		upage += PGSIZE;
		file_ofs += PGSIZE;
		remaining_map -= page_map_bytes;
		remaining_file -= page_read_bytes;
	}
	return start_addr;
	
}

/* munmap을 수행한다 */
void
do_munmap (void *addr) {
}
