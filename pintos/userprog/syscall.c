#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/init.h"
#include "threads/palloc.h"
#include "userprog/fd.h"
#include "userprog/process.h"
#include "intrinsic.h"
#include "vm/vm.h"
#include "debug_log.h"


void syscall_entry (void);
void syscall_handler (struct intr_frame *);
struct lock filesys_lock;

static bool sys_create (const char *ufile, unsigned initial_size);
static bool sys_remove (const char *ufile);
static int sys_open (const char *ufile);
static int sys_filesize (int fd);
static int sys_read (int fd, void *ubuf, unsigned size);
static int sys_write (int fd, const void *ubuf, unsigned size);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell (int fd);
static void sys_close (int fd);

enum user_access {
	USER_ACCESS_READ,
	USER_ACCESS_WRITE,
};

/* 시스템 콜.
 *
 * 이전에는 system call 서비스가 interrupt handler(예: linux의 int 0x80)에 의해 처리되었다. 그러나
 * x86-64에서는 제조사가 system call 요청을 위한 효율적인 경로인 `syscall` instruction을 제공한다.
 *
 * syscall instruction은 Model Specific Register (MSR)의 값을 읽는 방식으로 동작한다. 자세한
 * 내용은 manual을 참고하라. */

#define MSR_STAR 0xc0000081         /* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL 대상 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags용 마스크 */

void
syscall_init (void) 
{
	lock_init (&filesys_lock);
	/* syscall 진입 시 사용할 코드 세그먼트 정보 설정 */
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
			
	/* syscall 명령이 들어오면 어디로 점프할지 설정 */
	write_msr (MSR_LSTAR, (uint64_t) syscall_entry);
	
	/* syscall_entry가 userland stack을 kernel mode stack으로 바꿀 때까지 interrupt
	 * service routine은 어떤 interrupt도 처리해서는 안 된다. 따라서 FLAG_FL을 마스킹했다.
	 * syscall 진입 중 마스킹할 플래그 설정 */
	write_msr (MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/* 주요 system call interface */
/* 인터럽트나 시스템 콜이 발생했을 때 cpu 레지스터 상태를 저장한 구조체가 intr_frame */
/* 시스템콜 인자들이 syscall_handler에 일반 함수 인자처럼 들어오는 게 아니라, 
 * 인터럽트 프레임 f 안에 저장된 레지스터 값으로 들어온다. */
void
syscall_handler (struct intr_frame *f) 
{
	// TODO: 구현을 여기에 작성하라.
	/* rax에 syscall 번호가 들어 있다. */
	thread_current ()->rsp = f->rsp;
	switch (f->R.rax) {

		/* 시스템 종료 */
		case SYS_HALT:
			power_off ();
			break;

		/* 현재 프로세스 종료 */
		case SYS_EXIT:
			/* thread_exit() 내부에서 process_exit() 호출해 현재 스레드 종료 */
			thread_current ()->exit_status = (int) f->R.rdi;
			thread_exit ();
			break;

		case SYS_WAIT:
			f->R.rax = process_wait ((tid_t) f->R.rdi);
			break;

		case SYS_CREATE:
			f->R.rax = sys_create ((const char*) f->R.rdi, 
								   (unsigned) f->R.rsi);
			break;

		case SYS_REMOVE:
			f->R.rax = sys_remove ((const char*) f->R.rdi);
			break;

		case SYS_OPEN:
			f->R.rax = sys_open ((const char*) f->R.rdi);
			break;

		case SYS_FILESIZE:
			f->R.rax = sys_filesize ((int) f->R.rdi);
			break;

		case SYS_READ:
			f->R.rax = sys_read ((int) f->R.rdi, 
								 (void *) f->R.rsi,
								 (unsigned) f->R.rdx);
			break;

		case SYS_WRITE:
			f->R.rax = sys_write ((int) f->R.rdi, 
								 (const void *) f->R.rsi,
								 (unsigned) f->R.rdx);
			break;

		case SYS_SEEK:
			sys_seek ((int) f->R.rdi,
					  (unsigned) f->R.rsi);
			break;
			
		case SYS_TELL:
			f->R.rax = sys_tell ((int) f->R.rdi);
			break;

		case SYS_CLOSE:
			sys_close ((int) f->R.rdi);
			break;
		
		/* exec (const char *cmd_line) */
		case SYS_EXEC: {
			
			/* 첫 번째 인자 file은 rdi */
			char *user_file = (char *) f->R.rdi; // exec("echo hello") 라는 system call을 user process가 호출했다면, f->R.rdi 는 "echo hello" 문자열이 있는 user memory 주소

			/* cmd_line 문자열을 kernel page에 복사, 내부적으로 주소 검증도 진행.. 하는지는 확인 필요 */
			char *kernel_page = copy_in_string (user_file);

			/* 복사 성공하면 */
			if (process_exec (kernel_page) == -1) {
				thread_exit ();
			}
			break;
			
		}

		case SYS_FORK: {
			char *thread_name = copy_in_string ((const char *) f->R.rdi);
			if (thread_name == NULL) {
				f->R.rax = TID_ERROR;
				break;
			}

			f->R.rax = process_fork (thread_name, f);
			palloc_free_page (thread_name);
			break;
		}

		
		case SYS_MMAP:
			void *addr = (void*) f->R.rdi;
			size_t length = (size_t) f->R.rsi;
			int writable = (int) f->R.rdx;
			int fd = (int) f->R.r10;
			struct file* file = fd_get (fd);
			off_t offset = (off_t) f->R.r8;

			f->R.rax = do_mmap (addr, length, writable, file, offset);
			break;
		
		case SYS_MUNMAP:
			do_munmap ((void*) f->R.rdi);
			break;

		/* 아직 구현 안 한 syscall은 비정상 종료 */
		default:
			thread_exit ();
			break;
		
	}
	thread_current ()->rsp = NULL;
}

/* Project 2는 pml4에 이미 매핑된 page만 user buffer로 인정한다.
 * Project 3에서 lazy page가 들어오면 이 helper의 "page present" 판정을
 * spt_find_page()/vm_try_handle_fault 흐름과 연결하면 된다. */
static void
validate_user_buffer (const void *buffer, size_t size, enum user_access access)
{
	if (size == 0)
		return;
	if (buffer == NULL)
		thread_exit ();

	// 반복문 돌리기 위해서 주소를 1바이트씩 이동시킬 수 있게 변경: buffer의 시작 바이트 주소
	const uint8_t *bf = (const uint8_t *) buffer;
	// end address 따로 정의: buffer의 끝 바이트 주소
	const uint8_t *end_adr = bf + size - 1;

	// 끝 주소 = 시작 주소보다 같거나 커야 한다, 근데 시작 주소보다 작다 (초과분이 잘린거임)
	if (end_adr < bf) {
		DBG ("validate_user_buffer: end_adr < bf\n");
		thread_exit ();
	}
		

	// 순회할 시작페이지, 끝 페이지 정의
	const uint8_t *start_page = pg_round_down (bf);
	const uint8_t *end_page = pg_round_down (end_adr);

	/* buffer가 걸쳐진 page를 순회 (페이지 단위로 확인)
	 * buffer의 시작주소 + PGSIZE: buffer의 끝 주소까지 순회
	 * i는 buffer가 걸친 각 페이지의 시작 주소를 가리킴 */
	for (const uint8_t *i = start_page; i <= end_page; i += PGSIZE) {
		// page 시작주소가 user virtual address 범위 안에 있지 않다면 현재 프로세스 exit(-1)
		if (!is_user_vaddr (i)) {
			DBG ("validate_user_buffer: !is_user_vaddr\n");
			thread_exit ();
		}
			

		/* 현재 프로세스의 페이지 테이블에서 유저 가상주소 i에 해당하는 PTE를 찾는다.
			create=false이므로, 없으면 새로 만들지 않고 NULL을 반환한다. */
		uint64_t *pte = pml4e_walk (thread_current ()->pml4,
				(const uint64_t) i, true);
		
		if (pte == NULL || (((*pte & PTE_P) == 1) && (*pte & PTE_U) == 0)) {
			DBG ("[debug] validate_user_buffer: invalid user PTE "
					"addr=%p pte=%p pte_val=%llx access=%d\n",
					(const void *) i, (void *) pte,
					pte != NULL ? (unsigned long long) *pte : 123, access);
			thread_exit ();
		}

		// PTE_P == 0 분기 분리
		struct page *page;
		if ((*pte & PTE_P) == 0) {
			page = spt_find_page (&thread_current ()->spt, i);
			if (page != NULL) {
				vm_claim_page (i);
			}
			else if (is_valid_stack_growth_request (false, NULL, i, page)) {
				vm_alloc_page (VM_ANON, i, true);
				vm_claim_page (i);
			} else {
				DBG ("validate_user_buffer: no spt entry\n");
				DBG ("validate_user_buffer: page:%p, pte:%p, *pte:%lld\n", page, pte, *pte);

				thread_exit ();
			}
		}

		if (access == USER_ACCESS_WRITE && (*pte & PTE_P) == 1 && (*pte & PTE_W) == 0) {
			DBG ("[debug] validate_user_buffer: write access denied "
					"buffer=%p size=%zu page=%p access=%d pte=%p pte_val=%llx\n",
					buffer, size, (const void *) i, access, (void *) pte,
					(unsigned long long) *pte);
			thread_exit ();
		}
	}
}

void
validate_user_read (const void *buffer, size_t size)
{
	validate_user_buffer (buffer, size, USER_ACCESS_READ);
}

void
validate_user_write (const void *buffer, size_t size)
{
	validate_user_buffer (buffer, size, USER_ACCESS_WRITE);
}

void
// dst: 복사 결과가 들어갈 커널 버퍼, usrc: 복사 원본 유저 메모리 주소, size: 복사할 바이트 수
copy_in (void *dst, const void *usrc, size_t size)
{
	// readable 유효성 검사
	validate_user_read (usrc, size);
	// user memory -> kernel memory 복사
	memcpy (dst, usrc, size);
}

// udst: 유저 메모리 목적지, src: 커널 복사 원본
void
copy_out (void *udst, const void *src, size_t size)
{
	// writable 유효성 검사
	validate_user_write (udst, size);
	// kernel memory -> user memory 복사
	memcpy (udst, src, size);
}

/* TODO: user string을 kernel buffer로 복사 */
// ustr: 유저가 넘긴 문자열 시작 주소, return: 커널 메모리에 새로 복사해 둔 문자열 주소
char *
copy_in_string (const char *ustr)
{
	if (ustr == NULL) thread_exit ();
	// NULL 아니면 1바이트씩 추적 가능하게 타입 캐스팅
	const uint8_t *us = (const uint8_t *) ustr;
	
	// 검증과 kbuf 할당 분리, 현재는 검증단계
	size_t i = 0;
	// kbuf는 한 페이지짜리 커널 버퍼이므로, PGSIZE 바이트까지만 복사한다.
	while (i < PGSIZE) {
		validate_user_read (us+i, 1);
		if (us[i] == '\0') {
			// 정상 종료, 할당 로직으로 넘어가야함.
			break;
		} else {
			i++;
		}
	}
	/* 여기까지 왔다는 건 0..PGSIZE-1 범위에서 '\0'을 못 찾았다는 뜻이다.
   ustr[PGSIZE]는 허용 범위 밖이므로 읽지 않고 실패 처리한다. */
	if (i == PGSIZE) thread_exit ();


	// 커널 버퍼 할당, 0: flag, 옵션 없음
	char *kbuf = palloc_get_page (0);
	if (kbuf == NULL) thread_exit ();

	// size는 '\0'도 같이 읽기 위해서
	copy_in (kbuf, us, i+1);
	return kbuf;
}

static bool 
sys_create (const char *ufile, unsigned initial_size)
{
	char *kfile = copy_in_string (ufile);
	bool ok;

	lock_acquire (&filesys_lock);
	ok = filesys_create (kfile, initial_size);
	lock_release (&filesys_lock);

	palloc_free_page (kfile);
	return ok;
}

static bool 
sys_remove (const char *ufile)
{
	char *kfile = copy_in_string (ufile);
	bool ok;

	lock_acquire (&filesys_lock);
	ok = filesys_remove (kfile);
	lock_release (&filesys_lock);

	palloc_free_page (kfile);
	return ok;
}


static int 
sys_open (const char *ufile)
{
	char *kfile = copy_in_string (ufile);
	struct file *file;
	int fd;

	lock_acquire (&filesys_lock);
	file = filesys_open (kfile);
	lock_release (&filesys_lock);

	palloc_free_page (kfile);

	if (file == NULL) {
		return -1;
	}

	fd = fd_alloc (file);
	if (fd == -1) {
		lock_acquire (&filesys_lock);
		file_close (file);
		lock_release (&filesys_lock);
	}
	return fd;
}

static int 
sys_filesize (int fd)
{
	struct file *file = fd_get (fd);
	int len;

	if (file == NULL) {
		return -1;
	}

	lock_acquire (&filesys_lock);
	len = (int) file_length (file);
	lock_release (&filesys_lock);

	return len;
}

static int 
sys_read (int fd, void *ubuf, unsigned size)
{
uint8_t *kbuf;
	unsigned total = 0;

	if (size == 0)
		return 0;

	validate_user_write (ubuf, size);

	if (fd == 1)
		return -1;

	kbuf = palloc_get_page (0);
	if (kbuf == NULL)
		return -1;

	while (total < size) {
		unsigned chunk = size - total > PGSIZE ? PGSIZE : size - total;
		int bytes = 0;

		if (fd == 0) {
			for (unsigned i = 0; i < chunk; i++)
				kbuf[i] = input_getc ();
			bytes = (int) chunk;
		} else {
			struct file *file = fd_get (fd);
			if (file == NULL) {
				palloc_free_page (kbuf);
				return -1;
			}

			lock_acquire (&filesys_lock);
			bytes = (int) file_read (file, kbuf, chunk);
			lock_release (&filesys_lock);
		}

		if (bytes <= 0)
			break;

		copy_out ((uint8_t *) ubuf + total, kbuf, (size_t) bytes);
		total += (unsigned) bytes;

		if ((unsigned) bytes < chunk)
			break;
	}

	palloc_free_page (kbuf);
	return (int) total;
}

static int 
sys_write (int fd, const void *ubuf, unsigned size)
{
	uint8_t *kbuf;
	unsigned total = 0;

	if (size == 0) {
		return 0;
	}

	validate_user_read (ubuf, size);

	if (fd == 0) {
		return -1;
	}

	kbuf = palloc_get_page (0);
	if (kbuf == NULL) {
		return -1;
	}

	while (total < size) {
		unsigned chunk = size - total > PGSIZE ? PGSIZE : size - total;
		int written;
	
		copy_in (kbuf, (const uint8_t *) ubuf + total, chunk);

		if (fd == 1) {
			putbuf ((char *) kbuf, chunk);
			total += chunk;
			continue;
		}

		{
			struct file *file = fd_get (fd);
			if (file == NULL) {
				palloc_free_page (kbuf);
				return -1;
			}

			lock_acquire (&filesys_lock);
			written = (int) file_write (file, kbuf, chunk);
			lock_release (&filesys_lock);
		}

		if (written <= 0) {
			break;
		}

		total += written;
		if ((unsigned) written < chunk) {
			break;
		}
	}
	palloc_free_page (kbuf);
	return (int) total;
}

static void 
sys_seek (int fd, unsigned position)
{
	struct file *file = fd_get (fd);

	if (file == NULL) {
		return;
	}

	lock_acquire (&filesys_lock);
	file_seek (file, (off_t) position);
	lock_release (&filesys_lock);
}

static unsigned 
sys_tell (int fd)
{
	struct file *file = fd_get (fd);
	unsigned pos;

	if (file == NULL) {
		return (unsigned) -1;
	}

	lock_acquire (&filesys_lock);
	pos = (unsigned) file_tell (file);
	lock_release (&filesys_lock);

	return pos;
}

static void 
sys_close (int fd)
{
	fd_close (fd);
}
