PAL host ABI
============

.. note::

   This page is inherited from upstream Gramine, where it is built with Sphinx and Doxygen. This
   tree no longer builds that site, so the ``doxygen...`` and ``auto...`` directives below are
   only a list of names: the declarations and their documentation comments are in ``pal/include/pal/pal.h`` and the headers it includes.

TODO: This document is outdated and needs a proper review.

PAL Host ABI is the interface used by Gramine to interact with its host. It is
translated into the host's native ABI (e.g. system calls for UNIX) by a layer
called the Platform Adaptation Layer (PAL). A PAL not only exports a set of APIs
(PAL APIs) that can be called by the library OS, but also acts as the loader
that bootstraps the library OS. The design of PAL Host ABI strictly follows
three primary principles, to guarantee functionality, security, and portability:

* The host ABI must be stateless.
* The host ABI must be a narrowed interface to reduce the attack surface.
* The host ABI must be generic and independent from the native ABI of any of the
  supported hosts.

Most of the PAL Host ABI is adapted from the Drawbridge library OS.

PAL as loader
-------------

Each PAL implementation must support loading and dynamic relocation of
ELF-format executables and dynamic libraries. PAL needs to look up all
unresolved symbols in loaded binaries and resolve the ones matching the names of
PAL APIs. PAL does not and will not resolve other unresolved symbols, so the
loaded libraries and executables must resolve them afterwards.

After loading the binaries, PAL needs to load and interpret the manifest files.
The manifest syntax is described in :doc:`../manifest-syntax`.

Manifest and executable loading
^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^

To run a program in Gramine the PAL loader needs a manifest, which will describe
the whole environment inside Gramine namespace. It also describes which
executable to start first (via ``libos.entrypoint``).

Data types and variables
------------------------

Data types
^^^^^^^^^^

PAL handles
"""""""""""

``PAL_HANDLE`` is the type of identifiers that are returned by PAL when opening
or creating resources. It is an opaque type, that should not be accessed outside
of PAL and its details depend on the actual PAL version (host). There is a
common header (present on all PAL hosts) accessible from PAL code, which allows
for checking the handle type::

   typedef struct {
       struct {
           PAL_IDX type;
       } hdr;
       /* other host-specific definitions */
   }* PAL_HANDLE;

Rest of the fields are private to specific PAL hosts, but they usually define
different handle subtypes that represent different resources such as files,
directories, pipes or sockets. The actual memory allocated for the PAL handles
may be variable-sized.

Basic types
"""""""""""

.. doxygentypedef:: PAL_IDX
   :project: pal

PAL public state
^^^^^^^^^^^^^^^^

All PALs in Gramine expose a structure that provides static immutable
information about the current process and its host. The address of the structure
can be retrieved via :func:`PalGetPalPublicState()` and can be memoized in a
global variable for ease of use.

.. doxygenstruct:: pal_public_state
   :project: pal
   :members:

PAL public state - topology information
"""""""""""""""""""""""""""""""""""""""

.. doxygenstruct:: pal_cpu_info
   :project: pal
   :members:

.. doxygenstruct:: pal_topo_info
   :project: pal
   :members:

PAL APIs
--------

The PAL APIs contain a |~| number of functions that can be called from the
library OS.


Memory allocation
^^^^^^^^^^^^^^^^^

The ABI includes three calls to allocate, free, and modify the permission bits
on page-base virtual memory. Permissions include read, write, execute, and
guard. Memory regions can be unallocated, reserved, or backed by committed
memory.

.. doxygenfunction:: PalVirtualMemoryAlloc
   :project: pal

.. doxygenfunction:: PalVirtualMemoryFree
   :project: pal

.. doxygentypedef:: pal_prot_flags_t
   :project: pal

.. doxygenstruct:: pal_initial_mem_range
   :project: pal
   :members:

.. doxygenfunction:: PalVirtualMemoryProtect
   :project: pal


Process creation
^^^^^^^^^^^^^^^^

The ABI includes one call to create a child process and one call to terminate
the running process. A child process does not inherit any objects or memory from
its parent process and the parent process may not modify the execution of its
children. A parent can wait for a child to exit using its handle. Parent and
child may communicate through I/O streams provided by the parent to the child at
creation.

.. doxygenfunction:: PalProcessCreate
   :project: pal
.. doxygenfunction:: PalProcessExit
   :project: pal


Stream creation/connect/open
^^^^^^^^^^^^^^^^^^^^^^^^^^^^

The stream ABI includes nine calls to open, read, write, map, unmap, truncate,
flush, delete and wait for I/O streams and three calls to access metadata about
an I/O stream. The ABI purposefully does not provide an ioctl call. Supported
URI schemes include:
``file:``,
``pipe:``,
``http:``,
``https:``,
``tcp:``,
``udp:``,
``pipe.srv:``,
``http.srv``,
``tcp.srv:`` and
``udp.srv:``.
The latter four schemes are used to open inbound I/O streams for server
applications.

.. doxygenfunction:: PalStreamOpen
   :project: pal

.. doxygenfunction:: PalStreamWaitForClient
   :project: pal

.. doxygenfunction:: PalStreamRead
   :project: pal

.. doxygenfunction:: PalStreamWrite
   :project: pal

.. doxygenfunction:: PalStreamDelete
   :project: pal

.. doxygenfunction:: PalStreamMap
   :project: pal

.. doxygenfunction:: PalStreamSetLength
   :project: pal

.. doxygenfunction:: PalStreamFlush
   :project: pal

.. doxygenfunction:: PalSendHandle
   :project: pal

.. doxygenfunction:: PalReceiveHandle
   :project: pal

.. doxygenfunction:: PalStreamAttributesQuery
   :project: pal

.. doxygentypedef:: PAL_STREAM_ATTR
   :project: pal
.. doxygenstruct:: _PAL_STREAM_ATTR
   :project: pal

.. doxygenfunction:: PalStreamAttributesQueryByHandle
   :project: pal

.. doxygenfunction:: PalStreamAttributesSetByHandle
   :project: pal

.. doxygenfunction:: PalStreamChangeName
   :project: pal


Path operations on host files
"""""""""""""""""""""""""""""

These calls take a ``file:`` URI and work on the host path itself, without a
handle (declared in ``pal/include/pal/pal.h``, where the arguments are
described):

- ``PalFileExchange(a, b)`` swaps two paths atomically (``renameat2`` with
  ``RENAME_EXCHANGE``).
- ``PalFileReadLink(uri, buf, &size)`` reads a symbolic link without following
  it; ``-PAL_ERROR_INVAL`` if the path is not a link.
- ``PalFileXattr(uri, op, name, value, &size, flags)`` gets, sets, lists or
  removes an extended attribute (``PAL_XATTR_GET``/``SET``/``LIST``/``REMOVE``;
  flags ``PAL_XATTR_CREATE``, ``PAL_XATTR_REPLACE``, ``PAL_XATTR_NOFOLLOW``). A
  missing attribute is ``-PAL_ERROR_STREAMNOTEXIST``, a filesystem without
  attributes ``-PAL_ERROR_NOTSUPPORT``, a too-small buffer or too-large value
  ``-PAL_ERROR_TOOLONG``.

The Linux PAL implements them with the host system calls, the VM and TDX PALs
with virtio-fs requests (``FUSE_RENAME2``, ``FUSE_READLINK``,
``FUSE_*XATTR``; ``gramine-vm`` starts virtiofsd with ``--xattr``). Other PALs
return ``-PAL_ERROR_NOTSUPPORT``, which the LibOS treats like a host
filesystem without these features.

Flags used for stream manipulation
""""""""""""""""""""""""""""""""""

.. doxygenenum:: pal_access
   :project: pal

.. doxygentypedef:: pal_share_flags_t
   :project: pal

.. doxygenenum:: pal_create_mode
   :project: pal

.. doxygentypedef:: pal_stream_options_t
   :project: pal

.. doxygenenum:: pal_delete_mode
   :project: pal

.. doxygentypedef:: pal_wait_flags_t
   :project: pal


Socket handling
^^^^^^^^^^^^^^^

.. doxygenenum:: pal_socket_domain
   :project: pal

.. doxygenenum:: pal_socket_type
   :project: pal

.. doxygenstruct:: pal_socket_addr
   :project: pal

.. doxygenstruct:: iovec
   :project: pal

.. doxygenfunction:: PalSocketCreate
   :project: pal

.. doxygenfunction:: PalSocketBind
   :project: pal

.. doxygenfunction:: PalSocketListen
   :project: pal

.. doxygenfunction:: PalSocketAccept
   :project: pal

.. doxygenfunction:: PalSocketConnect
   :project: pal

.. doxygenfunction:: PalSocketSend
   :project: pal

.. doxygenfunction:: PalSocketRecv
   :project: pal


Thread creation
^^^^^^^^^^^^^^^

The ABI supports multithreading through five calls to create, sleep, yield the
scheduler quantum for, resume execution of, and terminate threads, as well as
seven calls to create, signal, and block on synchronization objects.

.. doxygenfunction:: PalThreadCreate
   :project: pal

.. doxygenfunction:: PalThreadYieldExecution
   :project: pal

.. doxygenfunction:: PalThreadExit
   :project: pal

.. doxygenfunction:: PalThreadResume
   :project: pal


Exception handling
^^^^^^^^^^^^^^^^^^

.. doxygenenum:: pal_event
   :project: pal

.. doxygenstruct:: PAL_CONTEXT
   :project: pal
   :members:

.. doxygentypedef:: pal_event_handler_t
   :project: pal

.. doxygenfunction:: PalSetExceptionHandler
   :project: pal


Synchronization
^^^^^^^^^^^^^^^

.. doxygenfunction:: PalEventCreate
   :project: pal

.. doxygenfunction:: PalEventSet
   :project: pal

.. doxygenfunction:: PalEventClear
   :project: pal

.. doxygenfunction:: PalEventWait
   :project: pal

Objects
^^^^^^^

.. doxygenfunction:: PalStreamsWaitEvents
   :project: pal

.. doxygenfunction:: PalObjectDestroy
   :project: pal

Miscellaneous
^^^^^^^^^^^^^

The ABI includes calls to get wall clock time, generate cryptographically-strong
random bits, to obtain an attestation report and quote, etc.

.. doxygenfunction:: PalDebugLog
   :project: pal

.. doxygenfunction:: PalGetPalPublicState
   :project: pal

.. doxygenfunction:: PalSystemTimeQuery
   :project: pal

``PalGetCpuTime(thread, &user_us, &sys_us)`` returns the CPU time used by the
whole process or by the calling thread, in microseconds. Only the Linux PAL
implements it (host ``getrusage``); the others return
``-PAL_ERROR_NOTIMPLEMENTED``, and the LibOS then reports zero CPU time in
``getrusage``/``times`` and uses the real-time clock for the CPU-time clocks.

.. doxygenfunction:: PalRandomBitsRead
   :project: pal

.. doxygenfunction:: PalSegmentBaseGet
   :project: pal

.. doxygenfunction:: PalSegmentBaseSet
   :project: pal

.. doxygenenum:: pal_segment_reg
   :project: pal

.. doxygenfunction:: PalCpuIdRetrieve
   :project: pal

.. doxygenfunction:: PalAttestationReport
   :project: pal

.. doxygenfunction:: PalAttestationQuote
   :project: pal

.. doxygenfunction:: PalGetSpecialKey
   :project: pal
