      netdelay, a test utility for ethernet roundtrip network delay
                        (c) 2020 Andreas Steinmetz

--------------------------------------------------------------------------


  A utility to measure network roundtrip delay between processes.
===================================================================

netdelay is a test utility for network delay that is designed to give
realistic data. It is fast enough for useful data gathering but
slow enough to allow system powersave to kick in.

You can measure roundtrip delays either using layer 2 packets or
UDP or UDPLITE.

One use case is the to gather data about the latency of different
systems or different NICs. For platforms that support cpu\_dma\_latency
configuration the utility allows to set this value to estimate
system behaviour for different powersave states.

The results of such tests can be uses to estimate, if a system
or a combination of systems are suitable for (near) realtime
communication tasks, i.e. communication that has a hard upper
bound with respect to delay.

As socket priority as well as DSCP can be configured, the utility
can be used to test priority configuration of managed switches.

The utility does run in two major operation modes, initiator and
responder. One system must run the utility as a responder. The
utility must be started first on this system. The other system
runs in initiator mode and this is the the system where the
delay output is produced.

To see the configuration options run 'netdelay' without any options.
