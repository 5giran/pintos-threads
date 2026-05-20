/* anon.c: 디스크 이미지가 아닌 페이지(a.k.a. anonymous page)의 구현. */

#include "vm/vm.h"
#include "devices/disk.h"
#include "kernel/bitmap.h"

/* 아래 줄을 수정하지 마십시오 */
static struct disk *swap_disk;
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

/* 이 struct를 수정하지 마십시오 */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};


static struct lock swap_lock;               /* 상호 배제. */
static struct bitmap *swap_table;        /* free swap slot 나타내는 bitmap. */

/* anonymous page용 데이터를 초기화한다 */
void
vm_anon_init (void) {
	/* TODO: swap_disk를 설정한다. */
	// disk_init ();
	swap_disk = disk_get (1, 1);
	lock_init (&swap_lock);

	swap_table = bitmap_create (disk_size (swap_disk) / 8);
}

/* 파일 매핑을 초기화한다 */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* handler를 설정한다 */
	page->operations = &anon_ops;

	struct anon_page *anon_page = &page->anon;
	return true;
}

/* swap disk에서 내용을 읽어 페이지를 swap in 한다. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *anon_page = &page->anon;

	int swap_slot_index = anon_page->swap_slot_index;

	disk_sector_t start_sector_no = swap_slot_index * 8;
	void * buffer = kva;
	for (int i = 0; i <= 7; i++) {
		disk_read (swap_disk, start_sector_no, buffer);
		start_sector_no += DISK_SECTOR_SIZE;
		buffer += DISK_SECTOR_SIZE;
	}
	bitmap_flip (swap_table, swap_slot_index);

	page->anon.swap_slot_index = -1;

	return true;
}

/* 내용을 swap disk에 써서 페이지를 swap out 한다. */
static bool
anon_swap_out (struct page *page) {
	struct anon_page *anon_page = &page->anon;
	// TODO. if 이미 swap out 되어 있다면 넘어가기
	lock_acquire (&swap_lock);
	int bit_index = bitmap_scan_and_flip (swap_table, 0, 1, 0);
	lock_release (&swap_lock);

	anon_page->swap_slot_index = bit_index;

	disk_sector_t start_sector_no = bit_index * 8;
	void * buffer = page->frame->kva;
	for (int i = 0; i <= 7; i++) {
		disk_write (swap_disk, start_sector_no, buffer);
		start_sector_no += DISK_SECTOR_SIZE;
		buffer += DISK_SECTOR_SIZE;
	}
	pml4_clear_page (thread_current ()->pml4, page->va);
	page->frame = NULL;
	
	return true;
}

/* anonymous page를 파괴한다. PAGE는 호출자가 해제한다. */
static void
anon_destroy (struct page *page) {
	struct anon_page *anon_page = &page->anon;
}
