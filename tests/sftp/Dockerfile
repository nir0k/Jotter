# An OpenSSH server with one SFTP user for the test stand. The host keys are made when the image
# is built, so they stay the same between `docker compose down` and `up`; building the image
# again with `--no-cache` gives new keys, for checking a changed host key.
FROM alpine:3.24.3

RUN apk add --no-cache openssh-server \
    && ssh-keygen -A \
    && adduser -D -s /bin/sh jotter \
    && echo 'jotter:jotter' | chpasswd \
    && mkdir -p /home/jotter/library \
    && chown jotter:jotter /home/jotter/library

COPY sshd_config /etc/ssh/sshd_config
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

EXPOSE 22
ENTRYPOINT ["/bin/sh", "/usr/local/bin/entrypoint.sh"]
