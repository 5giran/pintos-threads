#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file; // 이 page가 읽을 backing file
	off_t ofs; // 이 page가 파일의 어느 offset과 연결되는지
	uint32_t read_bytes; // page fault 때 파일에서 몇 byte 읽을지
	uint32_t zero_bytes; // 나머지 몇 byte를 0으로 채울지
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
