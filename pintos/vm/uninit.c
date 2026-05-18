/* uninit.c: 초기화되지 않은 페이지의 구현.
 *
 * 모든 페이지는 uninit page로 태어난다. 첫 page fault가 발생하면,
 * handler chain은 uninit_initialize (page->operations.swap_in)를 호출한다.
 * uninit_initialize 함수는 page object를 초기화하여 페이지를 특정 page
 * object(anon, file, page_cache)로 변환하고, vm_alloc_page_with_initializer
 * 함수에서 전달된 initialization callback을 호출한다. */

#include "vm/vm.h"
#include "vm/uninit.h"
#include "debug_log.h"
#include "userprog/process.h"

static bool uninit_initialize (struct page *page, void *kva);
static void uninit_destroy (struct page *page);

/* 이 struct를 수정하지 마십시오 */
static const struct page_operations uninit_ops = {
	.swap_in = uninit_initialize,
	.swap_out = NULL,
	.destroy = uninit_destroy,
	.type = VM_UNINIT,
};

/* 이 function을 수정하지 마십시오 */
void
uninit_new (struct page *page, void *va, vm_initializer *init,
		enum vm_type type, void *aux,
		bool (*initializer)(struct page *, enum vm_type, void *)) {
	ASSERT (page != NULL);

	*page = (struct page) {
		.operations = &uninit_ops,
		.va = va,
		.frame = NULL, /* 지금은 frame이 없다 */
		.uninit = (struct uninit_page) {
			.init = init,
			.type = type,
			.aux = aux,
			.page_initializer = initializer,
		}
	};
}

/* 첫 page fault에서 페이지를 초기화한다 */
static bool
uninit_initialize (struct page *page, void *kva) {
	struct uninit_page *uninit = &page->uninit;

	/* 먼저 가져와라. page_initialize가 값을 덮어쓸 수 있다 */
	vm_initializer *init = uninit->init;
	void *aux = uninit->aux;
	
	/* TODO: 이 함수를 수정해야 할 수도 있다. */
	return uninit->page_initializer (page, uninit->type, kva) &&
		(init ? init (page, aux) : true);
}

/* uninit_page가 보유한 자원을 해제한다. 대부분의 페이지는 다른 page objects로 변환되지만, process exit 시
 * 실행 중 한 번도 참조되지 않은 uninit page가 남아 있을 수 있다. PAGE는 호출자가 해제한다. */
static void
uninit_destroy (struct page *page) {
	struct uninit_page *uninit UNUSED = &page->uninit;
	/* TODO: 이 함수를 채워라.
	 * TODO: 할 일이 없다면 그냥 반환하라. */
}
