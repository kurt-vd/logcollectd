# logcollectd

This is a stderr-to-syslog collector.

How does it differ from _logger (1)_ and quite many others?

I wanted to avoid creating subprocesses for every service in order
to capture their stderr and forward to syslog.

So instead, I run logcollectd once. It does not do anything alone.

For every stderr to capture, I prepend `logcollect -t SOMENAME` before
the command.
**logcollect** will create a _pipe (2)_ and send the reading end
to **logcollected** who will do the reading.
**logcollect** sets the writing end of the pipe as stdout and stderr
and _execvp (2)_ the service.

**logcollectd** reads many pipes, prefixes a syslog header, and sends the packets
to syslog directly, bypassing the libc's syslog function.
This is necessary since each pipe's data must be logged with a different tag name.
