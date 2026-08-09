# Security Policy

## Supported Versions

Karshipta is in active pre-release development. Security updates target the latest code on `main`.

| Version | Supported          |
| ------- | ------------------ |
| v0.x    | :white_check_mark: |

## Reporting a Vulnerability

We take the security of Karshipta seriously. It is command and control software: vulnerabilities can have physical consequences. If you discover one, please do not disclose it publicly.

### How to Report

1. **GitHub Private Vulnerability Reporting**: the preferred way is GitHub's [Private Vulnerability Reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-post-processing-security-vulnerabilities/privately-reporting-a-security-vulnerability) on this repository.
2. **Email**: alternatively, send an email to `tech@nikx.one`.

### What to Expect

- **Acknowledgment**: within 48 hours.
- **Updates**: regular status updates until the issue is resolved.
- **Disclosure**: once a fix is verified, we coordinate a public disclosure date with you, preferably via a GitHub Security Advisory.

## Scope Notes

- Karshipta is simulation-first and not certified for real flight operations yet. Reports about unsafe behavior against real wards are still very welcome; treat anything that could bypass autopilot safety checks as security-relevant.
- The gateway-to-console wire protocol (protobuf Envelopes over WebSocket) and the optional E2E-encrypted relay transport are in scope.
