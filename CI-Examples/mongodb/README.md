# MongoDB example

This directory contains an example for running MongoDB in Gramine, including the
Makefile and a template for generating the manifest.

# Generating the manifest

## Installing prerequisites

Install MongoDB 7.0 Community Edition on Ubuntu 22.04 with the following commands:

1. Import the public key used by the package management system:

    1. From a terminal, install `gnupg` and `curl` if they are not already available:
       ```
       sudo apt-get install gnupg curl
       ```

    2. Issue the following command to import the MongoDB public GPG Key from
       https://pgp.mongodb.com/server-7.0.asc:
       ```
       curl -fsSL https://pgp.mongodb.com/server-7.0.asc | \
           sudo gpg -o /usr/share/keyrings/mongodb-server-7.0.gpg --dearmor
       ```

2. Create a list file for MongoDB:
   ```
   echo "deb [ arch=amd64,arm64 signed-by=/usr/share/keyrings/mongodb-server-7.0.gpg ] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/7.0 multiverse" | \
       sudo tee /etc/apt/sources.list.d/mongodb-org-7.0.list
   ```

3. Reload local package database:
   ```
   sudo apt-get update
   ```

4. Install the MongoDB packages:
   ```
   sudo apt-get install -y mongodb-org
   ```

## Building for Linux

Run `make` (non-debug) or `make DEBUG=1` (debug) in the directory.

## Building for SGX

Run `make SGX=1` (non-debug) or `make SGX=1 DEBUG=1` (debug) in the directory.

# Running MongoDB with Gramine

Examples of running MongoDB under Gramine (the command-line options are hardcoded in the manifest
file):

Without SGX:
```
gramine-direct mongod
```

With SGX:
```
gramine-sgx mongod
```

# Testing client connection

Run the below commands from a new terminal:

- `mongosh scripts/insert.js` - inserts new documents into a collection
- `mongosh scripts/fetch.js` - fetches all documents, and prints their content

# Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** not verified.¹ Command: `gramine-direct mongod`.
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `gramine-sgx mongod`.
- **vbs:** not verified.¹ No VBS recipe. `mongod` uses the x18 register (ARM64).

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
