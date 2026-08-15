# Hosted and platform-porting records

The current Macaros host integration is documented by subsystem in
[`docs/features`](../features/README.md) and implemented under
[`hosted/`](../../hosted/README.md).

The [initial platform bring-up](initial-platform-bringup/README.md) is retained
separately. It records how the AArch64 fundamentals were proven on QEMU, how the
macOS-hosted execution model was de-risked, and how those results were grafted
into AROS. It is historical for Macaros, but remains a runnable reference for
anyone adding AROS support to a new architecture or host platform.
