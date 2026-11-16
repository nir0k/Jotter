# Security policy

## Supported versions

Only the latest release receives security fixes. Please check that a problem is still present in it before reporting.

## Reporting a vulnerability

Report vulnerabilities privately through GitHub: open the repository's **Security** tab and choose **Report a vulnerability**. Please do not open a public issue, discussion or pull request for a vulnerability.

A useful report includes the Jotter version, how it was installed, the steps or files that reproduce the problem and what an attacker gains. A reproducing library, note file or SFTP server setup helps most; remove any personal notes and credentials from it first.

Reports are answered as soon as possible. Once a fix is released, the advisory is published with credit to the reporter unless they prefer otherwise.

## Scope

Jotter handles untrusted input in several places, and problems there are in scope:

- Library files: note HTML, `library.json`, `collections.json`, `rules.json` and attachments, which may come from another device or another person.
- Synchronisation: data sent by an SFTP server, including file names, listings and file contents, host key checks and the handling of passwords, private keys and passphrases, which are kept in the system keyring.
- Pasted and dropped content: HTML, Markdown, images and files.
- Links in notes and exported files: HTML, PDF and JPG.

Out of scope are problems that need an attacker who already controls the person's account or desktop session, and vulnerabilities in the Flatpak runtime or the system libraries, which go to their own projects.
