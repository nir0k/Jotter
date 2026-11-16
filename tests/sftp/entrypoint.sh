#!/bin/sh
# Installs the optional public key for the jotter user, then runs the SSH server in the
# foreground with its log on stderr.
set -eu

if [ -n "${JOTTER_SFTP_AUTHORIZED_KEY:-}" ]; then
    mkdir -p /home/jotter/.ssh
    printf '%s\n' "$JOTTER_SFTP_AUTHORIZED_KEY" > /home/jotter/.ssh/authorized_keys
    chown -R jotter:jotter /home/jotter/.ssh
    chmod 700 /home/jotter/.ssh
    chmod 600 /home/jotter/.ssh/authorized_keys
fi

exec /usr/sbin/sshd -D -e
