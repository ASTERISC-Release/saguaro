Test purpose
------------

These tests perform common FS operations in various ways to exercise the Saguaro
FS subsystem:

- open/close
- read/write
- create/delete
- read/change size
- seek/tell
- memory-mapped read/write
- sendfile
- copy directory in different ways

How to execute
--------------

- `saguaro-test pytest -v`

Encrypted file tests assume that Saguaro was built with SGX enabled (see comment
in `test_enc.py`).
