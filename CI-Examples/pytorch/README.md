# PyTorch

This directory contains steps and artifacts to run a PyTorch sample workload on
Gramine. Specifically, this example uses a pre-trained model for image
classification.

The workload reads an image from a file on disk `input.jpg` and runs the
classifier to detect what object is present in the image. For the image shipping
with the workload, the output should be

```
[('Labrador retriever', 41.58518600463867), ('golden retriever', 16.59165382385254), ('Saluki, gazelle hound', 16.286855697631836), ('whippet', 2.853914976119995), ('Ibizan hound, Ibizan Podenco', 2.3924756050109863)]
```

With high probability (41%) the classifier detected the image to contain a
Labrador retriever.

# Pre-requisites
The following steps should suffice to run the workload on a standard Ubuntu distribution.

- `sudo apt install libnss-mdns libnss-myhostname` to install additional
  DNS-resolver libraries.
- `sudo apt install python3-pip lsb-release` to install `pip` and `lsb_release`.
  The former is required to install additional Python packages while the latter
  is used by the Makefile.

# Build

Run `make` to build the non-SGX version and `make SGX=1` to build the SGX
version.

# Run

Execute any one of the following commands to run the workload:

- natively: `python3 pytorchexample.py`
- Gramine w/o SGX: `gramine-direct ./pytorch ./pytorchexample.py`
- Gramine with SGX: `gramine-sgx ./pytorch ./pytorchexample.py`

# Running under each backend

Run the commands in this directory after the build (`make`, or `make VBS=1`), with the install of the backend active (`Documentation/quickstart/`). The matrix in `CI-Examples/README.md` gives the result and the date for each backend. "Not verified" means that no result is recorded.

- **direct:** not verified.¹ Command: `gramine-direct ./pytorch ./pytorchexample.py`.
- **vm:** not verified.¹
- **sev:** not verified.¹
- **tdx:** not verified.²
- **sgx:** not verified.³ Upstream command: `gramine-sgx ./pytorch ./pytorchexample.py`.
- **vbs:** not verified.¹ No VBS recipe.

¹ Not verified: no result is recorded for this backend (matrix in `CI-Examples/README.md`).  
² TDX was tested offline by the maintainer, with the same result as SEV. No TDX log is in the repository, so the result is reported and not verified here. The TDX install needs `td-shim` (`Documentation/TODO.md`).  
³ SGX has not run on this tree's hardware. The SGX build needs `protobuf-c` (`Documentation/TODO.md`).
