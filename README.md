logenqueue
==========

logenqueue is a daemon that listens for Syslog and GELF messages via UDP.
The messages are then enqueued in AMQP exchange.
The Syslog messages are converted to GELF to preserve the sending host name.


