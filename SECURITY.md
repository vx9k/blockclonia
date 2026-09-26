# Security Policy

## Reporting a Vulnerability

We take security very seriously. If you discover a security vulnerability in Blockclonia, please **do not** open a public GitHub issue. Instead, please report it responsibly by emailing security@kthread.dev or using GitHub's private vulnerability reporting feature.

### What to Include

When reporting a vulnerability, please provide:
- A clear description of the vulnerability
- Steps to reproduce the issue
- Potential impact and severity
- Any affected platforms (Windows, Linux, macOS, Android, FreeBSD, OpenBSD)
- Your contact information for follow-up

### Response Timeline

We aim to:
- Acknowledge receipt of your report within 48 hours
- Provide an initial assessment within 7 days
- Release a security patch within 30 days for confirmed vulnerabilities (where feasible)
- Keep you updated on the status of your report

## Scope of Security Coverage

This policy applies to:
- **Core Engine**: C11 codebase, Vulkan 1.0 renderer, physics simulation
- **Network**: Multiplayer protocol and server-client communication
- **User Data**: Account credentials, player data, world data
- **Platforms**: Windows, Linux, macOS, Android, FreeBSD, OpenBSD
- **Dependencies**: All third-party libraries and build tools

## Known Security Limitations

### Current Development State
- **Alpha/Beta Status**: Blockclonia is under active development. Security should not be considered production-ready for sensitive use cases.
- **Protocol Evolution**: The multiplayer protocol may change; older clients may become incompatible.
- **No User Authentication** (Initial Releases): Early versions may use lightweight or no authentication. This will be strengthened as multiplayer support matures.

## Security Best Practices

### For Server Operators

1. **Network Security**
   - Run servers behind firewalls with restrictive ingress/egress rules
   - Use TLS/SSL for all client-server communications
   - Implement rate limiting to mitigate DDoS attacks
   - Monitor for suspicious connection patterns

2. **Access Control**
   - Implement player whitelisting/blacklisting systems
   - Use strong, unique admin credentials
   - Rotate API keys and authentication tokens regularly
   - Enforce strong password policies for player accounts

3. **World & Data Protection**
   - Regularly backup world data and player data
   - Store data with appropriate file permissions (not world-readable)
   - Encrypt sensitive data at rest
   - Use separate storage for authentication credentials and game data

4. **Server Maintenance**
   - Keep Blockclonia server binaries up to date
   - Patch operating system and dependency vulnerabilities promptly
   - Maintain audit logs of administrative actions
   - Review logs regularly for unauthorized access attempts

### For Client Users

1. **Software Updates**
   - Keep the Blockclonia client up to date
   - Enable automatic updates where available
   - Review changelog before updating

2. **Credentials**
   - Use unique, strong passwords for your Blockclonia account
   - Enable multi-factor authentication (MFA) when available
   - Never share your authentication tokens or API keys
   - Be cautious when connecting to unfamiliar servers

3. **Malware Prevention**
   - Download Blockclonia only from official sources
   - Verify GPG signatures or checksums when provided
   - Keep your operating system and antivirus software updated
   - Be cautious with mods, plugins, and add-ons from untrusted sources

4. **Platform-Specific Considerations**
   - **Android**: Only install from official app store; review requested permissions
   - **BSD Variants**: Use your system package manager or official builds; verify checksums
   - **Desktop**: Use official binary downloads or verify source code before compilation

## Security Features

### Current Implementation
- Input validation on all network messages
- Bounds checking for physics calculations
- Memory safety practices in C11 code
- Secure random number generation for cryptographic operations

### Planned/In Development
- End-to-end encryption for multiplayer communications
- Account authentication with salted password hashing
- Rate limiting on authentication attempts
- Player permission and capability system
- Admin audit logging
- Mod/plugin security sandboxing

## Third-Party Dependencies

We maintain a list of all dependencies and their security status. To review:
- Check `CMakeLists.txt` for build dependencies
- Review the dependency versions in release notes
- Monitor dependency advisories through GitHub security alerts

## Supported Versions

Only the latest stable release receives security updates. Earlier versions will not receive patches, but we encourage users to upgrade to the latest version for security and performance improvements.

## Security Testing

- **Code Review**: All pull requests are reviewed for security issues
- **Fuzzing**: Critical code paths (especially network parsing) undergo fuzzing
- **Static Analysis**: Automated security scanning via GitHub's CodeQL
- **Dynamic Analysis**: Runtime bounds checking and memory safety tools
- **Penetration Testing**: Will be conducted as the multiplayer component matures

## Multi-Platform Security Considerations

### Windows
- Digital code signing (when available)
- SmartScreen compatibility
- Windows Defender exclusion guidance if needed

### Linux/macOS/BSD
- AppArmor/SELinux profiles (where applicable)
- Signed releases (GPG/Ed25519)
- Compatibility with security-hardened kernels

### Android
- Google Play Protect compatibility
- Permission model compliance
- Over-the-air (OTA) update security

### FreeBSD/OpenBSD
- Pledge/unveil sandbox support (OpenBSD)
- Jail compatibility (FreeBSD)
- Port security considerations

## Contributing Security Fixes

If you'd like to contribute a security fix:
1. Do not open a public PR
2. Contact the maintainers privately with the details
3. Work with maintainers on a fix in a private branch
4. Coordinate public disclosure timing

## Compliance & Standards

- Follow OWASP principles for web/multiplayer security
- Adhere to platform-specific guidelines (Apple App Store, Google Play, etc.)
- Comply with data protection regulations (GDPR, CCPA, etc.) as applicable
- Follow CWE/CVSS standards for vulnerability severity assessment

## Disclaimer

Blockclonia is provided as-is without any warranty. While we take security seriously, no software is completely free from vulnerabilities. Users accept the risks associated with running this software and are responsible for securing their own installations and data.

## Questions & Contact

- **Security Issues**: security@kthread.dev
- **General Support**: See README.md for community resources
- **Feature Requests**: GitHub Issues (non-security)

---

**Last Updated**: 2026-09-27
**Policy Version**: 1.0
