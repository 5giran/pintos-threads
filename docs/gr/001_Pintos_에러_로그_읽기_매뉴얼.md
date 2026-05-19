# Pintos 에러 로그 읽기 매뉴얼

이 문서는 Pintos 테스트나 빌드가 실패했을 때 로그를 어떤 순서로 읽고, 무엇을 결론으로 삼아야 하는지 정리한 매뉴얼이다. 구현 코드를 대신 제시하지 않고, 현재 저장소에 남아 있는 빌드/테스트 산출물과 로컬 테스트 스크립트를 기준으로 로그 해석 절차만 다룬다.

근거로 삼은 로컬 파일:

- `pintos/vm/build/tests/vm/mmap-zero-len.errors`
- `pintos/vm/build/tests/vm/mmap-zero-len.result`
- `pintos/vm/build/tests/vm/page-merge-seq.output`
- `pintos/vm/build/tests/vm/page-merge-seq.result`
- `pintos/threads/build/tests/threads/mlfqs/mlfqs-load-1.output`
- `pintos/threads/build/tests/threads/mlfqs/mlfqs-load-1.result`
- `pintos/userprog/build/tests/userprog/no-vm/multi-oom.output`
- `pintos/userprog/build/tests/userprog/no-vm/multi-oom.result`
- `pintos/tests/tests.pm`
- `pintos/tests/vm/page-merge-seq.ck`
- `pintos/tests/userprog/no-vm/multi-oom.ck`

## 1. 로그 파일 종류부터 구분하기

Pintos 테스트 산출물은 보통 테스트별로 아래 파일에 나뉘어 남는다.

| 파일 | 먼저 볼지 | 의미 |
| --- | ---: | --- |
| `<test>.result` | 1 | 채점기가 최종적으로 `PASS`, `FAIL`, `BUILD_ERROR` 등으로 판정한 결과 |
| `<test>.errors` | 2 | 빌드 실패, stderr, panic 해석, backtrace 같은 진단 정보 |
| `<test>.output` | 3 | QEMU 부팅부터 테스트 프로그램 출력, page fault, exit, power off까지의 전체 실행 출력 |
| `<test>.ck` | 4 | 테스트가 기대하는 출력과 비교 옵션 |
| `<test>.c` | 5 | 테스트 프로그램이 무엇을 시도하는지 확인하는 파일 |

집계 파일인 `build/results`가 있으면 가장 먼저 봐도 된다. 다만 현재 작업 트리에서는 `pintos/vm/build/results`, `pintos/userprog/build/results`, `pintos/threads/build/results`가 없고 테스트별 `.result` 파일만 남아 있었다. 이럴 때는 `.result` 파일들을 모아서 실패 종류를 먼저 나눈다.

예:

```text
pintos/vm/build/tests/vm/mmap-zero-len.result
BUILD_ERROR

pintos/vm/build/tests/vm/page-merge-seq.result
FAIL
run: child[0] wait fail: FAILED

pintos/threads/build/tests/threads/mlfqs/mlfqs-load-1.result
FAIL
Kernel panic in run: PANIC at ../../tests/threads/tests.c:93 in fail(): test failed
```

여기서 이미 세 종류를 구분할 수 있다.

- `BUILD_ERROR`: 커널/테스트를 실행하기 전에 컴파일에서 막힘.
- `FAIL`: 실행은 됐지만 출력이나 테스트 조건이 기대와 다름.
- `Kernel panic`: 테스트 내부의 `fail()`이나 커널 assertion/panic으로 중단됨.

## 2. 컴파일 에러 로그 읽기

컴파일 에러는 `.errors` 파일에서 읽는다. 현재 예시는 `pintos/vm/build/tests/vm/mmap-zero-len.errors`에 남아 있다.

읽는 순서:

1. 맨 위의 `gcc -c ...` 줄에서 어떤 파일을 컴파일했는지 본다.
2. `error:`를 먼저 찾는다.
3. 각 `error:` 바로 위의 `warning:` 또는 바로 아래의 `note:`를 같이 읽는다.
4. 마지막 `make: *** ... Error 1`은 결과 알림일 뿐, 원인은 그 위의 `error:`다.

현재 로그의 핵심 흐름:

```text
gcc -c ../../userprog/syscall.c -o userprog/syscall.o ...
../../userprog/syscall.c:133:36: warning: implicit declaration of function 'sys_mmap'
../../userprog/syscall.c:563:1: error: conflicting types for 'sys_mmap'
../../userprog/syscall.c:572:69: error: incompatible type for argument 1 of 'spt_find_page'
make: *** [../../Make.config:38: userprog/syscall.o] Error 1
```

이 로그에서 결론은 두 단계로 나눠야 한다.

첫 번째 에러:

- 133번째 줄에서 `sys_mmap`을 호출할 때 컴파일러가 함수 선언을 못 봤다.
- 그래서 C 컴파일러가 임시로 `int sys_mmap()`처럼 추정했다.
- 563번째 줄에서 실제 정의를 보니 반환형이 `void *`라서 타입이 충돌했다.
- 따라서 이 에러의 핵심은 "사용 지점보다 앞에서 함수 원형이 보이는가"이다.

두 번째 에러:

- `spt_find_page`의 첫 번째 인자는 `struct supplemental_page_table *`를 기대한다.
- 실제 호출에서는 `struct supplemental_page_table` 값 자체가 전달되었다.
- `note:`는 "expected ... but argument is ..." 형식으로 기대 타입과 실제 타입을 알려준다.
- 따라서 이 에러의 핵심은 "구조체 값과 구조체 포인터를 혼동했는가"이다.

경고도 버리지 않는다. 예를 들어 `discarded-qualifiers`는 `const` 포인터를 수정 가능한 포인터처럼 넘기고 있다는 뜻이고, `comparison of distinct pointer types`는 서로 다른 포인터 타입끼리 비교/산술 연산을 하고 있다는 뜻이다. 당장 빌드를 멈추지 않아도 이후 런타임 버그의 단서가 될 수 있다.

## 3. 런타임 FAIL 로그 읽기

`FAIL`은 컴파일은 됐고 QEMU에서 테스트도 실행됐지만, 테스트가 기대한 출력이나 조건을 만족하지 못했다는 뜻이다.

예시는 `pintos/vm/build/tests/vm/page-merge-seq.result`다.

```text
FAIL
run: child[0] wait fail: FAILED
```

이 줄만 보면 원인이 부족하다. 같은 테스트의 `.output`을 열어서 실제 실행 흐름을 본다.

```text
Executing 'page-merge-seq':
(page-merge-seq) begin
(page-merge-seq) init
(page-merge-seq) sort chunk 0
Page fault at 0x403d01: not present error reading page in user context.
child-sort: exit(-1)
(page-merge-seq) child[0] wait fail: FAILED
page-merge-seq: exit(1)
```

이 경우 읽는 순서는 아래와 같다.

1. `Executing 'page-merge-seq':` 이후부터 테스트 본문이다.
2. 마지막 실패 메시지 `child[0] wait fail`만 보지 말고, 그 직전의 첫 이상 징후를 찾는다.
3. 첫 이상 징후는 `Page fault at 0x403d01: not present error reading page in user context.`다.
4. 그 결과 child process인 `child-sort`가 `exit(-1)`로 죽었고, parent가 `wait fail`을 출력했다.

즉 이 로그에서 바로 단정할 수 있는 것은 "부모의 wait 로직이 반드시 원인"이 아니라, "자식 `child-sort`가 user context page fault로 죽었고, 그 여파로 parent의 wait 체크가 실패했다"이다.

기대 출력은 `.ck` 파일에서 확인한다. `pintos/tests/vm/page-merge-seq.ck`는 `child[0] wait success`부터 `child[15] wait success`, `merge`, `verify`, `success`를 기대한다. 따라서 현재 출력은 첫 번째 child 실행 단계에서 이미 기대 경로를 벗어났다.

## 4. Kernel PANIC 로그 읽기

`Kernel PANIC`이 있으면 `.result`와 `.output`을 둘 다 본다.

예시는 `pintos/threads/build/tests/threads/mlfqs/mlfqs-load-1.result`다.

```text
FAIL
Kernel panic in run: PANIC at ../../tests/threads/tests.c:93 in fail(): test failed
Call stack: ...
Translation of call stack:
0x0000008004214308: debug_panic (lib/kernel/debug.c:31)
0x0000008004217cf2: pass (tests/threads/tests.c:99)
0x000000800421b0c0: test_mlfqs_load_1 (tests/threads/mlfqs/mlfqs-load-1.c:31)
...
```

이 panic은 커널 내부 자료구조가 무조건 깨졌다는 뜻으로 바로 단정하면 안 된다. `tests.c`의 `fail()`이 호출되어도 Pintos 테스트 프레임워크는 panic 형식으로 종료할 수 있다.

같은 테스트의 `.output`에는 더 직접적인 실패 이유가 있다.

```text
(mlfqs-load-1) FAIL: load average stayed below 0.5 for more than 45 seconds
Kernel PANIC at ../../tests/threads/tests.c:93 in fail(): test failed
```

따라서 결론은 "임의의 kernel panic"이 아니라 "테스트가 load average 조건을 만족하지 못했고, 테스트 프레임워크의 `fail()`이 panic을 발생시켰다"이다.

Panic 로그에서 봐야 할 것:

- `PANIC at <file>:<line> in <function>()`: 누가 panic을 냈는가.
- `Call stack`: 주소 목록. `.result`에는 `backtrace` 번역이 같이 들어올 수 있다.
- `Translation of call stack`: 사람이 읽을 수 있는 함수명과 파일/라인.
- `.output`의 panic 직전 테스트 메시지: 실제 실패 조건.

## 5. Page fault 로그 읽기

Page fault 줄은 보통 아래 형식이다.

```text
Page fault at <addr>: <present/not present or rights violation> error <reading/writing> page in <user/kernel> context.
```

각 부분의 의미:

| 부분 | 예 | 읽는 법 |
| --- | --- | --- |
| fault address | `0x403d01`, `0`, `0x8004000000` | 접근하려던 주소 |
| fault 종류 | `not present` | 매핑되지 않은 페이지 접근 |
| 권한 종류 | `rights violation` | 페이지는 있지만 권한이 맞지 않는 접근 |
| 접근 방향 | `reading`, `writing` | 읽다가 죽었는지 쓰다가 죽었는지 |
| context | `user context`, `kernel context` | 유저 코드 실행 중인지 커널 코드 실행 중인지 |

예를 들어 `page-merge-seq.output`의 줄은:

```text
Page fault at 0x403d01: not present error reading page in user context.
```

읽기:

- 주소 `0x403d01`을 읽으려 했다.
- 해당 페이지가 present가 아니었다.
- fault는 user context에서 났다.
- 이어서 `child-sort: exit(-1)`가 나오므로, 이 fault 때문에 child가 비정상 종료한 것으로 볼 수 있다.

반대로 `pt-write-code.output`에 남아 있는 형식은:

```text
Page fault at 0x400000: rights violation error writing page in user context.
```

읽기:

- 주소 `0x400000`에 쓰려고 했다.
- 페이지가 아예 없었다기보다 권한 위반이다.
- 코드/읽기 전용 영역에 write를 시도하는 테스트라면 기대되는 fault일 수도 있다.

중요한 점은 Page fault가 항상 실패 원인은 아니라는 것이다. `pintos/tests/userprog/no-vm/multi-oom.ck`처럼 `IGNORE_USER_FAULTS` 옵션이 있는 테스트는 user fault 메시지를 비교에서 제외한다. 따라서 Page fault 줄이 많아도 `.result`와 `.ck`의 옵션을 같이 봐야 한다.

## 6. `.ck` 파일로 기대값 확인하기

Pintos 테스트는 `.ck` 파일에서 기대 출력을 비교한다. `pintos/tests/tests.pm`의 `check_expected`는 `.output`을 읽고, 공통 체크를 통과한 뒤 기대 출력과 비교한다.

중요한 공통 체크:

- 출력이 아예 없으면 실패.
- `PANIC`, `FAIL`, `TIMEOUT` 키워드가 있으면 실패 처리.
- `Pintos booting with`, `Boot complete`, `Timer: ... ticks`, `Powering off`가 없으면 정상 부팅/종료가 아니라고 본다.

중요한 비교 옵션:

- `IGNORE_EXIT_CODES`: `program: exit(n)` 줄은 비교에서 제외한다.
- `IGNORE_USER_FAULTS`: user context page fault 관련 줄은 비교에서 제외한다.

예를 들어 `pintos/tests/userprog/no-vm/multi-oom.ck`는 다음을 기대한다.

```text
(multi-oom) begin
(multi-oom) Spawned at least 10 children.
(multi-oom) success. Program forked 10 iterations.
(multi-oom) end
```

그리고 `IGNORE_USER_FAULTS`가 켜져 있다. 그러므로 `multi-oom.output`에 page fault가 많다는 사실만으로 실패를 판단하면 안 된다. 실제 `.result`는 다음처럼 나온다.

```text
FAIL
run: Should return > 0.: FAILED
```

이 경우 우선 볼 것은 "무수한 page fault" 자체가 아니라, 테스트 본문에서 출력한 `Should return > 0.: FAILED`와 기대 출력에서 빠진 `Spawned at least 10 children`, `success` 줄이다.

## 7. 한 번에 읽는 절차

실패 로그를 받을 때마다 아래 순서로 읽는다.

1. `.result` 첫 줄을 본다.
   - `BUILD_ERROR`면 `.errors`로 간다.
   - `FAIL`이면 `.result`의 두 번째 줄과 `.output`을 같이 본다.
   - `PASS`면 해당 테스트는 통과다. `.output`에 page fault가 있어도 테스트가 허용했을 수 있다.
2. `BUILD_ERROR`일 때:
   - 첫 `error:`를 찾는다.
   - 그 직전 `warning:`과 직후 `note:`를 같이 읽는다.
   - 마지막 `make: ***`는 원인이 아니라 결과로 본다.
3. `FAIL`일 때:
   - `.output`에서 `Executing '<test>':` 이후를 본다.
   - 실패 메시지보다 앞에 있는 첫 이상 징후를 찾는다.
   - `.ck` 파일에서 기대 출력과 비교 옵션을 확인한다.
4. `Kernel PANIC`일 때:
   - panic 위치가 테스트 프레임워크인지 커널 코드인지 구분한다.
   - `.output`에서 panic 직전의 테스트 메시지를 본다.
   - backtrace는 "어디서 죽었는지"이고, 원인은 그보다 앞선 조건 실패일 수 있다.
5. Page fault가 보일 때:
   - 주소, not present/rights violation, read/write, user/kernel context를 분해한다.
   - 그 fault가 테스트에서 기대하는 fault인지 `.ck`와 테스트 `.c`로 확인한다.

## 8. 현재 로그에서 바로 보이는 분류 예시

| 테스트 | 결과 | 먼저 볼 파일 | 현재 로그에서 읽히는 1차 결론 |
| --- | --- | --- | --- |
| `mmap-zero-len` | `BUILD_ERROR` | `.errors` | `syscall.c` 컴파일 중 `sys_mmap` 선언/정의 충돌과 `spt_find_page` 인자 타입 오류 |
| `page-merge-seq` | `FAIL` | `.output`, `.ck` | `child-sort`가 user context page fault로 `exit(-1)`했고 parent가 첫 child wait에서 실패 |
| `mlfqs-load-1` | `FAIL` + panic | `.output`, `.result` | load average 조건 실패로 테스트 프레임워크 `fail()`이 panic 발생 |
| `multi-oom` | `FAIL` | `.result`, `.output`, `.ck` | user fault는 일부 무시 대상이고, 핵심은 `Should return > 0` 조건 실패와 기대 성공 메시지 누락 |

## 9. 조사 기록에 남길 최소 정보

실패를 팀원에게 공유할 때는 아래만 남겨도 다음 사람이 이어서 읽기 쉽다.

```text
테스트:
결과 파일:
결과 첫 줄:
핵심 실패 메시지:
첫 이상 징후:
기대 출력 파일:
비교 옵션:
현재 판단:
아직 확인 안 한 것:
```

예:

```text
테스트: tests/vm/page-merge-seq
결과 파일: pintos/vm/build/tests/vm/page-merge-seq.result
결과 첫 줄: FAIL
핵심 실패 메시지: run: child[0] wait fail: FAILED
첫 이상 징후: Page fault at 0x403d01: not present error reading page in user context.
기대 출력 파일: pintos/tests/vm/page-merge-seq.ck
비교 옵션: IGNORE_EXIT_CODES
현재 판단: 첫 child인 child-sort가 page fault로 죽어 parent의 wait success 기대 출력에 도달하지 못했다.
아직 확인 안 한 것: child-sort가 접근한 주소가 lazy loading, stack growth, mmap/file-backed page 중 어느 경로와 관련되는지.
```

이 정도로 남기면 "무엇이 실패했는가"와 "무엇을 아직 모르는가"가 분리된다. Pintos 로그는 길어도 대부분은 부팅/디스크/종료 소음이고, 실제 단서는 `.result` 첫 줄, `.output`의 첫 이상 징후, `.ck`의 기대 출력 세 군데에 모여 있다.
