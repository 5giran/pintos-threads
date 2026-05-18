/* file.c: memory backed file object(mmaped object)의 구현. */

#include "vm/vm.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "debug_log.h"
#include "userprog/syscall.h"

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

/* file vm의 초기화자 
	파일 기반 페이지(file-backed page) 서브시스템을 초기화합니다. 
	이 함수에서 파일 기반 페이지와 관련된 모든 것을 설정할 수 있습니다.
*/
void
vm_file_init (void) {
}

/* file-backed page를 초기화한다 
	파일 기반 페이지(file-backed page)를 초기화합니다. 
	이 함수는 먼저 page->operations에서 파일 기반 페이지의 핸들러(handler)를 설정합니다. 
	메모리를 뒷받침하는 파일 같은 일부 정보를 page 구조체에 업데이트할 수 있습니다.
*/
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

struct lazy_load_file_aux {
	struct file *file; // 어느 실행 파일을 읽을지
	off_t ofs; // 실행 파일의 어느 위치부터(숫자값) 읽을지
	uint32_t read_bytes; // 이 page file에서 몇바이트 읽을지
	uint32_t zero_bytes; // 채워지지 않은 만큼 0으로 채워놓음
};

/* file-backed page를 파괴한다. PAGE는 호출자가 해제한다. 
	연관된 파일을 닫아 파일 기반 페이지(file-backed page)를 파괴합니다. 
	내용이 더티(dirty)이면 변경 사항을 파일에 다시 기록해야 합니다. 
	이 함수에서 page 구조체를 해제할 필요는 없습니다. 
	file_backed_destroy의 호출자가 이를 처리해야 합니다.
*/
static void
file_backed_destroy (struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
}


static bool
lazy_load_file (struct page *page, void *aux)
{
	if (aux == NULL) {
		return false;
	}
	struct file *file = ((struct lazy_load_file_aux *) aux)->file;
	off_t ofs = ((struct lazy_load_file_aux *) aux)->ofs;
	uint32_t read_bytes = ((struct lazy_load_file_aux *) aux)->read_bytes;
	uint32_t zero_bytes = ((struct lazy_load_file_aux *) aux)->zero_bytes;
	// free (aux);

	void* kpage = page->frame->kva;

	lock_acquire (&filesys_lock);
	if (read_bytes > 0 && !file_read_at (file, kpage, read_bytes, ofs)) {
		lock_release (&filesys_lock);
		DBG ("lazy_load_file: file read at 실패... \n");
		return false;
	}
	lock_release (&filesys_lock);
	memset (kpage + read_bytes, 0, zero_bytes);

	return true;
}

/* mmap을 수행한다 */
void *
do_mmap (void *addr, size_t length, int writable,
		struct file *file, off_t offset) {
	// TODO. validate request ... 

	// load_segment 하드 코딩 복사
	off_t ofs = offset;
	uint8_t *upage = addr;
	uint32_t read_bytes = 0;
	uint32_t zero_bytes = 0;
	// read_bytes, zero_bytes 계산
	// read_bytes = : 읽어야 하는 총 bytes 수 
	lock_acquire (&filesys_lock);
	read_bytes = file_length (file);
	lock_release (&filesys_lock);

	if (read_bytes > length) { // filesize 가 length보다 더 큰 경우
		read_bytes = length;
	}
	zero_bytes += length - read_bytes;
	zero_bytes += PGSIZE - (length % PGSIZE);
	
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs(upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0)
	{
		/* 이 페이지를 어떻게 채울지 계산하세요.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트는 0으로 채웁니다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Project 3 구현 시 struct lazy_load_segment_aux를 page마다 만들어
		 * file, offset, read/zero byte 수를 넘긴다. 이렇게 하면 lazy load와
		 * mmap 모두 file position 공유 없이 file_read_at() 기반으로 이어갈 수 있다. */
		struct lazy_load_file_aux *aux = malloc (sizeof (struct lazy_load_file_aux));

		if (aux == NULL) {
			return NULL;
		}
		

		struct file *new_file = file_reopen (file); // TODO. 각 page 마다 file 구조체를 새롭게 만들지 말고, 첫 번째 page 에만 만드는 식으로 최적화를 할 수 있을 것 같아요...
		if (new_file == NULL) {
			free (aux); // TODO. 이런 예외 상황에서 기존에 만들어두었던 다른 메모리 자원들도 회수해야 하지 않나?
		}
		aux->file = new_file;
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		

		if (!vm_alloc_page_with_initializer (VM_FILE, upage,
											writable, lazy_load_file, aux))
		{
			free (aux);
			return NULL;
		}
			

		/* 다음으로 진행합니다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return addr;
}

/* munmap을 수행한다 */
void
do_munmap (void *addr) {

}
