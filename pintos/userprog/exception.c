#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"
#include "debug_log.h"

/* 처리한 page fault 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 사용자 프로그램이 일으킬 수 있는 인터럽트의 핸들러를 등록한다.

   실제 Unix 계열 OS에서는 이런 인터럽트 대부분이 [SV-386] 3-24와 3-25에 설명된 것처럼 signal 형태로 사용자
   프로세스에 전달되지만, 우리는 signal을 구현하지 않는다. 대신 사용자 프로세스를 그냥 종료시킨다.

   page fault는 예외다. 여기서는 다른 예외와 같은 방식으로 처리하지만, 가상 메모리를 구현하려면 이것을 바꿔야 한다.

   각 예외에 대한 설명은 [IA32-v3a] 5.15절 "Exception and Interrupt Reference"를 참조하라. */
void
exception_init (void) {
	/* 이러한 예외는 사용자 프로그램이 INT, INT3, INTO, BOUND 명령을 통해 명시적으로 발생시킬 수 있다.
	   따라서 DPL==3으로 설정하며, 이는 사용자 프로그램이 이런 명령으로 이를 유발할 수 있음을 뜻한다. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이러한 예외는 DPL==0을 가지므로 사용자 프로세스가 INT 명령으로 이를 호출하지 못하게 한다.
	   그래도 간접적으로는 발생할 수 있다. 예를 들어 #DE는 0으로 나누기로 인해 발생할 수 있다. */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트를 켠 상태에서 처리할 수 있다.
	   page fault에서는 인터럽트를 꺼야 하는데, fault 주소가 CR2에 저장되어 있어 이를 보존해야 하기 때문이다. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력한다. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 사용자 프로세스가 (아마) 일으킨 예외의 핸들러. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마) 사용자 프로세스가 일으킨 것이다.
	   예를 들어 프로세스가 매핑되지 않은 가상 메모리에 접근하려 했을 수 있다
	   (page fault). 지금은 사용자 프로세스를 그냥 종료시킨다. 나중에는
	   커널에서 page fault를 처리하고자 한다. 실제 Unix 계열 운영체제는
	   대부분의 예외를 signal을 통해 프로세스로 돌려보내지만, 우리는
	   이를 구현하지 않는다. */

	/* 인터럽트 프레임의 code segment 값이 예외가 어디서 발생했는지 알려준다. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 사용자의 코드 세그먼트이므로, 예상한 대로 사용자 예외다. 사용자 프로세스를 종료시킨다. */
			thread_current()->exit_status = -1;
			thread_exit ();

		case SEL_KCSEG:
			/* 커널의 코드 세그먼트이므로 커널 버그를 뜻한다.
			   커널 코드는 예외를 던지면 안 된다. (page fault는 커널 예외를 일으킬 수 있지만, 그것들은
			   여기로 오면 안 된다.) 커널을 panic시켜 이를 분명히 한다. */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 다른 코드 세그먼트인가? 그럴 리 없다. 커널을 panic시킨다. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* page fault 핸들러. 이것은 가상 메모리를 구현하려면 채워 넣어야 하는 스켈레톤이다. project 2의 일부 해법에서는 이
   코드를 수정해야 할 수도 있다.

   진입 시 fault가 발생한 주소는 CR2 (Control Register
   2)에 있고, fault에 대한 정보는 exception.h의 PF_* 매크로에 설명된 형식으로 F의 error_code 멤버에 들어
   있다. 여기의 예제 코드는 그 정보를 어떻게 파싱하는지 보여 준다. 이 둘에 대한 더 많은 정보는 [IA32-v3a] 5.15절
   "Exception and Interrupt Reference"의 "Interrupt 14--Page Fault Exception
   (#PF)" 설명에서 찾을 수 있다. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* 참: not-present page, 거짓: r/o page에 쓰기. */
	bool write;        /* 참: write 접근, 거짓: read 접근. */
	bool user;         /* 참: user에 의한 접근, 거짓: kernel에 의한 접근. */
	void *fault_addr;  /* fault 주소. */

	/* fault를 일으킨 주소, 즉 fault를 유발한 가상 주소를 알아낸다. 이는 code나 data를 가리킬 수 있다. 반드시
	   fault를 일으킨 명령어의 주소는 아니다(그 주소는 f->rip이다). */

	fault_addr = (void *) rcr2();

	/* CR2가 바뀌기 전에 읽는 것을 보장하기 위해 인터럽트를 잠시 껐으니 다시 켠다. */
	intr_enable ();


	/* 원인을 판별한다. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* project 3 이후용. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	if (user) {
		DBG ("페이지 폴트: user 문제");
		thread_exit ();
	}

	/* page fault를 센다. */
	page_fault_cnt++;

	/* fault가 진짜 fault라면 정보를 보여 주고 종료한다. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

