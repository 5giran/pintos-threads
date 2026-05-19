#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "userprog/syscall.h"
#include "userprog/fd.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/malloc.h"
#include "debug_log.h"
#ifdef VM
#include "vm/vm.h"
#endif

#define MAX_ARGS PGSIZE / sizeof(char *) - 1

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_, int argc, char *argv_tokens[]);
static void initd (void *f_name);
static void __do_fork (void *);
static void running_file_close (struct thread *t);
static void running_file_install (struct thread *t, struct file *file);
static bool running_file_duplicate (struct thread *dst, struct thread *src);

struct fork_aux {
	struct thread *parent;
	struct child_status *cs;
	struct intr_frame if_;
	struct semaphore sema;
	bool success;
};

/* child_status record 하나를 새로 만든다. 
 * 만들지 못하면 NULL 반환
 * record 안의 필드들을 기본값으로 채운다.
 * parent가 기다릴 semaphore를 0으로 초기화
 * 만든 record 주소를 반환
 * 이 함수는 process_create_initd()와 process_fork() 에서 사용되는 helper 함수다.
*/
static struct child_status *
child_status_create (void) {
	/* child_status 타입 구조체를 가리키는 포인터 변수인 cs에게 동적으로 메모리를 할당한다.
	 * 왜 동적 메모리를 할당해줘야하고, 왜 palloc 대신 malloc을 사용할까?
	 * 동적 메모리 할당 이유: 만약 process_fork()나 process_create_initd() 안의 지역 변수로 선언 하면, 그 함수들이 종료될 때 스택에 올라간 child_status의 메모리도 사라진다.
	 * 하지만 나중에 child나 parent가 child_status 내부의 값을 변경하고자 접근할 수 있으므로, child_status의 메모리는 함수 종료와 상관 없이 계속 살아있어야 한다. 그래서 kernel heap/page 영역에 만들어야 함.
	 * palloc 안 쓰는 이유: 이 구조체 하나 만들고자 4KB page 주는건 낭비.
	*/
	struct child_status *cs = malloc(sizeof *cs);
	if (cs == NULL) {
		return NULL;
	}

	cs->tid = TID_ERROR; /* 처음에는 임시값으로 초기화, thread_create () 성공하면 tid 갱신 */
	cs->exit_status = -1;
	cs->has_exited = false;
	cs->has_been_waited = false;
	cs->ref_cnt = 2;
	sema_init(&cs->wait_sema, 0);

	return cs;
}

/* child_status_find ()
 * parent->children list에서 tid로 record 찾기
 * process_wait 에서 사용할 용도
*/
static struct child_status *
child_status_find (struct thread *parent, tid_t child_tid) {
	/* parent->children list 순회 하면서 parent->children->tid == child_tid 면 그게 우리가 찾는 자식 스레드*/

	struct list_elem *e = list_begin (&parent->children);

	while (e != list_end (&parent->children)) {
		struct child_status *cs = list_entry (e, struct child_status, elem);
		if (cs->tid == child_tid) {
			return cs;
		}
		e = list_next(e);
	}
	/* 못 찾으면 NULL return */
	return NULL;
}

/* child_status_release () */
static void
child_status_release (struct child_status *cs) {
	bool should_free;
	enum intr_level old_level;

	/* record가 NULL이면 이미 메모리 해제된 것이므로 바로 return */
	if (cs == NULL) {
		return;
	}

	old_level = intr_disable ();
	ASSERT(cs->ref_cnt > 0);
	/* 이 record의 참조 수를 1 감소시킨다. */
	cs->ref_cnt--;
	/* ref_cnt가 0이면 malloc으로 할당 받았던 record의 memory를 free */
	should_free = cs->ref_cnt == 0;
	intr_set_level (old_level);

	if (should_free) {
		free(cs);
	}
}

/* initd에 넘길, 자식 프로세스가 받아야 하는 aux 인자 묶음 */
struct initd_aux {
	char *fn_copy;
	struct child_status *cs;
};

/* initd와 다른 프로세스를 위한 일반 프로세스 초기화. */
static void
process_init (void)
{
	struct thread *current = thread_current();
	fd_init (current);
}

/* running_file은 fd table과 별개의 실행 파일 lifetime record다.
 * file_deny_write()는 file object가 닫힐 때 풀리므로, exec 성공부터
 * process cleanup까지 file을 열어 둔다는 invariant를 이 helper들에 모은다. */
static void
running_file_close (struct thread *t)
{
	if (t->running_file == NULL)
		return;

	lock_acquire (&filesys_lock);
	file_close (t->running_file);
	lock_release (&filesys_lock);
	t->running_file = NULL;
}

static void
running_file_install (struct thread *t, struct file *file)
{
	ASSERT (t->running_file == NULL);
	ASSERT (file != NULL);

	lock_acquire (&filesys_lock);
	file_deny_write (file);
	lock_release (&filesys_lock);
	t->running_file = file;
}

static bool
running_file_duplicate (struct thread *dst, struct thread *src)
{
	ASSERT (dst->running_file == NULL);

	if (src->running_file == NULL)
		return true;

	lock_acquire (&filesys_lock);
	dst->running_file = file_duplicate (src->running_file);
	lock_release (&filesys_lock);

	return dst->running_file != NULL;
}

/* FILE_NAME command line으로 첫 user process를 시작할 kernel thread를 만든다.
 *
 * run_task()는 "args-single onearg" 같은 전체 command line을 이 함수에
 * 넘긴다. 이 함수가 사용자 코드를 직접 실행하는 것은 아니다. 대신 새
 * kernel thread를 만들고, 그 thread가 initd(fn_copy)를 실행하게 한다.
 *
 * 새 thread는 initd() 안에서 process_exec() -> load() -> do_iret() 순서로
 * 사용자 프로그램을 적재하고 user mode로 진입한다. 따라서 반환값 tid는
 * "방금 만든 kernel thread이자 첫 user process"를 기다리기 위해
 * process_wait()에 전달된다.
 *
 * 새 스레드는 process_create_initd()가 반환하기 전에 스케줄될 수 있고
 * 심지어 종료될 수도 있다. 그래서 thread_create()에 넘길 command line은
 * 호출자 stack이 아니라 별도 page(fn_copy)에 복사해 둔다. */
tid_t 
process_create_initd (const char *file_name)
{
	char *fn_copy;			/* 원본 명령줄을 저장해 둘 커널 페이지 포인터 */
	char thread_name[16];	/* 새 스레드 이름으로 쓸 짧은 버퍼 */
	size_t name_len;		/* 첫 번째 토큰 길이 계산용 */
	tid_t tid;				/* 생성된 스레드의 tid를 받을 변수 */
	struct thread *parent = thread_current ();
	
	/* 명령줄 전체를 저장할 페이지 하나를 할당한다. */
	fn_copy = palloc_get_page (0);

	/* 할당 실패 시 프로세스 생성 실패 */
	if (fn_copy == NULL)
		return TID_ERROR;

	/* 인자 묶음을 가리키는 포인터 변수 ia의 메모리 동적 할당 */
	struct initd_aux *ia = malloc(sizeof *ia);

	/* 할당 실패 시 fn_copy page free 후 프로세스 생성 실패 */
	if (ia == NULL) {
		palloc_free_page(fn_copy);
		return TID_ERROR;
	}
	/* 성공하면 fn_copy 갱신 */
	ia->fn_copy = fn_copy;

	/* "args-single one two" 같은 전체 명령줄을 페이지에 복사한다.
	   이 복사본은 새 thread의 initd()가 받아서 process_exec()에 넘긴다.
		 복사본을 넘기지 않고 그냥 filename을 thread_create의 aux 인자로 넘기면 위험하다. */
	strlcpy (fn_copy, file_name, PGSIZE);

	/* 첫 공백 전까지 길이를 구한다.
	   즉 실행 파일 이름만 뽑는다. */
	name_len = strcspn (file_name, " ");

	/* thread 이름 버퍼를 넘지 않도록 자른다. */
	if (name_len >= sizeof thread_name)
		name_len = sizeof thread_name - 1;

	/* 실행 파일 이름만 복사한다. 예: "args-single" */
	memcpy (thread_name, file_name, name_len);

	/* C 문자열 끝 표시 */
	thread_name[name_len] = '\0';

	/* child_status를 먼저 생성해줘야함. thread_create이 실행되는 순간 끝까지 실행되고 끝날 수 있기 때문에, 먼저 status setting이 필요 */
	struct child_status *cs = child_status_create ();
	/* 할당 실패 시 fn_copy page free 후 프로세스 생성 실패 */
	if (cs == NULL) {
		palloc_free_page (fn_copy);
		free(ia);
		return TID_ERROR;
	}
	/* 성공하면 cs 갱신 */
	ia->cs = cs;

	/* 새 kernel thread를 만든다.
	   스레드 이름은 실행 파일 이름만 사용하고,
		 그 스레드의 시작 함수로 initd를 등록한다. 스레드 실행에 필요한
	   데이터는 전체 명령줄 복사본 fn_copy를 aux 인자로 넘긴다.
		 새 스레드를 ready queue에 넣는다.
	   새 thread가 시작되면 kernel_thread()를 거쳐 initd(fn_copy)를 호출한다.
		 나중에 스케줄러가 이 스레드를 실행시키면 initd(aux)부터 시작하게 해라.*/
	tid = thread_create (thread_name, PRI_DEFAULT, initd, ia); // child_status 연결은 initd(aux)에서 수행됨
	/* 스레드 생성 실패 시 아까 잡은 페이지를 반환한다. */
	if (tid == TID_ERROR) {
		palloc_free_page (fn_copy);
		free(cs); // 여기서 child_status_release(cs) 해도 되지만, 아직 공유 record로 등록되기 전이라 free가 자연스러움.
		free(ia);
		return TID_ERROR;
	}
	/* 성공 했을 경우 */
	cs->tid = tid;
	/* 현재 스레드의 children list에 cs->elem 삽입 */
	list_push_back (&(parent->children), &(cs->elem));
	
	/* 새 프로세스의 tid 반환 */
	return tid;
}

/* 첫 사용자 프로세스를 시작하는 스레드 함수. */
static void
initd (void *aux)
{
#ifdef VM
	supplemental_page_table_init (&thread_current()->spt);
#endif
	struct initd_aux *ia = aux;
	char *fn_copy = ia->fn_copy;
	struct child_status *cs = ia->cs;
	thread_current ()->child_status = cs;
	process_init();
	free(ia);
	
	if (process_exec (fn_copy) < 0)
		PANIC ("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`으로 복제한다. 새 프로세스의 thread id를 반환하며, 스레드를 생성할 수 없으면 TID_ERROR를
 * 반환한다. */
tid_t 
process_fork (const char *name, struct intr_frame *if_)
{
	/* 현재 스레드를 새 스레드로 복제한다. */
	struct thread *parent = thread_current ();
	struct child_status *cs = child_status_create ();
	struct fork_aux *fa;
	tid_t tid;

	if (cs == NULL)
		return TID_ERROR;

	list_push_back (&parent->children, &cs->elem);

	fa = malloc (sizeof *fa);
	if (fa == NULL) {
		list_remove (&cs->elem);
		free (cs);
		return TID_ERROR;
	}

	fa->parent = parent;
	fa->cs = cs;
	fa->if_ = *if_;
	fa->success = false;
	sema_init (&fa->sema, 0);

	tid = thread_create (name, PRI_DEFAULT, __do_fork, fa);
	if (tid == TID_ERROR) {
		DBG ("process_fork: thread_create failed name=%s parent=%s parent_tid=%d\n",
			 name, parent->name, parent->tid);
		list_remove (&cs->elem);
		free (cs);
		free (fa);
		return TID_ERROR;
	}

	cs->tid = tid;

	sema_down (&fa->sema);

	if (!fa->success) {
		list_remove (&cs->elem);
		child_status_release (cs);
		free (fa);
		return TID_ERROR;
	}

	free (fa);
	return tid;
}

#ifndef VM
/* 이 함수를 pml4_for_each에 넘겨 부모의 주소 공간을 복제한다. 이것은 project 2에서만 사용된다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux)
{
	struct thread *current = thread_current();
	struct thread *parent = (struct thread *)aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: parent_page가 kernel page이면 즉시 반환한다. 
	 * user address space만 복제함. */
	if (is_kernel_vaddr (va)) {
		return true;
	}
	/* 2. 부모의 page map level 4에서 VA를 구한다. 
	 * 만약 부모의 page table에서 va가 가리키는 실제 kernel mapping 주소를 찾는데,
	 * 없다면..? 부모쪽 매핑 상태가 이상하기에 실패 처리해버림. */
	parent_page = pml4_get_page(parent->pml4, va);
	if (parent_page == NULL)
		return false;

	/* 3. TODO: 자식용 새 PAL_USER page를 할당하고 결과를
	 * TODO: NEWPAGE로 설정한다. 
	 * 부모 페이지와 독립된 복사본이여야 함. 아니면 실패 처리. */
	newpage = palloc_get_page (PAL_USER);
	if (newpage == NULL)
		return false;
	
	/* 4. TODO: 부모의 page를 새 page로 복제하고
	 * TODO: 부모의 page가 writable인지 확인한다(결과에 따라 WRITABLE을
	 * TODO: 설정한다). */
	memcpy (newpage, parent_page, PGSIZE);
	/* 원래 페이지의 WRITEABLE 비트를 그대로 보존함. */
	writable = is_writable (pte);

	/* 5. 주소 VA에 WRITABLE 권한으로 새 page를 자식의 page table에 추가한다.
	 * 즉, va->newpage 매핑 설치. */
	if (!pml4_set_page(current->pml4, va, newpage, writable))
	{
		/* 6. TODO: page 삽입에 실패하면 새로 할당한 페이지를 반드시 회수해야 하므로 오류 처리를 한다. */
		palloc_free_page (newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 execution context를 복사하는 스레드 함수.
 * 힌트) parent->tf는 프로세스의 userland context를 담고 있지 않다.
 * 즉, process_fork의 두 번째 인자를 이 함수에 넘겨야 한다. */
static void
__do_fork (void *aux)
{
	struct fork_aux *fa = aux;
	struct intr_frame if_;
	struct thread *parent = fa->parent;
	struct thread *current = thread_current ();
	/* TODO: 어떻게든 parent_if를 전달한다. (즉, process_fork()의 if_) */
	// struct intr_frame *parent_if;
	// bool succ = true;

	current->child_status = fa->cs;
	process_init ();

	/* 1. CPU context를 local stack에 읽어 온다. 
	 * 자식은 fork 직후, 부모와 거의 같은 register state에서 시작해야 함.
	 * 그래서 부모가 syscall 진입 시에 저장해 둔 intr_frame을 먼저 복사함.
	 * 나중에 child의 fork 반환값만 0으로 변경해줌. */

	memcpy (&if_, &fa->if_, sizeof if_);

	/* 2. PT를 복제한다 
	 * child만의 새 페이지 테이블을 만든다.
	 * 부모의 pml4를 그대로 공유하는 게 아니라,자식 전용 address space를 새로 구성해야 함. */
	current->pml4 = pml4_create ();
	if (current->pml4 == NULL)
		goto error;

	/* 현재 CPU가 자식(child)의 페이지 테이블을 기준으로 동작하라고 함. */
	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	bool spt_copy_success = supplemental_page_table_copy (&current->spt, &parent->spt);
	bool spt_copy_failed = !spt_copy_success;
	DBG ("__do_fork: supplemental_page_table_copy result=%d failed=%d parent=%s parent_tid=%d child=%s child_tid=%d\n",
		 spt_copy_success, spt_copy_failed, parent->name, parent->tid,
		 current->name, current->tid);
	if (spt_copy_failed) {
		DBG ("__do_fork: entering error path after supplemental_page_table_copy failure child=%s child_tid=%d\n",
			 current->name, current->tid);
		goto error;
	}
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	if (!fd_duplicate_all (current, parent))
		goto error;

	if (!running_file_duplicate (current, parent))
		goto error;

	/* child 입장에서는 fork()의 반환값이 0이어야 한다. */
	if_.R.rax = 0;

	/* 여기까지 오면 child address space 복제가 끝난 상태란 것.
	 * 이후 user mode로 복귀하면 parent와 같은 코드 위치에서 실행을 이어간다. */
	fa->success = true;
	sema_up (&fa->sema);
	do_iret (&if_);

error:
	fa->success = false;
	sema_up (&fa->sema);
	thread_exit ();
}	


	/* TODO: 여기에 코드를 작성하세요.
	 * TODO: 힌트) 파일 객체를 복제하려면 `file_duplicate`를 사용하세요.
	 * TODO:       include/filesys/file.h에 있습니다. parent는
	 * TODO:       이 함수가 parent의 자원을 성공적으로 복제할 때까지 fork()에서 반환하면 안 됩니다. */

	/* 마지막으로, 새로 생성된 프로세스로 전환합니다. */


/* 현재 실행 컨텍스트를 f_name으로 전환합니다.
 * 실패 시 -1을 반환합니다. */
int 
process_exec (void *f_name)
{
	char *file_name = f_name;
	bool success;
	/* 파싱 결과 담을 페이지(포인터 연산으로 배열처럼 이용) */
	char **argv_tokens = palloc_get_page (0);
	if (argv_tokens == NULL) {
		palloc_free_page(file_name);
		return -1;
	}
	/* strtok_r 함수 사용을 위한 포인터 */
	char *token;
	char *next_token;
	/* argv_tokens에 argument 저장과 argc 계산을 위한 변수 */
	int argc = 0;
	/* strtok_r을 이용해 tokenize */
	for (token = strtok_r (file_name, " ", &next_token);
			 token != NULL;
			 token = strtok_r (NULL, " ", &next_token)) {
		/* argv_tokens[MAX_ARGS]에 접근 전에 반복문이 끝나야 함.
		 * 그렇지 않으면, 배열의 크기보다 인자 수가 더 많은 것
		*/		
		if (argc == MAX_ARGS) {
			palloc_free_page(file_name);
			palloc_free_page(argv_tokens);
			return -1;
		}

		argv_tokens[argc] = token;
		argc++;
	}
	/* 0부터 시작했으니 배열의 argc 번째 요소는 NULL */
	argv_tokens[argc] = NULL;

	/* thread 구조체의 intr_frame은 사용할 수 없습니다.
	 * 그 이유는 intr_frame에는 현재 스레드가 다시 스케줄될 때 이전 실행 정보를 저장하기 때문입니다. (복구할 실행 상태 저장, 스케줄링 재개용)
	 * 아래는 user mode에서 사용자 코드 영역을 실행하고, 사용자 데이터/스택 영역을 쓰도록 준비 시키는 것이기 때문에,
	 * 스레드 구조체 안의 intr_frame을 못 쓰니 지역 변수로 새롭게 만들어주는 것입니다.
	 * 새 유저 프로그램을 main(argc, argv) 쪽으로 시작시키기 위한 CPU 상태를 저장
	*/
	struct intr_frame _if; // user mode로 진입 시 CPU 레지스터들이 어떤 값이어야 하는지를 담는 임시 구조체
	_if.ds = _if.es = _if.ss = SEL_UDSEG; // user data segment 사용
	_if.cs = SEL_UCSEG; // user code segment 사용
	_if.eflags = FLAG_IF | FLAG_MBS; // 인터럽트 허용 + 반드시 켜져 있어야 하는 기본 비트 설정

	/* 먼저 현재 컨텍스트를 종료합니다 
	 * 새 프로세스가 시작되는 상황에서는 방어적인 정리이지만,
	 * 이미 유저 프로그램을 실행 중인 프로세스가 system call exec을 실행하는 경우에는 의미 있게 동작.
	 * 기존 유저 프로그램의 주소 공간, 페이지 테이블(pml4), supplemental page table, 매핑된 유저 메모리 등을 정리
	 * */
	process_cleanup ();

	/* 그런 다음 바이너리를 로드합니다 -> 컴파일된 실행 파일(.o, binary)을 파일 시스템에서 읽어서, 그 프로그램이 실행될 수 있도록 현재 프로세스의 유저 메모리에 올리고, 시작 주소와 스택을 세팅한다.
		 parsing 이후이기 때문에, 첫 인자로 argv_tokens[0]을 넘겨주어야 한다.
	*/
	success = load (argv_tokens[0], &_if, argc, argv_tokens); // argv_tokens[0]에 해당하는 실행 파일을 찾아 메모리에 올리고, argv_tokens에 있는 인자들을 유저 스택에 올려서 성공하면 1, 실패하면 0 반환
	/* 로드에 실패하면 종료합니다. 
		 file_name은 f_name의 포인터이므로, f_name을 free 하는 것과 같음. 
		 f_name은 process_create_initd()에서 할당 받은 페이지
	*/
	palloc_free_page (argv_tokens);
	palloc_free_page (file_name);
	if (!success) {
		return -1;
	}
		

	/* 전환된 프로세스를 시작합니다. */
	do_iret (&_if);
	NOT_REACHED ();
}

/* 1. 내 child가 맞는지 확인한다.
2. 아직 살아 있으면 끝날 때까지 기다린다.
3. child가 남긴 exit_status를 읽는다.
4. 그 status를 parent에게 반환한다.
5. child record를 정리한다. */
int 
process_wait (tid_t child_tid)
{
	/* XXX: 힌트) pintos는 process_wait (initd)에서 종료합니다. process_wait를 구현하기 전에
	 * XXX:       여기서 무한 루프를 추가하는 것을 권장합니다. */
	/* 현재는 임시적으로 구현 */

	/* 이 함수가 호출 되었을 때 자식 프로세스가 살아있다면 부모 프로세스는 왜 잠들어야할까?
	 * child가 수정할 수 있는 record(공유 자료)를 parent가 접근하면 안되기도 하고, busy wait을 하자니 CPU 낭비가 너무 심함. 제대로 기다리는 것도 아니고.
	 */
	struct thread *curr = thread_current ();
	struct child_status *cs = child_status_find (curr, child_tid);
	
	if (cs == NULL) {
		return -1;
	}
	/* 이미 현재 스레드가 wait 권한 사용했을 경우 */
	if (cs->has_been_waited) {
		return -1;
	}
	/* wait 권한 살아있을 경우 */
	cs->has_been_waited = true;
	/* 아직 살아있을 경우 */
	if (!cs->has_exited) {
		sema_down (&(cs->wait_sema));
	}
	/* sema_up 이후 아래 코드 실행 혹은 자식이 이미 exit 했을 경우에는 바로 실행됨 */

	/* 반환할 자식 스레드의 exit_status를 저장 */
	int child_es = cs->exit_status;
	/* 현재 스레드의 children list 에서 종료된 자식 스레드 제거 */
	list_remove (&(cs->elem));
	/* 현재 스레드(parent) 몫의 ref_cnt release */
	child_status_release (cs);
			
	return child_es;
}

/* 프로세스를 종료합니다. 이 함수는 thread_exit ()에서 호출됩니다. */
void 
process_exit (void)
{
	struct thread *curr = thread_current ();
	/* TODO: 여기에 코드를 작성하세요.
	 * TODO: 프로세스 종료 메시지를 구현하세요(참고:
	 * TODO: project2/process_termination.html).
	 * TODO: 여기서 프로세스 자원 정리를 구현하는 것을 권장합니다. */
	running_file_close (curr);

	/* 유저 프로세스의 thread라면 종료 메시지 출력, pure kernel thread면 메시지 출력 X 
	 * 유저 프로세스라면 pml4(페이지 테이블)가 존재한다.
	*/
	if (curr->pml4 != NULL) {
		printf ("%s: exit(%d)\n", thread_name (), curr->exit_status);
	}
	struct child_status *cs = curr->child_status;
	/* 현재 스레드가 부모 스레드를 가지고 있다면 공유 자료구조 업데이트 필요 */
	if (cs != NULL) {
		cs->exit_status = curr->exit_status; // 이미 system call 호출될 때, curr->exit_status 는 update 되었음. syscall.c 참고
		cs->has_exited = true;
		sema_up (&cs->wait_sema); // 부모 스레드가 wait_sema 에서 기다리고 있을 수 있으니, sema_up 필요
		child_status_release (cs); // cs->ref_cnt--;, 이후 ref_cnt == 0 이면 내부적으로 free 진행
		curr->child_status = NULL; // child_status_release() 에서 처리하게 하면 안됨. 이 함수 자체는 parent도 process_wait() 이나 다른 함수에서 호출 할 수도 있기 때문에, 우리 의도와 다르게 동작할 수 있음.
	}
	/* 현재 스레드가 자식을 갖고 있다면 정리 해야함 */
	while (!list_empty (&(curr->children))) {
		struct list_elem *e = list_pop_front (&(curr->children));
		struct child_status *cs = list_entry (e, struct child_status, elem);
		child_status_release (cs);
	}

	fd_close_all ();
	process_cleanup ();
}

/* 현재 프로세스의 자원을 해제합니다. */
static void
process_cleanup (void)
{
	struct thread *curr = thread_current ();
	running_file_close (curr);

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 page directory를 파괴하고 kernel-only page directory로 다시 전환합니다. */
	pml4 = curr->pml4; // 현재 프로세스의 페이지 테이블 가져오기
	if (pml4 != NULL)
	{
		/* 여기서 올바른 순서는 매우 중요합니다.  timer interrupt가 프로세스 page directory로 되돌아가지 못하도록
		 * cur->pagedir를 page directory 전환 전에 NULL로 설정해야 합니다. 프로세스의 page directory를
		 * 파괴하기 전에 base page directory를 활성화해야 합니다.  그렇지 않으면 현재 활성화된 page directory가
		 * 이미 해제되어 비워진 것을 가리키게 됩니다. 
		 * 쉽게 말하면:
		 * 낡은 pml4를 먼저 thread 구조체에서 끊어내고 -> 그 다음 CPU에서도 끊어내고 -> 그 다음 메모리에서 파괴
		 * 만약 curr->pml4 = NULL; 이 나중에 있을 경우, 인터럽트나 컨텍스트 스위칭 발생 후 process_activate가 curr->pml4가 NULL이 아니라서 destroy 될 예정이거나 destroy 된 page table을 다시 활성화할 수도 있음
		 * */
		curr->pml4 = NULL; // 이 스레드는 이제 기존 유저 주소 공간이 없다. 이 코드를 아래 두 줄보다 먼저 써줘야, 중간에 timer interrupt 같은게 들어와도 이미 현재 스레드의 주소 공간을 해제 했기 때문에 안전함 (disconnect with the struct thread)
		pml4_activate (NULL); // CPU는 항상 어떤 페이지 테이블을 하나를 참고하는데, 그것을 기존 유저 프로그램 페이지 테이블 -> 커널 전용 페이지 테이블로 전환하는 함수, 내가 밟고 있는 발판 destroy 전에 커널 발판으로 옮겨가는 것 (disconnect with CPU)
		pml4_destroy (pml4); // 기존 유저 주소 공간 실제로 해제 (disconnect with memory)
	}
}

/* 다음 스레드에서 사용자 코드를 실행할 수 있도록 CPU를 설정합니다.
 * 이 함수는 모든 context switch마다 호출됩니다. */
void process_activate (struct thread *next)
{
	/* 스레드의 page table을 활성화합니다. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 kernel stack을 설정합니다. */
	tss_update (next);
}

/* ELF 바이너리를 로드합니다.  다음 정의는 ELF 사양 [ELF1]에서
 * 거의 그대로 가져왔습니다. */

/* ELF 타입입니다.  [ELF1] 1-2를 참조하세요. */
#define EI_NIDENT 16

#define PT_NULL 0			/* 무시합니다. */
#define PT_LOAD 1			/* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2		/* 동적 링크 정보. */
#define PT_INTERP 3			/* 동적 로더의 이름. */
#define PT_NOTE 4			/* 보조 정보. */
#define PT_SHLIB 5			/* 예약됨. */
#define PT_PHDR 6			/* 프로그램 헤더 테이블. */
#define PT_STACK 0x6474e551 /* 스택 세그먼트. */

#define PF_X 1 /* 실행 가능. */
#define PF_W 2 /* 쓰기 가능. */
#define PF_R 4 /* 읽기 가능. */

/* 실행 헤더입니다.  [ELF1] 1-4부터 1-8을 참조하세요.
 * 이것은 ELF 바이너리의 맨 앞에 나타납니다. */
struct ELF64_hdr
{
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR
{
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
						 uint32_t read_bytes, uint32_t zero_bytes,
						 bool writable);

bool
read_file_exact_at (struct file *file, void *buffer, off_t size, off_t ofs)
{
	off_t bytes_read;

	lock_acquire (&filesys_lock);
	bytes_read = file_read_at (file, buffer, size, ofs);
	lock_release (&filesys_lock);

	return bytes_read == size;
}

static off_t
file_length_synchronized (struct file *file)
{
	off_t length;

	lock_acquire (&filesys_lock);
	length = file_length (file);
	lock_release (&filesys_lock);

	return length;
}

/* FILE_NAME에서 ELF 실행 파일을 현재 스레드에 로드합니다.
 * 실행 파일의 진입점을 *RIP에 저장하고 초기 스택 포인터를 *RSP에 저장합니다.
 * 성공하면 true, 그렇지 않으면 false를 반환합니다. */
static bool
load (const char *file_name, struct intr_frame *if_, int argc, char *argv_tokens[]) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;
	/* 문자열을 user stack에 복사한 위치의 주소를 담는 배열, char * 보다도 void * 가 의미에 맞음. */
	void **arg_addr = palloc_get_page (0);
	if (arg_addr == NULL) {
		goto done;
	}
	void *argv_addr;

	/* page directory를 할당한다.
		 이 사용자 프로그램만의 가상 메모리 주소표를 새로 만든다는 말 (현재 스레드가 사용할 새 유저 주소 공간 생성)
	*/
	t->pml4 = pml4_create ();

	/* 실패하면 load 실패 */
	if (t->pml4 == NULL)
		goto done;
	/* 현재 스레드의 페이지 테이블 활성화 (CPU에게 이제 이 page table 참고하라고 명령)
		 이제 CPU가 이 프로그램의 page table을 기준으로 물리 주소 <-> 가상 주소 변환
	*/
	process_activate (thread_current ());

	/* 디스크에서 실행할 프로그램 파일을 엽니다. */
	lock_acquire (&filesys_lock);
	file = filesys_open (file_name);
	lock_release (&filesys_lock);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv_tokens[0]);
		goto done;
	}

	/* ELF(Linux 계열에서 쓰는 실행 파일 형식) 헤더를 읽고, 진짜 실행 가능한 ELF 파일인지 검증
	 * ELF header는 실행 파일의 맨 앞에 있는 큰 설명서입니다.
 	 * 여기에는 "이 파일이 실행 파일이 맞는지",
 	 * "어떤 아키텍처용인지",
 	 * "program header가 어디에 몇 개 있는지" 같은 정보가 들어 있습니다.
	*/
	if (!read_file_exact_at (file, &ehdr, sizeof ehdr, 0)
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv_tokens[0]);
		goto done;
	}

	/* ELF 파일 안의 program header들을 하나씩 읽습니다.
 	 *
 	 * program header는 실행 파일 안의 각 구역을
 	 * "파일의 어느 위치에서 읽어서",
 	 * "유저 가상 메모리의 어느 주소에 올려야 하는지"
 	 * 설명하는 작은 설명서입니다.
 	 *
 	 * 모든 header가 실제로 메모리에 올라가는 것은 아니고,
 	 * PT_LOAD 타입인 header만 실제 코드/데이터 세그먼트로 메모리에 적재됩니다.
 	 */
	file_ofs = ehdr.e_phoff;

	/* 각 프로그램 헤더를 순회하며 메모리에 적재할 세그먼트를 찾는다. */
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		/* program header를 읽을 위치가 파일 범위를 벗어나면 잘못된 ELF입니다. */
		if (file_ofs < 0 || file_ofs > file_length_synchronized (file))
			goto done;

		/* program header도 file position을 움직이지 않고 offset 기준으로 읽는다. */
		if (!read_file_exact_at (file, &phdr, sizeof phdr, file_ofs))
			goto done;

		/* 다음 헤더 위치로 이동 */
		file_ofs += sizeof phdr;

		switch (phdr.p_type) {
			/* 이 타입들은 Pintos에서 실제 메모리에 올릴 필요가 없으므로 무시합니다. */
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				break;

			/* 동적 링킹 관련 세그먼트입니다.
			 * Pintos는 동적 링킹을 지원하지 않으므로 이런 ELF는 로드 실패 처리합니다.
			 */
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			/* PT_LOAD 타입 세그먼트만 실제 메모리에 올린다.
				 ELF 파일 내부에서도 실제 실행에 필요한 code/data 구역만 메모리에 올리는 것
			*/	
			case PT_LOAD:
				/* PT_LOAD는 실제 실행에 필요한 세그먼트입니다.
			 	 *
			 	 * 예를 들면:
			 	 * - 기계어 코드 영역
			 	 * - 읽기 전용 데이터 영역
			 	 * - 전역 변수 데이터 영역
			 	 *
			 	 * 이 세그먼트들은 파일에서 읽어서
			 	 * 현재 프로세스의 유저 가상 메모리에 매핑해야 합니다.
			 	 */
				if (validate_segment (&phdr, file)) {
					/* load_segment()가 파일에서 읽을 바이트와 0으로 채울 바이트를 계산 */
					bool writable = (phdr.p_flags & PF_W) != 0;

					/* 파일에서 읽기 시작할 위치를 페이지 경계에 맞춥니다. */
					uint64_t file_page = phdr.p_offset & ~PGMASK;

					/* 유저 가상 메모리에 올릴 시작 주소를 페이지 경계에 맞춥니다. */
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;

					/* 세그먼트가 페이지 내부 어디서 시작하는지 나타내는 offset입니다. */
					uint64_t page_offset = phdr.p_vaddr & PGMASK;

					/* 읽어올 바이트 수와 0으로 채울 바이트 수 */
					uint32_t read_bytes, zero_bytes;

					/* 파일에 실제 데이터가 있는 경우입니다.
				 	 *
				 	 * read_bytes:
				 	 *   실행 파일에서 읽어와야 하는 바이트 수
				 	 *
				 	 * zero_bytes:
				 	 *   파일에는 없지만 메모리에서는 0으로 채워야 하는 바이트 수
				 	 *
				 	 * 예를 들어 초기화되지 않은 전역 변수, 즉 BSS 영역은
				 	 * 파일에 실제 값이 저장되어 있지 않고, 실행 시 0으로 채워집니다.
				 	 */
					if (phdr.p_filesz > 0) {
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE) - read_bytes;
					}
					/* BSS 같은 zero-fill 세그먼트 */
					else {
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}

					/* 이 함수가 실제로 세그먼트를 유저 메모리에 올립니다.
				 	 *
				 	 * 내부적으로는 대략:
				 	 * 1. 유저 페이지를 할당하고
				 	 * 2. 실행 파일에서 read_bytes만큼 읽고
				 	 * 3. 나머지 zero_bytes만큼 0으로 채우고
				 	 * 4. 그 페이지를 mem_page부터 시작하는 유저 가상 주소에 매핑합니다.
					 *
				 	 * writable 값은 이 세그먼트가 쓰기 가능한 영역인지 결정합니다.
				 	 * 코드 영역은 보통 writable=false,
				 	 * 데이터 영역은 보통 writable=true입니다.
				 	 */
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
					{	
						goto done;
					}
						
				} else {
					goto done;
				}
				break;
		}
	}

	/* 유저 주소 공간의 맨 위쪽에 스택용 페이지 하나를 만들어준다.
	 * 내부적으로
	 * 1. 커널에서 물리 페이지 하나를 할당한다.
	 * 2. 그 페이지를 유저 가상 주소 USER_STACK - PGSIZE 위치에 매핑한다.
   * 3. if_->rsp를 USER_STACK으로 설정한다. (스택 포인터는 빈 스택의 초기 rsp 값, 스택의 가장 높은 주소 경계를 가리키게 됨)
	*/
	if (!setup_stack (if_))
		goto done;

	/* ELF 엔트리 주소(실행 파일의 시작 주소)를 if_->rip에 저장 (instruction pointer, 즉 CPU가 다음에 실행할 명령어의 주소에 저장하는 것)
		 프로그램이 처음 실행될 시작 주소를 CPU 상태에 넣어두는 것, do_iret()으로 유저 모드에 들어갈 때, CPU가 ehdr.e_entry 주소부터 실행하게 됨.
	*/
	if_->rip = ehdr.e_entry;

	/* Argument passing은 load()가 만든 첫 stack page 안에서 끝낸다.
	 * Project 3에서도 setup_stack()이 즉시 claim한 첫 stack page 위에
	 * 같은 layout을 유지하면 syscall ABI를 바꾸지 않고 이어갈 수 있다. */
	uintptr_t rsp = if_->rsp;

	/* 스택에 인자 문자열 자체를 먼저 복사해 역순으로 push */
	for (int i = argc - 1; i >= 0; i--) {
		int len = strlen(argv_tokens[i]) + 1; // 문자열 끝 \0 포함
		rsp -= len; // 문자열 길이만큼 메모리 낮은 주소로 가서 높은 주소 방향으로 문자열 복사
		if (rsp < USER_STACK - PGSIZE) {
			goto done; // rsp가 계속 작아지다가 USER_STACK - PGSIZE 보다 작아지면, 할당된 스택 page 바깥이라 잘못된 접근
		} 
		memcpy((void *) rsp, argv_tokens[i], len);
		/* 이후 argv 배열(명령어 문자열 인자들의 주소 배열)을 스택에 저장하기 위해 주소들을 arg_addr에 저장 */
		arg_addr[i] = (void *) rsp;
	}
	/* rsp는 원래 uintptr_t 타입이므로, rsp를 8로 나눈 나머지를 원래 rsp에서 빼주면 8의 배수로 내림 정렬이 된다.*/
	rsp -= (rsp % 8);
	if (rsp < USER_STACK - PGSIZE) {
		goto done;
	}
	/* NULL pointer 크기 만큼 rsp 내려감 */
	rsp -= 8;
	if (rsp < USER_STACK - PGSIZE) {
		goto done;
	}
	/* argv[argc] = NULL 
	 * uintptr_t는 주소를 담을 수 있는 정수 타입이다.
	 * 해야하는 작업은, 이 정수를 주소로 보고 -> 그 주소를 가리키는 포인터를 통해 그 주소의 내용을 NULL로 만들어주는 것
	 * 그래서 uintptr_t 로 casting이 먼저 필요, 이후 안의 내용을 역참조(*)하여 그 값에 NULL 대입
	 */
	*(uintptr_t *) rsp = 0;

	/* 스택에 각 인자 문자열 주소를 push 
	 * 이 스택 주소에는 char * 값 하나가 저장되어야한다. (실제 문자열의 주소)
	 * 그러면 rsp는 char * 타입의 데이터를 담고 있는 주소니까, char ** 타입으로 캐스팅하고, 그 안의 내용을 arg_addr[i]로 대입해주어야 하니 역참조(*) 필요
	 */
	for (int i = argc - 1; i >= 0; i--) {
		rsp -= 8;
		if (rsp < USER_STACK - PGSIZE) {
			goto done;
		}
		*(char **) rsp = arg_addr[i];
	}

	/* 현재 rsp 포인터가 argv 배열 시작 주소 */
	argv_addr = (void *) rsp;

	rsp -= 8;
	if (rsp < USER_STACK - PGSIZE) {
		goto done;
	}
	/* fake return address */
	*(uintptr_t *) rsp = 0;

	if_->R.rdi = (uint64_t) argc;
	if_->rsp = (uint64_t) rsp;
	if_->R.rsi = (uint64_t) argv_addr;

	success = true;
	running_file_install (t, file);
	file = NULL;
	
done:
	/* 로드가 성공하든 실패하든 여기로 옵니다. */
	if (arg_addr != NULL) {
		palloc_free_page (arg_addr);
	}
	if (file != NULL) {
		lock_acquire (&filesys_lock);
		file_close (file);
		lock_release (&filesys_lock);
	}
	

	/* 최종 성공 여부 반환 */
	return success;
}

/* PHDR이 FILE 안의 유효하고 로드 가능한 세그먼트를 설명하는지 확인하고, 그렇다면 true를 반환하고 그렇지 않으면 false를
 * 반환합니다. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file)
{
	/* p_offset과 p_vaddr는 같은 페이지 오프셋을 가져야 합니다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 내부를 가리켜야 합니다. */
	if (phdr->p_offset > (uint64_t) file_length_synchronized (file))
		return false;

	/* p_memsz는 p_filesz보다 적어도 커야 합니다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트는 비어 있으면 안 됩니다. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역은 시작과 끝이 모두
	   사용자 주소 공간 범위 안에 있어야 합니다. */
	if (!is_user_vaddr ((void *)phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *)(phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 이 영역은 커널 가상
	   주소 공간을 가로질러 "wrap around"해서는 안 됩니다. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 페이지 0 매핑은 허용하지 않습니다.
	   페이지 0을 매핑하는 것은 좋은 생각이 아닐 뿐만 아니라, 이를 허용하면
	   user code가 system calls에 null pointer를 전달했을 때
	   memcpy() 등의 null pointer assertion 때문에 커널이 쉽게 panic할 수 있습니다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 괜찮습니다. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 project 2 동안에만 사용됩니다.
 * 함수를 project 2 전체에 대해 구현하고 싶다면 #ifndef 매크로 밖에 구현하세요. */

/* load() 헬퍼. */
static bool 
install_page (void *upage, void *kpage, bool writable);

/* FILE의 오프셋 OFS에서 시작해 주소 UPAGE에 위치한 세그먼트를 로드합니다.  전체적으로 READ_BYTES +
 * ZERO_BYTES 바이트의 가상
 * 메모리가 다음과 같이 초기화됩니다:
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE에서 오프셋 OFS부터 읽어야 합니다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 초기화해야 합니다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가 수정할 수 있어야 하고, 그렇지 않으면 읽기 전용이어야
 * 합니다.
 *
 * 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를, 성공하면 true를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
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

		/* 메모리 페이지를 가져옵니다. 사용자 프로그램용 물리 메모리 1페이지를 가져온다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지를 로드합니다. 실행 파일 내용을 일단 커널이 접근 가능한 메모리에 읽어옴  */
		if (!read_file_exact_at (file, kpage, page_read_bytes, ofs)) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 페이지를 프로세스의 주소 공간에 추가합니다. 
			 사용자 주소 upage가 실제 메모리 kpage를 가리키게 주소표에 등록
		*/
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음으로 진행합니다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* USER_STACK에 0으로 초기화된 페이지를 매핑하여 최소 스택을 만듭니다. */
static bool
setup_stack (struct intr_frame *if_)
{
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL)
	{
		success = install_page (((uint8_t *)USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 사용자 가상 주소 UPAGE를 커널
 * 가상 주소 KPAGE에 매핑하는 항목을 페이지 테이블에 추가합니다.
 * WRITABLE이 true이면 사용자 프로세스가 페이지를 수정할 수 있고,
 * 그렇지 않으면 읽기 전용입니다.
 * UPAGE는 이미 매핑되어 있으면 안 됩니다.
 * KPAGE는 아마도 palloc_get_page()로 사용자 풀에서 얻은 페이지여야 합니다.
 * 성공하면 true를, UPAGE가 이미 매핑되어 있거나
 * 메모리 할당에 실패하면 false를 반환합니다. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
	struct thread *t = thread_current ();

	/* 해당 가상 주소에 이미 페이지가 없는지 확인한 다음, 그곳에 페이지를 매핑합니다. */
	return (pml4_get_page (t->pml4, upage) == NULL && pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기부터의 코드는 project 3 이후에 사용됩니다.
 * 함수를 project 2에만 구현하고 싶다면 위쪽 블록에 구현하세요. */

/* page fault가 일어났을때 frame을 어떻게 채울지 */
struct lazy_load_segment_aux {
	struct file *file; // 어느 실행 파일을 읽을지
	off_t ofs; // 실행 파일의 어느 위치부터(숫자값) 읽을지
	uint32_t read_bytes; // 이 page file에서 몇바이트 읽을지
	uint32_t zero_bytes; // 채워지지 않은 만큼 0으로 채워놓음
};

static bool
lazy_load_segment (struct page *page, void *aux)
{
	if (aux == NULL) {
		return false;
	}
	struct file *file = ((struct lazy_load_segment_aux *) aux)->file;
	off_t ofs = ((struct lazy_load_segment_aux *) aux)->ofs;
	uint32_t read_bytes = ((struct lazy_load_segment_aux *) aux)->read_bytes;
	uint32_t zero_bytes = ((struct lazy_load_segment_aux *) aux)->zero_bytes;
	free (aux);

	void* kpage = page->frame->kva;
	
	if (!read_file_exact_at (file, kpage, read_bytes, ofs)) {
		return false;
	}
	memset (kpage + read_bytes, 0, zero_bytes);

	return true;
}

/* FILE의 오프셋 OFS에서 시작해 주소 UPAGE에 위치한 세그먼트를 로드합니다.  전체적으로 READ_BYTES +
 * ZERO_BYTES 바이트의 가상
 * 메모리가 다음과 같이 초기화됩니다:
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE에서 오프셋 OFS부터 읽어야 합니다.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 초기화해야 합니다.
 *
 * 이 함수가 초기화한 페이지는 WRITABLE이 true이면 사용자 프로세스가 수정할 수 있어야 하고, 그렇지 않으면 읽기 전용이어야
 * 합니다.
 *
 * 메모리 할당 오류나 디스크 읽기 오류가 발생하면 false를, 성공하면 true를 반환합니다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
			 uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
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
		struct lazy_load_segment_aux *aux = malloc (sizeof (struct lazy_load_segment_aux));

		if (aux == NULL) {
			return false;
		}
		
		aux->file = file;
		aux->ofs = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;
		

		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
											writable, lazy_load_segment, aux)) 
		{
			free (aux);
			return false;
		}
			

		/* 다음으로 진행합니다. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
		ofs += page_read_bytes;
	}
	return true;
}

/* USER_STACK에 스택용 PAGE를 생성합니다. 성공하면 true를 반환합니다. */
static bool
setup_stack (struct intr_frame *if_)
{
	bool success = false;
	void *stack_bottom = (void *)(((uint8_t *)USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택을 매핑하고 페이지를 즉시 claim하세요.
	 * TODO: 성공하면 rsp를 그에 맞게 설정하세요.
	 * TODO: 페이지가 stack임을 표시해야 합니다. */
	/* TODO: 여기에 코드를 작성하세요 */

	vm_alloc_page (VM_ANON | VM_MARKER_0, stack_bottom, true);
	vm_claim_page (stack_bottom);
	
	struct page *page = spt_find_page (&thread_current ()->spt, stack_bottom);
	anon_initializer (page, VM_ANON | VM_MARKER_0, page->frame->kva); // TODO. 이거 넣는게 맞나??
	if_->rsp = USER_STACK;
	success = true;
	return success;
}
#endif /* 가상 메모리(VM) */
