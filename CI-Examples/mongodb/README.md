# MongoDB example

This directory contains an example for running MongoDB in Gramine, including the Makefile and a
template for generating the manifest. Pass or fail status per backend: matrix in
[`CI-Examples/README.md`](../README.md).

# Building

Install the backend first (`Documentation/quickstart/`) and run all commands in this directory.

## Pre-requisites

Install MongoDB 7.0 Community Edition on Ubuntu 22.04:

```sh
sudo apt-get install gnupg curl
curl -fsSL https://pgp.mongodb.com/server-7.0.asc | \
    sudo gpg -o /usr/share/keyrings/mongodb-server-7.0.gpg --dearmor
echo "deb [ arch=amd64,arm64 signed-by=/usr/share/keyrings/mongodb-server-7.0.gpg ] https://repo.mongodb.org/apt/ubuntu jammy/mongodb-org/7.0 multiverse" | \
    sudo tee /etc/apt/sources.list.d/mongodb-org-7.0.list
sudo apt-get update
sudo apt-get install -y mongodb-org
```

## Direct, VM, and SEV

```sh
make
```

## SGX

```sh
make SGX=1
```

The example has no VBS recipe.

# Testing

Start the server (the command-line options are hardcoded in the manifest). It listens on the
MongoDB default port 27017. Under the VM and SEV, `GRAMINE_VM_INBOUND` forwards the port through
the relay:

```sh
gramine-direct mongod
GRAMINE_VM_INBOUND=27017:27017 gramine-vm mongod     # VM; not verified
GRAMINE_VM_INBOUND=27017:27017 gramine-sev mongod    # SEV; not verified
gramine-sgx mongod                                   # SGX; not verified
```

From a new terminal, insert documents into a collection and fetch them:

```sh
mongosh scripts/insert.js
mongosh scripts/fetch.js
```

`fetch.js` prints the inserted documents.
