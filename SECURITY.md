# Security policy

## Supported versions

Only the latest revision on the default branch is supported.

## Reporting a vulnerability

Do not include UU account data, session logs, device identifiers, Wine registry
exports, or private network addresses in a public issue. Open a GitHub security
advisory for the repository and provide a minimal reproducer with sensitive
values removed.

The helper listens only on `127.0.0.1`. Changing it to a non-loopback address is
unsupported because the helper protocol does not provide authentication or
encryption.
