# Running servers on ManiOS

Since 0.31.0 ManiOS programs can listen for connections, and the package
repository has servers for it (from manios-apps):

```
pkg install @servers        # sshd httpd telnetd ircd ftpd
```

## By hand

A server runs until it is stopped; start it in the background with `&`:

```
manios% httpd /n/ata0p1/www &
[12]
manios% jobs
[12] running  httpd
manios% kill 12
[12] killed  httpd
```

^C stops a server started without `&`. `cat /dev/ps` lists every
process.

## sshd

```
sshd -g /n/ata0p1/manios/etc/sshkey     # once: the host key
sshd -H > /n/ata0p1/manios/etc/ssh.pw   # a password line ...
```

`sshd -H` asks for the password twice and prints a line; put the login
name and a colon before it (`edit` the file), for example
`admin:sha256:...`. For key logins, put your `~/.ssh/id_ed25519.pub`
line into a file, as in `authorized_keys`. Then:

```
sshd -k /n/ata0p1/manios/etc/sshkey -P /n/ata0p1/manios/etc/ssh.pw -A /n/ata0p1/manios/etc/authkeys &
```

and from another machine `ssh admin@ADDRESS` (the address is in
`cat /dev/net`). sshd prints its key's fingerprint when it starts:
compare it with the one ssh shows the first time.

## At every boot

An installed system runs the disk's `manios/etc/rc` with `sh` at every
start (after `setup`). Servers started there with `&` keep running:

```
# /n/ata0p1/manios/etc/rc
sshd -k /n/ata0p1/manios/etc/sshkey -A /n/ata0p1/manios/etc/authkeys &
httpd -q /n/ata0p1/www &
```

Their log lines go to the console.

## Passwords and the network

telnetd and ftpd send passwords as they are, as the protocols do: use
them only on a network you trust, and sshd beyond it. Passwords are kept
salted and hashed (`sha256:SALT:HASH`, 4096 rounds), never as written.
Under QEMU's user network, forward the ports to reach the servers from
the host (`-nic user,hostfwd=tcp::2222-:22`).
