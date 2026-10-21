# Glossary

<div class="glossary">

PAL  
Platform Adaptation Layer

PAL is the layer of Gramine that implements a narrow Drawbridge-like ABI
interface (with function names starting with the <span class="title-ref">Pal</span> prefix)

<div class="seealso">

<https://www.microsoft.com/en-us/research/wp-content/uploads/2016/02/asplos2011-drawbridge.pdf>

</div>

Whenever Gramine requires a service from the host platform (memory
allocation, thread management and synchronization, filesystem and network
stacks, etc.), it calls the corresponding PAL functionality. The PAL ABI
is host-platform agnostic and is backed by the host-platform specific PAL,
for example, the Linux-SGX PAL.

RA-TLS  
A library to augment classic SSL/TLS sessions with
`Remote Attestation`. RA-TLS extends the SSL/TLS handshake protocol
to force one endpoint into verifying the `SGX Quote` embedded into
the other endpoint's certificate chain. RA-TLS is designed to be a drop-in
replacement for classic SSL/TLS libraries.

<div class="seealso">

`attestation`

</div>

Secret Provisioning  
Secret provisioning is a mechanism to deliver secrets (such as encryption
keys, passwords, etc.) from a remote trusted party inside a `TEE`.
It is typically built on top of a `Secure Channel`.

<div class="seealso">

`attestation`

</div>

Secure Channel  
Secure channels are communication channels for trusted transmission of
arbitrary data between a `TEE` and a remote trusted party or between
two TEEs. They are typically built on top of the classic TLS/SSL channels.

<div class="seealso">

`attestation`

</div>

SGX  
Software Guard Extensions is a set of instructions on Intel processors for
creating Trusted Execution Environments (`TEE`).

Thread Control Block

> <div class="todo">
>
> TBD
>
> </div>

</div>
