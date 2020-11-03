// The for loop in the backticks below requires bash.
// REQUIRES: shell
//
// RUN: %clangxx_memprof  %s -o %t

// stderr log_path
// RUN: %env_memprof_opts=log_path=stderr %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-GOOD --dump-input=always

// Good log_path.
// RUN: rm -f %t.log.*
// RUN: %env_memprof_opts=log_path=%t.log %run %t
// RUN: FileCheck %s --check-prefix=CHECK-GOOD --dump-input=always < %t.log.*

// Invalid log_path.
// RUN: %env_memprof_opts=log_path=/dev/null/INVALID not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-INVALID --dump-input=always

// Too long log_path.
// RUN: %env_memprof_opts=log_path=`for((i=0;i<10000;i++)); do echo -n $i; done` \
// RUN:   not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-LONG --dump-input=always

// Specifying the log name via the __memprof_profile_filename variable.
// Note we use an invalid path since that is sufficient for checking that the
// specified __memprof_profile_filename value is passed through to the runtime.
// Using an automatically generated name via %t can cause weird issues with
// unexpected macro expansion if the path includes tokens that match a build
// system macro (e.g. "linux").
// RUN: %clangxx_memprof  %s -o %t -DPROFILE_NAME_VAR="/dev/null/INVALID"
// RUN: not %run %t 2>&1 | FileCheck %s --check-prefix=CHECK-INVALID --dump-input=always

#ifdef PROFILE_NAME_VAR
#define xstr(s) str(s)
#define str(s) #s
char __memprof_profile_filename[] = xstr(PROFILE_NAME_VAR);
#endif

#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  char *x = (char *)malloc(10);
  memset(x, 0, 10);
  free(x);
  return 0;
}
// CHECK-GOOD: Memory allocation stack id
// CHECK-INVALID: ERROR: Can't open file: /dev/null/INVALID
// CHECK-LONG: ERROR: Path is too long: 01234
