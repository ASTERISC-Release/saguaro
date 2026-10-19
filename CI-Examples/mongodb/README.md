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

Build and run in this directory, with the install of the backend active (`Documentation/quickstart/`). Pass or fail status: matrix in [`CI-Examples/README.md`](../README.md).

- **direct:** `make`, then `gramine-direct mongod`; run the client as above.
- **vm:** `gramine-vm mongod`. Not verified.
- **sev:** `gramine-sev mongod`. Not verified.
- **vbs:** no recipe.
