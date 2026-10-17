<div class="program">

gramine-sgx-ias-request

</div>

# `gramine-sgx-ias-request` -- Submit Intel Attestation Service request

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` and
> `Documentation/devel/multiarch.md` for what is validated here.

## Synopsis

`gramine-sgx-ias-request` *COMMAND* \[*OPTION*\]...

## Description

<span class="title-ref">gramine-sgx-ias-request</span> submits requests to Intel Attestation Service (IAS).
Possible commands are retrieving EPID signature revocation list and verifying
attestation evidence for an SGX enclave quote.

## Command line arguments

### General options

<div class="option">

`-h, --help`  

Display usage.

</div>

<div class="option">

`-v, --verbose`  

Print more information.

</div>

<div class="option">

`-m, --msb`  

Print/parse hex strings in big-endian order.

</div>

<div class="option">

`-k, --api-key`  

IAS API key.

</div>

### Commands

<div class="option">

sigrl

Retrieve signature revocation list for a given EPID group.

Possible `sigrl` options:

<div class="option">

`-g, --gid`  

EPID group id (hex string).

</div>

<div class="option">

`-i, --sigrl-path`  

Path to save retrieved SigRL to.

</div>

<div class="option">

`-S, --sigrl-url`  

URL for the IAS SigRL endpoint (optional).

</div>

</div>

<div class="option">

report

Verify attestation evidence (quote).

Possible `report` options:

<div class="option">

`-q, --quote-path`  

Path to quote to submit.

</div>

<div class="option">

`-r, --report-path`  

Path to save IAS report to.

</div>

<div class="option">

`-s, --sig-path`  

Path to save IAS report's signature to.

</div>

<div class="option">

`-n, --nonce`  

Nonce to use (optional).

</div>

<div class="option">

`-c, --cert-path`  

Path to save IAS certificate to (optional).

</div>

<div class="option">

`-R, --report-url`  

URL for the IAS attestation report endpoint (optional).

</div>

</div>

## Examples

*SigRL* retrieval:

``` sh
$ gramine-sgx-ias-request sigrl -k $IAS_API_KEY -g ef0a0000 -i sigrl
No SigRL for given EPID group ID ef0a0000
```

*Quote* verification:

``` sh
$ gramine-sgx-ias-request report -k $IAS_API_KEY -q gr.quote -r ias.report -s ias.sig -c ias.cert -v
Verbose output enabled
IAS request:
{"isvEnclaveQuote":"AgABAO8..."}
[...snip curl output...]
IAS response: 200
IAS report saved to: ias.report
IAS report signature saved to: ias.sig
IAS certificate saved to: ias.cert
IAS submission successful

$ cat ias.report
{"id":"205146415611480061439763344693868541328","timestamp":"2020-03-20T10:48:32.353294","version":3,"epidPseudonym":"Itmg0 [...]","isvEnclaveQuoteStatus":"GROUP_OUT_OF_DATE" [...]}
```
