<div class="program">

gramine-sgx-ias-verify-report

</div>

# `gramine-sgx-ias-verify-report` -- Verify Intel Attestation Service report

> [!NOTE]
> This page is inherited from upstream Gramine and describes the Intel SGX backend. This tree
> still builds that backend but does not test it; see `README.md` for what is validated here.

## Synopsis

`gramine-sgx-ias-verify-report` \[*OPTION*\]...

## Description

<span class="title-ref">gramine-sgx-ias-verify-report</span> verifies attestation report retrieved from the
Intel Attestation Service (using `gramine-sgx-ias-request` for example). It
also verifies that the quote contained in the IAS report contains expected
values.

## Command line arguments

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

`-r, --report-path`  

IAS report to verify.

</div>

<div class="option">

`-s, --sig-path`  

Path to the IAS report's signature.

</div>

<div class="option">

`-n, --nonce`  

Nonce that's expected in the report (optional).

</div>

<div class="option">

`-S, --mr-signer`  

Expected mr_signer field (hex string, optional).

</div>

<div class="option">

`-E, --mr-enclave`  

Expected mr_enclave field (hex string, optional).

</div>

<div class="option">

`-R, --report-data`  

Expected report_data field (hex string, optional).

</div>

<div class="option">

`-P, --isv-prod-id`  

Expected isv_prod_id field (hex string, optional).

</div>

<div class="option">

`-V, --isv-svn`  

Expected isv_svn field (hex string, optional).

</div>

<div class="option">

`-i, --ias-pubkey`  

Path to IAS public RSA key (PEM format, optional).

</div>

<div class="option">

`--allow-debug-enclave`  

Allow debug enclave (SGXREPORT.ATTRIBUTES.DEBUG = 1).

</div>

<div class="option">

`--allow-outdated-tcb`  

Treat IAS status code GROUP_OUT_OF_DATE as OK.

</div>

<div class="option">

`--allow-hw-config-needed`  

Treat IAS status code CONFIGURATION_NEEDED as OK.

</div>

<div class="option">

`--allow-sw-hardening-needed`  

Treat IAS status code SW_HARDENING_NEEDED as OK.

</div>

To treat the IAS status code CONFIGURATION_AND_SW_HARDENING_NEEDED as OK, set
both `--allow-hw-config-needed` and
`--allow-sw-hardening-needed` options.

## Example

Report verification with most options enabled:

``` sh
$ gramine-sgx-ias-verify-report -v -m -r rp -s sp -i ias.pem -n thisisnonce -S 14b284525c45c4f526bf1535d05bd88aa73b9e184464f2d97be3dabc0d187b57 -E 4d69102c40401f40a54eb156601be73fb7605db0601845580f036fd284b7b303 -R 0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000004ba476e321e12c720000000000000001 -P 0 -V 0
Verbose output enabled
Endianness set to MSB
Using IAS public key from file 'ias.pem'
IAS key: RSA, 2048 bits
Decoded IAS signature size: 256 bytes
IAS report: signature verified correctly
IAS report: nonce OK
IAS report: quote decoded, size 432 bytes
[...quote dump...]
Quote: mr_signer OK
Quote: mr_enclave OK
Quote: isv_prod_id OK
Quote: isv_svn OK
Quote: report_data OK
Quote: enclave attributes OK
```
