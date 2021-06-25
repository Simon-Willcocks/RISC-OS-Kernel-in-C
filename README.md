RISC OS C Kernel
================

RISC OS is a lightweight OS that was designed to run from ROM and be used by a single user. The ROMs are long gone, but the single user experience is still there. If you want an operating system to try its hardest to do what you ask it to do, when you ask it to do it, it's an excellent choice. It does, or it does not, there is no try (or being unresponsive to all user input for minutes on end before a program finally dies). 

Unfortunately, its current implementation is based on a single CPU and predominantly written in assembler.

What I am hoping this approach to achieve is more modularity, and the ability to use more than a fraction of the power of modern processors.

I want to go back to a high-level view of what the OS does, and build from there.

The kernel requires memory and an MMU.

Many functions will not work without devices and interrupts, most obviously a timer providing regular ticks.

Booting
-------

In my opinion, the existing kernel (and HAL) gets involved in device interfaces far too early. The kernel only needs processor cores, some RAM, and an MMU to start up and start looking around.

Once it's up, it can start modules than know about the specific hardware, and affect the rest of the boot sequence. (There's no longer a direct wire going to a keyboard, just finding out if you have a keyboard takes quite a few modules.)

Memory
------

My impression is that there are three areas of virtual memory in a RISC OS system:

1. High memory, where the OS and its workspaces live
2. The Relocatable Memory Area, where modules keep their data and may be soft-loaded
3. Low memory, in which Wimp tasks are switched in and out

Each core requires its own translation tables, in which high memory and the RMA entries will be almost identical, apart from areas of local workspace and the translation tables.

Devices and Interrupts
----------------------

Different systems use different mechanisms for routing interrupts to cores; ....


Modules
-------

Many existing modules can probably simply be initialised on separate cores, and provide their service without any knowledge of multi-processing.

Wimp
----

The module only executes for a fraction of the time, the rest is the applications; hopefully it will be possible to lock other cores for only short periods of time, while windows are re-arranged, or similar.


