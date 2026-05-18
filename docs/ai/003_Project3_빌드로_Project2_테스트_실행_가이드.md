# Project 3 빌드로 Project 2 테스트 실행 가이드

## 목적

Project 3 VM 구현이 들어간 커널, 즉 `pintos/vm` 빌드 결과로 Project 2 테스트 묶음을 돌리는 방법을 정리한다.

핵심은 `pintos/userprog`에서 실행하지 않고 `pintos/vm`에서 실행하되, 테스트 목록만 Project 2 기준으로 덮어쓰는 것이다.

## 기준 파일

- `pintos/vm/Make.vars`
  - Project 3 빌드는 `KERNEL_SUBDIRS`에 `vm`을 포함한다.
  - 기본 `TEST_SUBDIRS`에는 `tests/vm`이 들어 있어 그냥 `make check`를 하면 Project 3 테스트까지 같이 돈다.
- `pintos/userprog/Make.vars`
  - Project 2 기본 테스트 목록은 `tests/userprog tests/filesys/base tests/userprog/no-vm tests/threads`이다.
- `pintos/Makefile.kernel`
  - 상위 디렉터리에서 `make check`를 실행하면 `build/` 디렉터리를 준비한 뒤 `build` 안에서 실제 테스트를 실행한다.
- `pintos/tests/Make.tests`
  - `TEST_SUBDIRS`에 들어간 테스트 디렉터리의 `Make.tests`만 포함해서 테스트 목록을 만든다.

## 전체 Project 2 테스트 실행

저장소 바깥 루트가 `/Users/sisu/Projects/jungle/pintos`일 때:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make check TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/threads"
```

이 명령은 다음 성질을 가진다.

- 빌드 위치는 `pintos/vm`이므로 `vm/Make.vars`의 Project 3 커널 구성을 사용한다.
- `KERNEL_SUBDIRS`에는 `vm`이 포함된 상태로 유지된다.
- `TEST_SUBDIRS`만 Project 2의 기본 테스트 목록으로 바뀐다.
- 결과는 `pintos/vm/build/results`에 요약된다.
- 각 테스트별 출력은 `pintos/vm/build/tests/.../*.output`, `*.errors`, `*.result`에 생긴다.

## 단일 테스트만 실행

예를 들어 `args-none` 하나만 Project 3 빌드로 확인하려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make build/tests/userprog/args-none.result \
  TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/threads"
```

`no-vm`의 `multi-oom`만 확인하려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make build/tests/userprog/no-vm/multi-oom.result \
  TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/threads"
```

상위 `pintos/vm`에서 `build/...` 형태로 호출하는 이유는 `Makefile.kernel`이 필요한 `build/tests/...` 디렉터리를 먼저 만들어 주기 때문이다. `pintos/vm/build`에 직접 들어가서 실행하면, 처음 실행하는 테스트 하위 디렉터리가 없어서 실패할 수 있다.

## fork 제외 Project 2 테스트만 실행

이 저장소에는 Project 3 빌드에서 Project 2 기본 테스트 묶음을 돌리되, `fork()` syscall에 직접 의존하는 테스트를 제외하는 편의 타깃을 추가했다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-nofork-check
```

결과 파일만 갱신하고 요약 파일을 만들고 싶으면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-nofork-results
```

요약은 `pintos/vm/build/selected-results`에 생긴다.

파일 시스템 base 테스트까지 섞지 않고 userprog 중심으로만 보고 싶으면 다음 타깃을 쓴다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-user-nofork-check
```

이 타깃은 `tests/userprog`와 `tests/threads`만 포함하고, `tests/filesys/base`와 `tests/userprog/no-vm`은 제외한다.

제외되는 테스트는 다음 항목이다.

```text
tests/userprog/fork-once
tests/userprog/fork-multiple
tests/userprog/fork-recursive
tests/userprog/fork-read
tests/userprog/fork-close
tests/userprog/fork-boundary
tests/userprog/wait-simple
tests/userprog/wait-twice
tests/userprog/wait-killed
tests/userprog/exec-boundary
tests/userprog/exec-read
tests/userprog/multi-recurse
tests/userprog/multi-child-fd
tests/userprog/rox-child
tests/userprog/rox-multichild
tests/userprog/no-vm/multi-oom
```

`wait-bad-pid`는 로컬 테스트 파일 기준으로 `fork()`를 호출하지 않고 잘못된 pid에 대한 `wait()`만 확인하므로 제외하지 않았다.

## fork 관련 Project 2 테스트만 실행

Project 3 VM 빌드 결과로 `fork()`와 그에 강하게 의존하는 Project 2 테스트만 실행하려면 다음 타깃을 쓴다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-check
```

이 타깃은 `pintos/vm` 빌드, 즉 Project 3 커널 구성으로 실행하되 테스트 목록만 fork 관련 Project 2 테스트로 제한한다. 실행 후 요약은 터미널에 출력되고, 선택 테스트 요약 파일은 `pintos/vm/build/selected-results`에 생긴다.

결과 파일만 갱신하고 `selected-results`를 만들려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-results
```

포함되는 테스트 목록은 `pintos/vm/Makefile`의 `P2_FORK_DEPENDENT_TESTS` 기준이다.

```text
tests/userprog/fork-once
tests/userprog/fork-multiple
tests/userprog/fork-recursive
tests/userprog/fork-read
tests/userprog/fork-close
tests/userprog/fork-boundary
tests/userprog/wait-simple
tests/userprog/wait-twice
tests/userprog/wait-killed
tests/userprog/exec-boundary
tests/userprog/exec-read
tests/userprog/multi-recurse
tests/userprog/multi-child-fd
tests/userprog/rox-child
tests/userprog/rox-multichild
tests/userprog/no-vm/multi-oom
```

## fork 관련 테스트 하나만 실행

기본 단일 실행 대상은 `tests/userprog/fork-once`다. 테스트 이름은 짧은 이름으로 넘길 수 있다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-one
```

다른 테스트 하나만 실행하려면 `P2_FORK_TEST`에 테스트 이름을 넘긴다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-one P2_FORK_TEST=fork-read
```

전체 경로로 지정해도 된다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-one P2_FORK_TEST=tests/userprog/no-vm/multi-oom
```

주의: `fork-one`이라는 테스트는 없다. 기본 fork 단일 테스트 이름은 `fork-once`다.

짧은 이름으로 지정할 수 있는 fork 관련 테스트는 다음과 같다.

```text
fork-once
fork-multiple
fork-recursive
fork-read
fork-close
fork-boundary
wait-simple
wait-twice
wait-killed
exec-boundary
exec-read
multi-recurse
multi-child-fd
rox-child
rox-multichild
multi-oom
```

## fork 관련 테스트 result 보기

단일 테스트의 `.result` 파일만 확인하려면 다음 타깃을 쓴다. `.result`가 없거나 오래되었으면 필요한 `.output`을 먼저 만든 뒤 결과를 출력한다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-one-result P2_FORK_TEST=fork-once
```

직접 파일을 열어도 된다.

```bash
cat /workspaces/pintos/pintos/vm/build/tests/userprog/fork-once.result
cat /workspaces/pintos/pintos/vm/build/tests/userprog/fork-once.output

```

전체 fork 관련 테스트의 pass/FAIL 요약은 다음 파일에서 본다.

```bash
cat build/selected-results
```

## fork 관련 테스트 output 보기

단일 테스트의 실제 실행 로그인 `.output`을 확인하려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-fork-one-output P2_FORK_TEST=fork-read

make p2-fork-one-output P2_FORK_TEST=fork-read DEBUG_LOG=1
```

직접 파일을 열어도 된다.

```bash
cat build/tests/userprog/fork-once.output
cat build/tests/userprog/fork-read.output
```

에러 스트림은 별도 `.errors` 파일에 기록된다.

```bash
cat build/tests/userprog/fork-read.errors
```

Docker/devcontainer 안에서 작업 중이면 `/Users/sisu/...` 같은 호스트 macOS 절대경로는 사용할 수 없다. 컨테이너 프롬프트가 `/workspaces/pintos/pintos/vm`라면 항상 위처럼 `build/...` 상대 경로를 쓰거나, 컨테이너 기준 절대경로인 `/workspaces/pintos/pintos/vm/build/...`를 쓴다.

## exec 기본 테스트만 실행

`exec-once`, `exec-arg` 두 테스트만 Project 3 빌드로 확인하려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-exec-basic-check
```

결과 파일만 갱신하고 요약을 만들려면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make p2-exec-basic-results
```

요약은 `pintos/vm/build/selected-results`에 생긴다.

## Project 3 PT/stack growth 테스트 실행

Project 3의 page table 및 stack growth 관련 PT 테스트를 확인하려면 `pintos/vm`에서 편의 타깃이나 개별 `.result` 타깃을 실행한다.

가장 권장하는 방식은 `pintos/vm/Makefile`에 추가된 편의 타깃을 쓰는 것이다. 이 타깃은 `build/` 준비, 하위 디렉터리 생성, `TEST_SUBDIRS="tests/vm tests/threads"`, `FSDISK=10` 전달을 한 번에 처리한다. 또한 `tests/vm/Make.tests`에서 실행 대상 바이너리를 `PUTFILES`로 자동 등록하므로, 개별 명령에 `PUTFILES=...`를 직접 붙이지 않아도 된다.

테스트 하나만 실행:

```bash
cd /workspaces/pintos/pintos/vm
make p3-stack-one P3_STACK_TEST=pt-grow-stack
```

단일 테스트의 `.result`만 만들고 바로 출력:

```bash
cd /workspaces/pintos/pintos/vm
make p3-stack-one-result P3_STACK_TEST=pt-grow-stack
```

단일 테스트의 `.output`만 만들고 바로 출력:

```bash
cd /workspaces/pintos/pintos/vm
make p3-stack-one-output P3_STACK_TEST=pt-grow-stack
```

PT 8개를 한 번에 실행:

```bash
cd /workspaces/pintos/pintos/vm
make p3-pt-check
```

결과 파일만 갱신하고 요약 파일을 만들려면:

```bash
make p3-pt-results
cat build/selected-results
```

기존 이름인 `p3-stack-growth-check`, `p3-stack-growth-results`도 남아 있지만, 이제는 4개가 아니라 아래 PT 8개 전체를 실행하는 alias다.

이 타깃은 내부적으로 `TEST_SUBDIRS="tests/vm tests/threads"`를 사용한다. `tests/threads`는 실행하려는 테스트 묶음이 아니라 커널 링크에 필요한 `run_test` 정의를 포함시키기 위한 빌드 의존성이다. 빼면 `undefined reference to run_test` 링크 에러가 날 수 있다.

또한 내부 make에 `FSDISK=10`을 명시적으로 넘긴다. 이 값이 빠지면 실행 명령이 `--fs-disk=`처럼 비어 파일 시스템 초기화 단계에서 `hd0:1 (hdb) not present` panic이 날 수 있다.

포함되는 PT 테스트는 다음 여덟 개다.

```text
tests/vm/pt-grow-stack
tests/vm/pt-grow-bad
tests/vm/pt-big-stk-obj
tests/vm/pt-bad-addr
tests/vm/pt-bad-read
tests/vm/pt-write-code
tests/vm/pt-write-code2
tests/vm/pt-grow-stk-sc
```

테스트 하나만 실행하려면:

```bash
make p3-stack-one P3_STACK_TEST=pt-grow-stack
make p3-stack-one-result P3_STACK_TEST=pt-grow-stack
make p3-stack-one-output P3_STACK_TEST=pt-grow-stack
```

이미 `.result`가 있어서 다시 실행되지 않으면 해당 테스트 산출물을 지운 뒤 다시 실행한다.

```bash
rm -f build/tests/vm/pt-grow-stack.output \
      build/tests/vm/pt-grow-stack.errors \
      build/tests/vm/pt-grow-stack.result

make p3-stack-one-result P3_STACK_TEST=pt-grow-stack
```

커널 링크 단계에서 `_start_bss`, `_end_bss`, `start`, `_end_kernel_text`, `_end` 같은 심벌을 찾지 못하거나 relocation overflow가 나면 테스트 실패라기보다 빌드 산출물 또는 링크 스크립트 생성물이 꼬인 상태일 가능성이 높다. 이때는 한 번 깨끗하게 지우고 다시 실행한다.

```bash
cd /workspaces/pintos/pintos/vm
make clean
make p3-stack-one P3_STACK_TEST=pt-grow-stack
```

결과 확인:

```bash
cat build/tests/vm/pt-grow-stack.result
cat build/tests/vm/pt-grow-stack.output
cat build/tests/vm/pt-grow-stack.errors
```

stack 인접 회귀 테스트까지 넓혀 보고 싶으면 다음도 추가로 실행한다. `page-merge-stk`와 `mmap-over-stk`는 stack growth 자체만 보는 테스트라기보다는 stack 영역과 다른 VM 기능이 함께 깨지지 않는지 보는 쪽에 가깝다.

```bash
make p3-stack-growth-regression-results
cat build/selected-results
```

## Project 3 mmap 테스트 실행

Project 3의 mmap 관련 테스트만 확인하려면 `pintos/vm`에서 mmap 편의 타깃을 사용한다. 이 타깃도 `build/` 준비, 하위 디렉터리 생성, `TEST_SUBDIRS="tests/vm tests/threads"`, `FSDISK=10` 전달을 한 번에 처리한다.

전체 mmap 테스트를 실행하고 pass/FAIL 요약까지 확인:

```bash
cd /workspaces/pintos/pintos/vm
make p3-mmap-check
```

결과 파일만 갱신하고 요약 파일을 출력:

```bash
cd /workspaces/pintos/pintos/vm
make p3-mmap-results
cat build/selected-results
```

mmap 테스트 하나만 실행:

```bash
cd /workspaces/pintos/pintos/vm
make p3-mmap-one P3_MMAP_TEST=mmap-read
```

단일 테스트의 `.result`만 만들고 바로 출력:

```bash
cd /workspaces/pintos/pintos/vm
make p3-mmap-one-result P3_MMAP_TEST=mmap-read
```

단일 테스트의 `.output`만 만들고 바로 출력:

```bash
cd /workspaces/pintos/pintos/vm
make p3-mmap-one-output P3_MMAP_TEST=mmap-read
```

테스트 이름은 짧은 이름 또는 전체 경로로 지정할 수 있다.

```bash
make p3-mmap-one P3_MMAP_TEST=mmap-write
make p3-mmap-one P3_MMAP_TEST=tests/vm/mmap-write
```

포함되는 mmap 테스트 목록은 다음과 같다.

```text
tests/vm/mmap-read
tests/vm/mmap-close
tests/vm/mmap-unmap
tests/vm/mmap-overlap
tests/vm/mmap-twice
tests/vm/mmap-write
tests/vm/mmap-ro
tests/vm/mmap-exit
tests/vm/mmap-shuffle
tests/vm/mmap-bad-fd
tests/vm/mmap-clean
tests/vm/mmap-inherit
tests/vm/mmap-misalign
tests/vm/mmap-null
tests/vm/mmap-over-code
tests/vm/mmap-over-data
tests/vm/mmap-over-stk
tests/vm/mmap-remove
tests/vm/mmap-zero
tests/vm/mmap-bad-fd2
tests/vm/mmap-bad-fd3
tests/vm/mmap-zero-len
tests/vm/mmap-off
tests/vm/mmap-bad-off
tests/vm/mmap-kernel
```

이미 `.result`가 있어서 다시 실행되지 않으면 해당 테스트 산출물을 지운 뒤 다시 실행한다.

```bash
rm -f build/tests/vm/mmap-read.output \
      build/tests/vm/mmap-read.errors \
      build/tests/vm/mmap-read.result

make p3-mmap-one-result P3_MMAP_TEST=mmap-read
```

결과 확인:

```bash
cat build/selected-results
cat build/tests/vm/mmap-read.result
cat build/tests/vm/mmap-read.output
cat build/tests/vm/mmap-read.errors
```

커널 링크 단계에서 `_start_bss`, `_end_bss`, `start`, `_end_kernel_text`, `_end` 같은 심벌을 찾지 못하거나 relocation overflow가 나면 테스트 실패라기보다 빌드 산출물 또는 링크 스크립트 생성물이 꼬인 상태일 가능성이 높다. 이때는 한 번 깨끗하게 지우고 다시 실행한다.

```bash
cd /workspaces/pintos/pintos/vm
make clean
make p3-mmap-one P3_MMAP_TEST=mmap-read
```

## Extra 2, dup2 테스트까지 포함

Project 2 extra 테스트인 `dup2`까지 포함하려면 `TEST_SUBDIRS`에 `tests/userprog/dup2`를 추가하고, user program 컴파일 플래그에 `-DEXTRA2`를 준다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make check \
  TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/userprog/dup2 tests/threads" \
  TDEFINE=-DEXTRA2
```

`TDEFINE`은 `pintos/Makefile.userprog`에서 user program 컴파일 옵션에 붙는다.

## grade 형식으로 보고 싶을 때

`check`는 pass/FAIL 요약을 보여준다. Pintos grading 형식의 `grade` 파일까지 만들고 싶으면 `GRADING_FILE`도 Project 2 기준으로 덮어쓴다.

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make grade \
  TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/threads" \
  GRADING_FILE='$(SRCDIR)/tests/userprog/Grading.no-extra'
```

extra2까지 포함한다면:

```bash
cd /Users/sisu/Projects/jungle/pintos/pintos/vm
make grade \
  TEST_SUBDIRS="tests/userprog tests/filesys/base tests/userprog/no-vm tests/userprog/dup2 tests/threads" \
  TDEFINE=-DEXTRA2 \
  GRADING_FILE='$(SRCDIR)/tests/userprog/Grading.extra'
```

`grade` 결과 파일은 `pintos/vm/build/grade`에 생긴다.

## 결과 확인 위치

전체 요약:

```bash
cat /Users/sisu/Projects/jungle/pintos/pintos/vm/build/results
```

단일 테스트 출력 예시:

```bash
cat /Users/sisu/Projects/jungle/pintos/pintos/vm/build/tests/userprog/args-none.output
cat /Users/sisu/Projects/jungle/pintos/pintos/vm/build/tests/userprog/args-none.errors
cat /Users/sisu/Projects/jungle/pintos/pintos/vm/build/tests/userprog/args-none.result
```

## 주의사항

- `cd pintos/userprog && make check`는 Project 2 빌드로 Project 2 테스트를 돌리는 명령이다. Project 3 VM 코드까지 포함한 커널로 확인하려면 `pintos/vm`에서 실행해야 한다.
- `cd pintos/vm && make check`만 실행하면 `vm/Make.vars` 기본값 때문에 Project 3 테스트까지 포함된다.
- Project 3 빌드로 Project 2 테스트를 돌릴 때는 `TEST_SUBDIRS`만 Project 2 목록으로 제한한다.
- 기존 `pintos/vm/build` 산출물이 꼬였다고 의심되면 사용자가 직접 `cd pintos/vm && make clean` 후 다시 위 명령을 실행하면 된다.
