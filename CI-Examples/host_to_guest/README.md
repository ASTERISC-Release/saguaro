## Note
**You must allocate greater than 4GB for this to function properly.
Anything 8GB or higher should be more than enough.**
# Relevant Files

- `gramine-tdx/libos/src/sys/libos_mmap.c`
- `qemu/util/mmap-alloc.c`
- `gramine-tdx/CI-Examples/host_to_guest/align_test.c`
- `host_to_guest.sh`

# Run the Test Case

- Update your `PYTHONPATH` to correct one

**Assuming in project's root dir.**
```bash
cd gramine-tdx
export PYTHONPATH=$PWD/built-debug/lib/python3.13/site-packages/
cd CI-Examples/host_to_guest
make
gramine-vm align_test
```

# Test Description

The test-case creates an array of 512 pointers for host_object_to_guest allocations, makes allocations and then prints the strings set by the host.

It uses a pre-set value for the len of the allocation as a magic number for signaling to `libos-mmap.c` that it should capture it as a host object allocation.
***In the final system, this would be matched on the relevant file descriptor.***

Also, I'm certain you could just as easily instrument what the `libos-mmap.c` changes do with `LD_PRELOAD`.
I think the logic should be pretty simple to follow, let me know if you have any questions.

# High-Level Flow -- Setup

1. Host spawns listener after ram allocation is made for vm.
2. Guest maps data region.
3. Guest maps comms region and sets magic number to signal region is ready.
4. libos-mmap sends message to listener to setup the data region (create data structure for tracking open slots).
5. Listener is now ready to receive host object allocation requests.

# High-Level Flow -- Allocation

1. Guest makes an allocation with a signal (in this case `size_t special_allocation = 0x123456789ULL;` for len in `mmap()`).
2. libos-mmap checks and sees that `len == special_allocation` is true.
3. libos-mmap signals to listener thread that it should allocate a host object into the guest and waits for the return pointer .
4. The listener finds an empty slot in the `gem_slots` data structure.
5. The listener unmaps the the slot at `host_address` and `guest_address`, and then maps them back in at identical physical addresses for alignment (just using a dummy fd backed memory currently).
6. Listener sets the pointer for the guest and signals that the event was handled.
7. libos-mmap returns the pointer to the guest.
