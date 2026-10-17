# Will my application run?

To check whether an application is likely to run under `gramine-direct` or `gramine-vm`,
trace its system calls and compare them with the syscall matrix built from the LTP tests:

    scripts/syscall-profile.py run -- ./app args      # which syscalls, and a verdict per loader
    scripts/syscall-tolerance.py -- ./app args        # does it still work without the ones
                                                      # that are not supported?

The results are heuristic and cover only the code paths of the traced run. How it works, how to
run it and how well it did on ten applications:
[devel/syscall-profile/README.md](devel/syscall-profile/README.md); the matrix itself:
[devel/syscall-compat/MATRIX.md](devel/syscall-compat/MATRIX.md).
