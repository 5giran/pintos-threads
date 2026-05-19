
#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h" /* child_status 내부의 semaphore 사용을 위해 */
#ifdef VM
#include "vm/vm.h"
#endif

struct file;            /* struct file의 전방 선언 */

#define FD_MAX 128		/* 프로세스당 가질 수 있는 최대 파일 디스크립터 개수 */

/* thread 생명 주기의 상태들. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 thread. */
	THREAD_READY,       /* 실행 중은 아니지만 실행 준비가 된 상태. */
	THREAD_BLOCKED,     /* 어떤 event가 발생하기를 기다리는 상태. */
	THREAD_DYING        /* 곧 파기될 상태. */
};

/* thread 식별자 타입.
   원하는 타입으로 재정의해도 된다. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 오류 값. */

// #ifdef USERPROG
/* parent와 child가 공유하는 process lifecycle record.
 * record 객체는 struct thread 안에 값으로 넣지 않고, process.c에서
 * malloc()으로 별도 할당해 parent/child가 pointer로 공유한다. */
struct child_status {
	tid_t tid;                    /* child thread id */
	int exit_status;              /* child의 종료 status */
	bool has_exited;              /* child가 이미 종료되었는지 여부 */
	bool has_been_waited;         /* parent가 wait 권한을 이미 사용했는지 여부 */
	int ref_cnt;                  /* parent 몫 1 + child 몫 1, 둘 다 release하면 free */

	struct semaphore wait_sema;   /* child exit 전 parent가 wait할 semaphore */
	struct list_elem elem;        /* parent->children list에 연결할 elem */
};

struct file;
// #endif

/* thread priority(스레드 우선순위). */
#define PRI_MIN 0                       /* 가장 낮은 priority. */
#define PRI_DEFAULT 31                  /* 기본 priority. */
#define PRI_MAX 63                      /* 가장 높은 priority. */

/* kernel thread 또는 user process.
 *
 * 각 thread 구조체는 자기 전용 4 kB page에 저장된다.
 * thread 구조체 자체는 페이지의 맨 아래(offset 0)에 놓인다.
 * 페이지의 나머지 부분은 thread의 kernel stack 용도로 예약되며,
 * kernel stack은 페이지의 맨 위(offset 4 kB)에서 아래로 자란다.
 * 그림으로 보면 다음과 같다:
 *
 *      4 kB +---------------------------------+
 *           |          kernel stack           |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |         grows downward          |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 여기서 얻을 수 있는 결론은 두 가지다:
 *
 *    1. 첫째, `struct thread'가 너무 커지면 안 된다.
 *       너무 커지면 kernel stack을 둘 공간이 부족해진다.
 *       기본 `struct thread'는 겨우 몇 바이트 크기다.
 *       아마 1 kB보다 훨씬 작게 유지되어야 한다.
 *
 *    2. 둘째, kernel stack도 너무 커지면 안 된다.
 *       stack이 overflow되면 thread 상태를 손상시킨다.
 *       따라서 kernel 함수는 큰 구조체나 배열을 non-static 지역 변수로
 *       할당해서는 안 된다. 대신 malloc()이나 palloc_get_page() 같은
 *       동적 할당을 사용하라.
 *
 * 이 두 문제 중 어느 것이든 첫 증상은 대개 thread_current()에서의
 * assertion failure일 것이다. 이 함수는 현재 실행 중인 thread의
 * `struct thread'에 있는 `magic' 멤버가 THREAD_MAGIC으로 설정되어 있는지
 * 검사한다. stack overflow가 발생하면 보통 이 값이 바뀌어 assertion이 터진다. */
/* `elem' 멤버는 두 가지 용도를 가진다.
 * run queue(thread.c)의 원소가 될 수도 있고,
 * semaphore 대기 리스트(synch.c)의 원소가 될 수도 있다.
 * 두 용도가 상호 배타적이기 때문에 이렇게 사용할 수 있다.
 * ready 상태의 thread만 run queue에 있고,
 * blocked 상태의 thread만 semaphore 대기 리스트에 있기 때문이다. */
struct thread {
	/* Owned by thread.c. */
	tid_t tid;                          /* Thread identifier. */
	enum thread_status status;          /* Thread state. */
	char name[16];                      /* Name (for debugging purposes). */
	int priority;                       /* donation이 반영된 유효 우선순위. */
	int base_priority;									/* donation 제거 후 돌아갈 원래 priority. */
	int64_t wakeup_ticks;								/* 잠든 스레드가 깨어날 수 있는 최소 절대 tick. */

	/* thread.c와 synch.c가 공유한다. */
	struct list_elem elem;              /* ready_list, semaphore waiters 원소. */
	
	/* 우선순위 기부 상태. */
	struct list_elem donation_elem;			/* 이 thread가 다른 thread의 donations 리스트에 들어갈 때 쓰는 elem. */
	struct list donations;							/* 이 thread에게 priority를 기부한 donor thread들의 목록. */
	struct lock *wait_lock;							/* 이 thread가 현재 기다리는 lock. 기다리는 lock이 없으면 NULL. */

// #ifdef USERPROG
	/* userprog/process.c가 소유한다. */
	uint64_t *pml4;                     /* PML4 테이블 */
	int exit_status;										/* syscall exit status */

	struct list children;								/* 현재 thread가 생성한 direct child들의 child status record 목록 (요소 자체가 child_status) */
	struct child_status *child_status;  /* 현재 thread의 parent에게 넘길 현재 thread의 record */

	struct file *running_file;					/* 현재 thread가 load 하고 실행하고 있는 file */
	struct file *fd_table[FD_MAX];     	/* 프로세스별 파일 디스크립터 테이블 */
	int next_fd;                       	/* 다음 할당 후보 fd */
// #endif
#ifdef VM
	/* thread가 소유한 전체 virtual memory에 대한 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c가 소유한다. */
	/* Context Switching 에 필요한 정보, CPU 상태를 저장해둔 구조체 
	 * CPU가 나중에 다시 실행을 이어가기 위해 필요한 레지스터 묶음 (세이브 파일이라 생각하면 편하다)
	*/
	struct intr_frame tf;               
	unsigned magic;                     /* stack overflow를 감지한다. */
	uintptr_t rsp;						/* 커널모드 rsp값 저장 */
};

/* false(기본값)면 round-robin scheduler를 사용한다.
   true면 multi-level feedback queue scheduler를 사용한다.
   kernel command-line option "-o mlfqs"로 제어한다. */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *, int, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

/* 우선순위 기부 보조 함수들. */
/* base_priority와 donations를 기준으로 thread의 유효 우선순위를 다시 계산한다. */
void refresh_priority(struct thread *);

/* donor가 receiver에게 priority를 기부하고 필요한 경우 lock holder chain 위로 전파한다. */
void donate_priority (struct thread *, struct thread *);

/* 현재 thread가 lock을 해제할 때 해당 lock 때문에 받은 donation을 제거한다. */
void remove_donation (struct lock *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

#endif /* threads/thread.h */
