#include "threads/init.h"
#include <console.h>
#include <debug.h>
#include <limits.h>
#include <random.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "devices/kbd.h"
#include "devices/input.h"
#include "devices/serial.h"
#include "devices/timer.h"
#include "devices/vga.h"
#include "threads/interrupt.h"
#include "threads/io.h"
#include "threads/loader.h"
#include "threads/malloc.h"
#include "threads/mmu.h"
#include "threads/palloc.h"
#include "threads/pte.h"
#include "threads/thread.h"
#ifdef USERPROG
#include "userprog/process.h"
#include "userprog/exception.h"
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "userprog/tss.h"
#endif
#include "tests/threads/tests.h"
#ifdef VM
#include "vm/vm.h"
#endif
#ifdef FILESYS
#include "filesys/filesys.h"
#include "filesys/fsutil.h"
#endif

/* kernel 매핑만 가진 Page-map-level-4. */
uint64_t *base_pml4;

#ifdef FILESYS
/* -f: file system을 포맷할 것인가? */
static bool format_filesys;
#endif

/* -q: kernel 작업이 끝난 뒤 전원을 끌 것인가? */
bool power_off_when_done;

bool thread_tests;

static void bss_init (void);
static void paging_init (uint64_t mem_end);

static char **read_command_line (void);
static char **parse_options (char **argv);
static void run_actions (char **argv);
static void usage (void);

static void print_stats (void);


int main (void) NO_RETURN;

/* Pintos 메인 프로그램. */
int
main (void) {
	uint64_t mem_end;
	char **argv;

	/* BSS를 지우고 machine의 RAM 크기를 얻는다. */
	bss_init ();

	/* command line을 인자들로 나누고 option을 파싱한다. */
	argv = read_command_line ();
	argv = parse_options (argv);

	/* lock을 사용할 수 있도록 현재 실행 흐름을 thread로 초기화한 뒤,
	   console locking을 활성화한다. */
	thread_init ();
	console_init ();

	/* 메모리 시스템을 초기화한다. */
	mem_end = palloc_init ();
	malloc_init ();
	paging_init (mem_end);

#ifdef USERPROG
	tss_init ();
	gdt_init ();
#endif

	/* interrupt handler를 초기화한다. */
	intr_init ();
	timer_init ();
	kbd_init ();
	input_init ();
#ifdef USERPROG
	exception_init ();
	syscall_init ();
#endif
	/* thread scheduler를 시작하고 interrupt를 활성화한다. */
	thread_start ();
	serial_init_queue ();
	timer_calibrate ();

#ifdef FILESYS
	/* file system을 초기화한다. */
	disk_init ();
	filesys_init (format_filesys);
#endif

#ifdef VM
	vm_init ();
	frame_table_init ();
#endif

	printf ("Boot complete.\n");

	/* kernel command line에 지정된 action을 실행한다. */
	run_actions (argv);

	/* 마무리한다. */
	if (power_off_when_done)
		power_off ();
	thread_exit ();
}

/* BSS를 지운다. */
static void
bss_init (void) {
	/* "BSS"는 0으로 초기화되어야 하는 segment다.
	   실제로는 disk에 저장되지도 않고 kernel loader가 0으로 채워 주지도 않으므로,
	   우리가 직접 0으로 초기화해야 한다.
	   BSS segment의 시작과 끝은 linker가 _start_bss와 _end_bss로 기록한다.
	   kernel.lds를 참고하라. */
	extern char _start_bss, _end_bss;
	memset (&_start_bss, 0, &_end_bss - &_start_bss);
}

/* page table을 kernel virtual mapping으로 채운 뒤,
 * CPU가 새 page directory를 사용하도록 설정한다.
 * 자신이 만든 pml4를 base_pml4가 가리키게 한다. */
static void
paging_init (uint64_t mem_end) {
	uint64_t *pml4, *pte;
	int perm;
	pml4 = base_pml4 = palloc_get_page (PAL_ASSERT | PAL_ZERO);

	extern char start, _end_kernel_text;
	// physical address [0 ~ mem_end]를
	//   [LOADER_KERN_BASE ~ LOADER_KERN_BASE + mem_end]에 매핑한다.
	for (uint64_t pa = 0; pa < mem_end; pa += PGSIZE) {
		uint64_t va = (uint64_t) ptov(pa);

		perm = PTE_P | PTE_W;
		if ((uint64_t) &start <= va && va < (uint64_t) &_end_kernel_text)
			perm &= ~PTE_W;

		if ((pte = pml4e_walk (pml4, va, 1)) != NULL)
			*pte = pa | perm;
	}

	// cr3를 다시 적재한다.
	pml4_activate(0);
}

/* kernel command line을 단어들로 나누고 argv와 비슷한 배열로 반환한다. */
static char **
read_command_line (void) {
	static char *argv[LOADER_ARGS_LEN / 2 + 1];
	char *p, *end;
	int argc;
	int i;

	argc = *(uint32_t *) ptov (LOADER_ARG_CNT);
	p = ptov (LOADER_ARGS);
	end = p + LOADER_ARGS_LEN;
	for (i = 0; i < argc; i++) {
		if (p >= end)
			PANIC ("command line arguments overflow");

		argv[i] = p;
		p += strnlen (p, end - p) + 1;
	}
	argv[argc] = NULL;

	/* kernel command line을 출력한다. */
	printf ("Kernel command line:");
	for (i = 0; i < argc; i++)
		if (strchr (argv[i], ' ') == NULL)
			printf (" %s", argv[i]);
		else
			printf (" '%s'", argv[i]);
	printf ("\n");

	return argv;
}

/* ARGV[]의 option을 파싱하고,
   첫 번째 non-option 인자를 반환한다. */
static char **
parse_options (char **argv) {
	for (; *argv != NULL && **argv == '-'; argv++) {
		char *save_ptr;
		char *name = strtok_r (*argv, "=", &save_ptr);
		char *value = strtok_r (NULL, "", &save_ptr);

		if (!strcmp (name, "-h"))
			usage ();
		else if (!strcmp (name, "-q"))
			power_off_when_done = true;
#ifdef FILESYS
		else if (!strcmp (name, "-f"))
			format_filesys = true;
#endif
		else if (!strcmp (name, "-rs"))
			random_init (atoi (value));
		else if (!strcmp (name, "-mlfqs"))
			thread_mlfqs = true;
#ifdef USERPROG
		else if (!strcmp (name, "-ul"))
			user_page_limit = atoi (value);
		else if (!strcmp (name, "-threads-tests"))
			thread_tests = true;
#endif
		else
			PANIC ("unknown option `%s' (use -h for help)", name);
	}

	return argv;
}

/* ARGV[1]에 지정된 작업을 실행한다.
 *
 * run_actions()가 "run" action을 찾으면 이 함수를 호출한다.
 * 이때 argv는 보통 다음 모양이다.
 *
 *   argv[0] = "run"
 *   argv[1] = "args-single onearg"
 *   argv[2] = NULL
 *
 * argv[1]은 실행 파일 이름만이 아니라 사용자 프로그램에 넘길 인자까지
 * 포함한 command line 전체다. userprog 빌드에서는 이 문자열을
 * process_create_initd()에 넘겨 첫 사용자 프로세스를 만들고,
 * process_wait()로 그 프로세스가 끝날 때까지 기다린다. */
static void
run_task (char **argv) {
	/* task = 사용자 프로그램 이름 + 인자.
	   예: "args-single onearg" */
	const char *task = argv[1];

	printf ("Executing '%s':\n", task);
#ifdef USERPROG
	/* thread_tests는 Project 1 thread 테스트를 userprog 빌드에서도
	   돌릴 때 쓰는 플래그다. */
	if (thread_tests){
		run_test (task);
	} else {
		/* 일반적인 Project 2 사용자 프로그램 테스트에서는 이 경로로 온다.
		   process_create_initd()는 task를 실행할 새 kernel thread를 만들고
		   그 tid를 반환한다. process_wait()는 그 child thread(새 kernel thread이자 user mode로 내려가는 thread)가 종료될 때까지
		   현재 action thread(parent)를 대기시킨다. 이 대기가 있어야 사용자
		   프로그램이 끝난 뒤 아래의 "Execution ... complete."까지 진행한다. */
		process_wait (process_create_initd (task));
	}
#else
	run_test (task);
#endif
	printf ("Execution of '%s' complete.\n", task);
}

/* ARGV[]에 남아 있는 action들을 순서대로 실행한다.
 *
 * 여기로 들어오는 ARGV는 parse_options()가 -q, -f 같은 kernel option을
 * 이미 처리한 뒤의 나머지 command line이다. 예를 들어 userprog 테스트의
 * `run "args-single onearg"`는 대략 다음 모양으로 들어온다.
 *
 *   argv[0] = "run"
 *   argv[1] = "args-single onearg"
 *   argv[2] = NULL
 *
 * run_actions()는 argv[0]에 있는 action 이름을 actions[] 표에서 찾고,
 * 그 action에 필요한 인자가 충분한지 확인한 뒤, 연결된 함수를 호출한다.
 * action이 여러 개라면 처리한 action 묶음만큼 argv를 앞으로 옮겨 다음
 * action을 계속 처리한다. action이 하나뿐이면 argv가 NULL sentinel을
 * 가리키게 되어 while 루프가 끝난다. */
static void
run_actions (char **argv) {
	/* action 하나. */
	struct action {
		char *name;                       /* action 이름. */
		int argc;                         /* action 이름을 포함한 인자 수. */
		void (*function) (char **argv);   /* action을 실행할 함수. */
	};

	/* 지원하는 action 표. */
	static const struct action actions[] = {
		{"run", 2, run_task},
#ifdef FILESYS
		{"ls", 1, fsutil_ls},
		{"cat", 2, fsutil_cat},
		{"rm", 2, fsutil_rm},
		{"put", 2, fsutil_put},
		{"get", 2, fsutil_get},
#endif
		{NULL, 0, NULL},
	};

	while (*argv != NULL) {
		const struct action *a;
		int i;

		/* 현재 argv[0]에 있는 action 이름을 지원 action 표에서 찾는다.
		   예: argv[0]이 "run"이면 a->name이 "run"인 항목에서 멈춘다. */
		for (a = actions; ; a++)
			if (a->name == NULL)
				PANIC ("unknown action `%s' (use -h for help)", *argv);
			else if (!strcmp (*argv, a->name))
				break;

		/* 위 loop에서 찾은 action descriptor(a)를 기준으로,
		   해당 action이 요구하는 argv 항목들이 모두 있는지 확인한다.
		   a->argc는 action 이름까지 포함한 총 항목 수다.
		   예: {"run", 2, run_task}는 argv[0]이 이미 "run"으로 확인됐고,
		   추가로 argv[1]에 실행할 task 문자열이 있어야 한다는 뜻이다. */
		for (i = 1; i < a->argc; i++)
			if (argv[i] == NULL)
				PANIC ("action `%s' requires %d argument(s)", *argv, a->argc - 1);

		/* action을 호출한다. "run"이면 run_task(argv)가 실행되고,
		   run_task는 argv[1]을 사용자 프로그램 command line으로 넘긴다. */
		a->function (argv);

		/* 방금 처리한 action과 그 인자들을 건너뛰고 다음 action으로 이동한다.
		   테스트처럼 action이 하나뿐이면 NULL sentinel로 이동해 while이 끝난다.
		   action이 여러 개라면 다음 반복에서 그 다음 action을 처리한다. */
		argv += a->argc;
	}

}

/* kernel command line 도움말을 출력하고 machine의 전원을 끈다. */
static void
usage (void) {
	printf ("\nCommand line syntax: [OPTION...] [ACTION...]\n"
			"Options must precede actions.\n"
			"Actions are executed in the order specified.\n"
			"\nAvailable actions:\n"
#ifdef USERPROG
			"  run 'PROG [ARG...]' Run PROG and wait for it to complete.\n"
#else
			"  run TEST           Run TEST.\n"
#endif
#ifdef FILESYS
			"  ls                 List files in the root directory.\n"
			"  cat FILE           Print FILE to the console.\n"
			"  rm FILE            Delete FILE.\n"
			"Use these actions indirectly via `pintos' -g and -p options:\n"
			"  put FILE           Put FILE into file system from scratch disk.\n"
			"  get FILE           Get FILE from file system into scratch disk.\n"
#endif
			"\nOptions:\n"
			"  -h                 Print this help message and power off.\n"
			"  -q                 Power off VM after actions or on panic.\n"
			"  -f                 Format file system disk during startup.\n"
			"  -rs=SEED           Set random number seed to SEED.\n"
			"  -mlfqs             Use multi-level feedback queue scheduler.\n"
#ifdef USERPROG
			"  -ul=COUNT          Limit user memory to COUNT pages.\n"
#endif
			);
	power_off ();
}


/* 현재 실행 중인 machine의 전원을 끈다.
   단, Bochs나 QEMU에서 실행 중인 경우에 한한다. */
void
power_off (void) {
#ifdef FILESYS
	filesys_done ();
#endif

	print_stats ();

	printf ("Powering off...\n");
	outw (0x604, 0x2000);               /* qemu용 전원 종료 명령 */
	for (;;);
}

/* Pintos 실행 통계를 출력한다. */
static void
print_stats (void) {
	timer_print_stats ();
	thread_print_stats ();
#ifdef FILESYS
	disk_print_stats ();
#endif
	console_print_stats ();
	kbd_print_stats ();
#ifdef USERPROG
	exception_print_stats ();
#endif
}
